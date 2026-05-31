#!/usr/bin/env python3
"""
AccuEnergy Monitor — visualização em tempo real dos dados do STM32H743
Recebe frames binários do ESP32 via USB Serial e plota:
  - Formas de onda de tensão e corrente
  - Espectro de harmônicas (V e I, h1..h50)
  - Métricas numéricas: Vrms, Irms, P, Q, S, FP, THD_V, THD_I

Inclui gravação de múltiplas cargas com export para HDF5 compatível com NILMTK.

Uso:
    pip install -r requirements.txt
    python monitor.py [--port COM5] [--baud 460800]
"""

import sys
import csv
import json
import struct
import pathlib
import datetime
import threading
import queue
import argparse
import numpy as np
import serial
import serial.tools.list_ports

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QGridLayout, QLabel, QComboBox, QPushButton, QStatusBar, QSplitter,
    QLineEdit, QListWidget, QListWidgetItem, QDialog, QDialogButtonBox,
    QFileDialog, QMessageBox, QGroupBox, QScrollArea, QFrame,
)
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QFont
import pyqtgraph as pg

# ── Constantes do protocolo (espelham calculo.h / main.c do STM32) ────────────
MAGIC        = bytes([0xAB, 0xCD])
FRAME_POWER  = 0x01
FRAME_HARM   = 0x02
FOOTER       = bytes([0xEF, 0xFE])

FRAME1_TOTAL   = 543
FRAME1_SAMPLES = 128
FRAME2_TOTAL   = 413
HARM_MAX       = 50

FS_HZ    = 15360
IDF_STEP = 8
F0_HZ    = 60

ADC_VREF = 3.3
ADC_MAX  = 65535.0
CAL_V    = (ADC_VREF / ADC_MAX) * 136.0
CAL_I    = (ADC_VREF / ADC_MAX) * (100.0 / 0.050 / 50.0)

T_AXIS_MS  = np.arange(FRAME1_SAMPLES) * IDF_STEP / FS_HZ * 1000.0
HARM_FREQS = np.arange(1, HARM_MAX + 1, dtype=float) * F0_HZ


# ── Parser de frames ───────────────────────────────────────────────────────────
def parse_frame1(data: bytes) -> dict | None:
    if len(data) < FRAME1_TOTAL:
        return None
    if data[0:2] != MAGIC or data[2] != FRAME_POWER:
        return None
    if data[541:543] != FOOTER:
        return None
    vrms, irms, fp, preal, q, s = struct.unpack_from('<6f', data, 3)
    n_samp = data[27]
    if n_samp != FRAME1_SAMPLES:
        return None
    raw      = struct.unpack_from(f'<{n_samp * 2}h', data, 29)
    v_counts = np.array(raw[0::2], dtype=np.float32)
    i_counts = np.array(raw[1::2], dtype=np.float32)
    return {
        'vrms': vrms, 'irms': irms, 'fp': fp,
        'preal': preal, 'q': q, 's': s,
        'v_wave': v_counts * CAL_V,
        'i_wave': i_counts * CAL_I,
    }


def parse_frame2(data: bytes) -> dict | None:
    if len(data) < FRAME2_TOTAL:
        return None
    if data[0:2] != MAGIC or data[2] != FRAME_HARM:
        return None
    if data[411:413] != FOOTER:
        return None
    thd_v, thd_i = struct.unpack_from('<2f', data, 3)
    harm_v = np.array(struct.unpack_from(f'<{HARM_MAX}f', data, 11))
    harm_i = np.array(struct.unpack_from(f'<{HARM_MAX}f', data, 11 + HARM_MAX * 4))
    return {'thd_v': thd_v, 'thd_i': thd_i, 'harm_v': harm_v, 'harm_i': harm_i}


# ── Thread de leitura serial ───────────────────────────────────────────────────
class SerialReader(threading.Thread):
    def __init__(self, port: str, baud: int, q: queue.Queue):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.q    = q
        self._stop_evt = threading.Event()

    def run(self):
        try:
            ser = serial.Serial(self.port, self.baud, timeout=0.02)
        except serial.SerialException as exc:
            self.q.put(('error', str(exc)))
            return
        buf = bytearray()
        while not self._stop_evt.is_set():
            chunk = ser.read(1024)
            if chunk:
                buf.extend(chunk)
                self._drain(buf)
        ser.close()

    def _drain(self, buf: bytearray):
        while True:
            idx = buf.find(MAGIC)
            if idx < 0:
                if len(buf) > 1:
                    del buf[:-1]
                return
            if idx > 0:
                del buf[:idx]
            if len(buf) < 3:
                return
            ftype = buf[2]
            if ftype == FRAME_POWER:
                if len(buf) < FRAME1_TOTAL:
                    return
                parsed = parse_frame1(bytes(buf[:FRAME1_TOTAL]))
                del buf[:FRAME1_TOTAL]
                if parsed:
                    self.q.put(('power', parsed))
            elif ftype == FRAME_HARM:
                if len(buf) < FRAME2_TOTAL:
                    return
                parsed = parse_frame2(bytes(buf[:FRAME2_TOTAL]))
                del buf[:FRAME2_TOTAL]
                if parsed:
                    self.q.put(('harm', parsed))
            else:
                del buf[:3]

    def stop(self):
        self._stop_evt.set()


# ── DataRecorder ───────────────────────────────────────────────────────────────
class DataRecorder:
    """
    Grava medições em segmentos nomeados e exporta para HDF5 compatível com NILMTK.

    Cada carga testada é um segmento. Ao exportar, cada segmento vira um meter
    separado no arquivo HDF5:
        /building1/elec/meter1  ← primeiro segmento
        /building1/elec/meter2  ← segundo segmento  ...

    Fluxo típico:
        rec = DataRecorder()
        rec.start(label="Ventilador")
        rec.append(ts, vrms, irms, preal, pap, prea, fp)  # chamado a cada frame
        rec.stop()                                          # finaliza segmento
        rec.start(label="Ferro de Passar")
        ...
        rec.stop()
        result = rec.export_nilmtk_hdf5("dataset.h5")
        # result = {'Ventilador': 4500, 'Ferro de Passar': 3200}

    Mapeamento → MultiIndex NILMTK:
        ('power',   'active')   ← preal  [W]
        ('power',   'apparent') ← pap    [VA]
        ('power',   'reactive') ← prea   [VAr]
        ('voltage', '')         ← vrms   [V]
        ('current', '')         ← irms   [A]
        ('power',   'factor')   ← fp
    """

    COLUMNS = [
        ('power',   'active'),
        ('power',   'apparent'),
        ('power',   'reactive'),
        ('voltage', ''),
        ('current', ''),
        ('power',   'factor'),
    ]

    CSV_HEADER = [
        'label', 'timestamp_utc',
        'preal_W', 'paparente_VA', 'preativa_VAr',
        'vrms_V', 'irms_A', 'fp',
    ]

    _METER_MSMTS = [
        {'physical_quantity': 'power',   'type': 'active',
         'lower_limit': -10000, 'upper_limit': 10000, 'unit': 'W'},
        {'physical_quantity': 'power',   'type': 'apparent',
         'lower_limit': 0,      'upper_limit': 10000, 'unit': 'VA'},
        {'physical_quantity': 'power',   'type': 'reactive',
         'lower_limit': -10000, 'upper_limit': 10000, 'unit': 'VAr'},
        {'physical_quantity': 'voltage', 'type': '',
         'lower_limit': 0,      'upper_limit': 300,   'unit': 'V'},
        {'physical_quantity': 'current', 'type': '',
         'lower_limit': 0,      'upper_limit': 100,   'unit': 'A'},
        {'physical_quantity': 'power',   'type': 'factor',
         'lower_limit': -1,     'upper_limit': 1,     'unit': ''},
    ]

    def __init__(self):
        self._segments: list[dict] = []   # segmentos finalizados
        self._active:   dict | None = None
        self._csv_file   = None
        self._csv_writer = None

    # ── Propriedades ──────────────────────────────────────────────────────────
    @property
    def recording(self) -> bool:
        return self._active is not None

    @property
    def row_count(self) -> int:
        return len(self._active['rows']) if self._active else 0

    @property
    def total_rows(self) -> int:
        n = sum(len(s['rows']) for s in self._segments)
        return n + (len(self._active['rows']) if self._active else 0)

    @property
    def segments(self) -> list[tuple[str, int]]:
        out = [(s['label'], len(s['rows'])) for s in self._segments]
        if self._active:
            out.append((self._active['label'] + ' ⏺', len(self._active['rows'])))
        return out

    @property
    def elapsed(self) -> str:
        if not self._active:
            return '00:00:00'
        delta = datetime.datetime.now(datetime.timezone.utc) - self._active['start_time']
        h, rem = divmod(int(delta.total_seconds()), 3600)
        m, s   = divmod(rem, 60)
        return f'{h:02d}:{m:02d}:{s:02d}'

    # ── Controle ──────────────────────────────────────────────────────────────
    def start(self, label: str = '', csv_path: str | None = None):
        """Inicia novo segmento. Se já há um ativo, finaliza-o antes."""
        if self._active:
            self.stop()
        label = label.strip() or f'Carga {len(self._segments) + 1}'
        self._active = {
            'label':      label,
            'rows':       [],
            'start_time': datetime.datetime.now(datetime.timezone.utc),
        }
        if csv_path and not self._csv_file:
            self._csv_file   = open(csv_path, 'w', newline='', encoding='utf-8')
            self._csv_writer = csv.writer(self._csv_file)
            self._csv_writer.writerow(self.CSV_HEADER)

    def stop(self):
        """Finaliza o segmento ativo e o move para a lista."""
        if not self._active:
            return
        if self._active['rows']:
            self._segments.append(self._active)
        self._active = None
        if self._csv_file:
            self._csv_file.flush()
            self._csv_file.close()
            self._csv_file   = None
            self._csv_writer = None

    def append(
        self,
        ts:    datetime.datetime,
        vrms:  float, irms:  float,
        preal: float, pap:   float,
        prea:  float, fp:    float,
    ):
        """Adiciona medição ao segmento ativo (no-op se não estiver gravando)."""
        if not self._active:
            return
        row = {'ts': ts, 'vrms': vrms, 'irms': irms,
               'preal': preal, 'pap': pap, 'prea': prea, 'fp': fp}
        self._active['rows'].append(row)
        if self._csv_writer:
            self._csv_writer.writerow([
                self._active['label'], ts.isoformat(),
                preal, pap, prea, vrms, irms, fp,
            ])

    def remove_last_segment(self) -> str | None:
        if self._segments:
            return self._segments.pop()['label']
        return None

    def clear_all(self):
        if self._active:
            self._active = None
            if self._csv_file:
                self._csv_file.close()
                self._csv_file   = None
                self._csv_writer = None
        self._segments.clear()

    # ── Export NILMTK ─────────────────────────────────────────────────────────
    def export_nilmtk_hdf5(
        self,
        hdf5_path:  str,
        building:   int = 1,
        timezone:   str = 'America/Sao_Paulo',
        resample_s: int | None = None,
    ) -> dict[str, int]:
        """
        Exporta todos os segmentos para HDF5 no formato NILMTK.
        Também gera <hdf5_path>.metadata.json.
        Retorna dict {label: n_linhas}.
        """
        try:
            import pandas as pd
        except ImportError:
            raise RuntimeError("pandas não instalado — execute: pip install pandas tables")

        all_segs = list(self._segments)
        if self._active and self._active['rows']:
            all_segs.append(self._active)
        if not all_segs:
            raise ValueError("Nenhum segmento gravado para exportar.")

        hdf5_path = pathlib.Path(hdf5_path)
        col_idx   = pd.MultiIndex.from_tuples(self.COLUMNS,
                                               names=['physical_quantity', 'type'])
        result      = {}
        meters_meta = {}

        with pd.HDFStore(str(hdf5_path), mode='w', complevel=5, complib='blosc') as store:
            for meter_idx, seg in enumerate(all_segs, start=1):
                df  = _rows_to_df(seg['rows'], col_idx, timezone, resample_s)
                key = f'/building{building}/elec/meter{meter_idx}'
                store.put(key, df, format='table', data_columns=True)
                result[seg['label']] = len(df)
                meters_meta[meter_idx] = {
                    'device':      'stm32_accuenergy',
                    'submeter_of': None,
                    'site_meter':  meter_idx == 1,
                    'label':       seg['label'],
                    'n_rows':      len(df),
                }

        _write_metadata_json(hdf5_path, building, meters_meta,
                             timezone, self._METER_MSMTS)
        return result


# ── Helpers de exportação ─────────────────────────────────────────────────────
def _rows_to_df(rows, col_idx, timezone, resample_s):
    import pandas as pd
    idx  = pd.DatetimeIndex([r['ts'] for r in rows], tz='UTC').tz_convert(timezone)
    data = np.array(
        [[r['preal'], r['pap'], r['prea'], r['vrms'], r['irms'], r['fp']]
         for r in rows],
        dtype=np.float32,
    )
    df = pd.DataFrame(data, index=idx, columns=col_idx)
    df.index.name = 'datetime'
    df = df[~df.index.duplicated(keep='last')]
    df.sort_index(inplace=True)
    if resample_s:
        df = df.resample(f'{resample_s}s').mean().dropna(how='all')
    return df


def _write_metadata_json(hdf5_path, building, meters, timezone, measurements):
    meta = {
        'name': 'AccuEnergy STM32',
        'long_name': 'AccuEnergy STM32H743 Power Measurement',
        'date_issued': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'timezone': timezone,
        'geo_location': {'country': 'BR', 'locality': ''},
        'buildings': {
            building: {
                'description': 'AccuEnergy multi-load dataset',
                'elec_meters': meters,
            }
        },
        'meter_devices': {
            'stm32_accuenergy': {
                'model': 'STM32H743 + SCT013-100 + ZMPT101B',
                'manufacturer': 'custom',
                'nominal_voltage': 127,
                'frequency': 60,
                'sample_period': 67,
                'measurements': measurements,
            }
        },
    }
    meta_path = pathlib.Path(hdf5_path).with_suffix('.metadata.json')
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False),
                         encoding='utf-8')


# ── Consolidação de arquivos externos ─────────────────────────────────────────
def consolidate_hdf5_files(
    inputs:      list[tuple[str, str]],
    output_path: str,
    building:    int = 1,
    timezone:    str = 'America/Sao_Paulo',
    resample_s:  int | None = None,
) -> dict[str, int]:
    """
    Mescla arquivos HDF5 exportados anteriormente em um único dataset NILMTK.

    Cada arquivo de entrada (mesmo que contenha vários meters) é lido pelo
    primeiro key disponível e mapeado para um meter no arquivo de saída.

    Args:
        inputs:      lista de (caminho_h5, label_da_carga)
        output_path: arquivo H5 consolidado de saída (sobrescrito)
        building:    índice do edifício no dataset resultante
        timezone:    fuso horário para conversão de índice
        resample_s:  reamostrar para período fixo em segundos (None = manter original)

    Returns:
        dict {label: n_linhas} para cada arquivo processado
    """
    try:
        import pandas as pd
    except ImportError:
        raise RuntimeError("pandas não instalado — execute: pip install pandas tables")

    output_path = pathlib.Path(output_path)
    col_idx     = pd.MultiIndex.from_tuples(DataRecorder.COLUMNS,
                                             names=['physical_quantity', 'type'])
    result      = {}
    meters_meta = {}

    with pd.HDFStore(str(output_path), mode='w', complevel=5, complib='blosc') as out_store:
        for meter_idx, (h5_path, label) in enumerate(inputs, start=1):
            h5_path = pathlib.Path(h5_path)
            if not h5_path.exists():
                raise FileNotFoundError(f"Arquivo não encontrado: {h5_path}")

            with pd.HDFStore(str(h5_path), mode='r') as src:
                keys = src.keys()
                if not keys:
                    continue
                df = src[keys[0]]

            if df.index.tzinfo is None:
                df.index = df.index.tz_localize('UTC').tz_convert(timezone)
            else:
                df.index = df.index.tz_convert(timezone)

            if not isinstance(df.columns, pd.MultiIndex):
                if len(df.columns) == len(DataRecorder.COLUMNS):
                    df.columns = col_idx

            df = df[~df.index.duplicated(keep='last')]
            df.sort_index(inplace=True)
            if resample_s:
                df = df.resample(f'{resample_s}s').mean().dropna(how='all')

            key = f'/building{building}/elec/meter{meter_idx}'
            out_store.put(key, df, format='table', data_columns=True)
            result[label] = len(df)
            meters_meta[meter_idx] = {
                'device':      'stm32_accuenergy',
                'submeter_of': None,
                'site_meter':  meter_idx == 1,
                'label':       label,
                'source_file': h5_path.name,
                'n_rows':      len(df),
            }

    _write_metadata_json(output_path, building, meters_meta,
                         timezone, DataRecorder._METER_MSMTS)
    return result


# ── Paleta dark ────────────────────────────────────────────────────────────────
BG_DARK  = '#1e1e2e'
BG_PANEL = '#24243e'
BG_CARD  = '#2a2a42'
C_V      = '#89b4fa'
C_I      = '#f38ba8'
C_P      = '#a6e3a1'
C_Q      = '#cba6f7'
C_S      = '#89dceb'
C_FP     = '#f9e2af'
C_THDV   = '#74c7ec'
C_THDI   = '#eba0ac'
C_HARM_V = '#a6e3a1'
C_HARM_I = '#fab387'
C_TXT    = '#cdd6f4'
C_SUB    = '#6c7086'
C_REC    = '#f38ba8'   # vermelho-rosa quando gravando

pg.setConfigOptions(antialias=True, background=BG_DARK, foreground=C_TXT)

_BTN_STYLE  = (f'background:{BG_CARD}; color:{C_TXT}; border:1px solid #45475a;'
               f'padding:5px 10px; border-radius:4px;')
_BTN_HOVER  = f'QPushButton:hover {{ background:#313244; }}'
_EDIT_STYLE = (f'background:{BG_CARD}; color:{C_TXT}; border:1px solid #45475a;'
               f'padding:4px; border-radius:4px;')


def _card_style():
    return f'background:{BG_CARD}; border-radius:8px; border:1px solid #313244;'


class MetricCard(QWidget):
    def __init__(self, title, unit, color, parent=None):
        super().__init__(parent)
        self.setStyleSheet(_card_style())
        lay = QVBoxLayout(self)
        lay.setSpacing(1)
        lay.setContentsMargins(10, 8, 10, 8)

        lbl_t = QLabel(title)
        lbl_t.setFont(QFont('Consolas', 9))
        lbl_t.setStyleSheet(f'color:{C_SUB}; border:none; background:transparent;')
        lbl_t.setAlignment(Qt.AlignCenter)

        self._lbl_val = QLabel('—')
        self._lbl_val.setFont(QFont('Consolas', 22, QFont.Bold))
        self._lbl_val.setStyleSheet(f'color:{color}; border:none; background:transparent;')
        self._lbl_val.setAlignment(Qt.AlignCenter)

        lbl_u = QLabel(unit)
        lbl_u.setFont(QFont('Consolas', 10))
        lbl_u.setStyleSheet(f'color:{C_SUB}; border:none; background:transparent;')
        lbl_u.setAlignment(Qt.AlignCenter)

        lay.addWidget(lbl_t)
        lay.addWidget(self._lbl_val)
        lay.addWidget(lbl_u)

    def set_value(self, text: str):
        self._lbl_val.setText(text)


# ── Diálogo de segmentos ───────────────────────────────────────────────────────
class SegmentsDialog(QDialog):
    """Mostra a lista de segmentos gravados com opções de remoção."""

    def __init__(self, recorder: DataRecorder, parent=None):
        super().__init__(parent)
        self.recorder = recorder
        self.setWindowTitle('Segmentos gravados')
        self.setMinimumWidth(380)
        self.setStyleSheet(f'background:{BG_DARK}; color:{C_TXT};')

        lay = QVBoxLayout(self)

        self._list = QListWidget()
        self._list.setStyleSheet(
            f'background:#181825; color:{C_TXT}; border:none;'
            f'font-family:Consolas; font-size:10px;')
        lay.addWidget(self._list)

        btn_row = QHBoxLayout()
        btn_rm  = QPushButton('✕ Remover último')
        btn_clr = QPushButton('🗑 Limpar tudo')
        btn_rm.setStyleSheet(_BTN_STYLE + _BTN_HOVER)
        btn_clr.setStyleSheet(_BTN_STYLE + _BTN_HOVER)
        btn_rm.clicked.connect(self._remove_last)
        btn_clr.clicked.connect(self._clear_all)
        btn_row.addWidget(btn_rm)
        btn_row.addWidget(btn_clr)
        lay.addLayout(btn_row)

        btns = QDialogButtonBox(QDialogButtonBox.Close)
        btns.rejected.connect(self.accept)
        btns.setStyleSheet(_BTN_STYLE)
        lay.addWidget(btns)

        self._refresh()

    def _refresh(self):
        self._list.clear()
        for i, (label, n) in enumerate(self.recorder.segments, start=1):
            active = label.endswith(' ⏺')
            lbl    = label.rstrip(' ⏺')
            tag    = '⏺ ativo' if active else f'meter{i}'
            item   = QListWidgetItem(f'  [{tag}]  {lbl:<22}  {n:>7} pts')
            item.setForeground(
                pg.mkColor(C_REC) if active else pg.mkColor(C_TXT))
            self._list.addItem(item)
        if not self.recorder.segments:
            self._list.addItem('  (nenhum segmento)')

    def _remove_last(self):
        if self.recorder.recording:
            QMessageBox.information(self, 'Gravação ativa',
                                    'Pare a gravação antes de remover o segmento.')
            return
        removed = self.recorder.remove_last_segment()
        if removed:
            self._refresh()

    def _clear_all(self):
        if QMessageBox.question(
            self, 'Limpar tudo',
            'Descartar TODOS os segmentos?\nEsta ação não pode ser desfeita.',
            QMessageBox.Yes | QMessageBox.No,
        ) == QMessageBox.Yes:
            self.recorder.clear_all()
            self._refresh()


# ── Diálogo de consolidação ────────────────────────────────────────────────────
class ConsolidateDialog(QDialog):
    """Seleciona múltiplos arquivos H5 com labels e consolida em um único dataset."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle('Consolidar arquivos HDF5')
        self.resize(560, 420)
        self.setStyleSheet(f'background:{BG_DARK}; color:{C_TXT};')

        lay = QVBoxLayout(self)

        # Instrução
        lbl = QLabel(
            'Adicione arquivos HDF5 exportados por esta GUI e atribua um label a cada um.\n'
            'Todos serão mesclados em um único dataset NILMTK.'
        )
        lbl.setStyleSheet(f'color:{C_SUB}; font-size:9px;')
        lay.addWidget(lbl)

        # Lista de arquivos
        self._list = QListWidget()
        self._list.setStyleSheet(
            f'background:#181825; color:{C_TXT}; border:none;'
            f'font-family:Consolas; font-size:9px;')
        lay.addWidget(self._list)

        self._entries: list[tuple[str, QLineEdit]] = []   # [(path, label_widget)]

        # Botões de gestão
        mgmt = QHBoxLayout()
        btn_add = QPushButton('＋ Adicionar arquivos…')
        btn_rm  = QPushButton('✕ Remover selecionado')
        btn_add.setStyleSheet(_BTN_STYLE + _BTN_HOVER)
        btn_rm.setStyleSheet(_BTN_STYLE + _BTN_HOVER)
        btn_add.clicked.connect(self._add_files)
        btn_rm.clicked.connect(self._remove_selected)
        mgmt.addWidget(btn_add)
        mgmt.addWidget(btn_rm)
        lay.addLayout(mgmt)

        # Resampling
        rs_row = QHBoxLayout()
        rs_lbl = QLabel('Resample:')
        rs_lbl.setStyleSheet(f'color:{C_SUB};')
        self._resample = QLineEdit()
        self._resample.setPlaceholderText('s (vazio=nenhum)')
        self._resample.setMaximumWidth(120)
        self._resample.setStyleSheet(_EDIT_STYLE)
        rs_row.addWidget(rs_lbl)
        rs_row.addWidget(self._resample)
        rs_row.addStretch()
        lay.addLayout(rs_row)

        # Botões OK/Cancelar
        btns = QDialogButtonBox()
        btn_go  = btns.addButton('⬇ Consolidar e salvar', QDialogButtonBox.AcceptRole)
        btn_cancel = btns.addButton('Cancelar', QDialogButtonBox.RejectRole)
        btn_go.setStyleSheet(_BTN_STYLE)
        btn_cancel.setStyleSheet(_BTN_STYLE)
        btns.accepted.connect(self._do_consolidate)
        btns.rejected.connect(self.reject)
        lay.addWidget(btns)

    def _add_files(self):
        paths, _ = QFileDialog.getOpenFileNames(
            self, 'Selecionar arquivos HDF5', '',
            'HDF5 (*.h5 *.hdf5);;Todos (*.*)',
        )
        for p in paths:
            label_w = QLineEdit(pathlib.Path(p).stem)
            label_w.setStyleSheet(_EDIT_STYLE)
            label_w.setMaximumWidth(160)
            self._entries.append((p, label_w))
        self._refresh_list()

    def _remove_selected(self):
        row = self._list.currentRow()
        if 0 <= row < len(self._entries):
            self._entries.pop(row)
            self._refresh_list()

    def _refresh_list(self):
        self._list.clear()
        for path, lw in self._entries:
            self._list.addItem(
                f'  {pathlib.Path(path).name:<30}  label: {lw.text() or "(vazio)"}')

    def _do_consolidate(self):
        if not self._entries:
            QMessageBox.warning(self, 'Sem arquivos', 'Adicione pelo menos um arquivo.')
            return

        inputs = [(p, lw.text().strip() or pathlib.Path(p).stem)
                  for p, lw in self._entries]

        rs_str     = self._resample.text().strip()
        resample_s = None
        if rs_str:
            try:
                resample_s = int(rs_str)
            except ValueError:
                QMessageBox.critical(self, 'Resample inválido',
                                     f"'{rs_str}' não é um inteiro.")
                return

        out_path, _ = QFileDialog.getSaveFileName(
            self, 'Salvar dataset consolidado', '',
            'HDF5 (*.h5 *.hdf5);;Todos (*.*)',
        )
        if not out_path:
            return

        try:
            result = consolidate_hdf5_files(inputs, out_path, resample_s=resample_s)
            lines  = [f'  meter{i+1}: {lbl} ({n} pts)'
                      for i, (lbl, n) in enumerate(result.items())]
            QMessageBox.information(
                self, 'Consolidação concluída',
                f'{len(result)} arquivo(s) consolidados:\n' + '\n'.join(lines) +
                f'\n\nSaída: {out_path}',
            )
            self.accept()
        except Exception as exc:
            QMessageBox.critical(self, 'Erro', str(exc))


# ── Janela principal ───────────────────────────────────────────────────────────
class MainWindow(QMainWindow):

    def __init__(self, args):
        super().__init__()
        self.setWindowTitle('AccuEnergy Monitor — STM32 Power Analyzer')
        self.resize(1400, 900)
        self.setStyleSheet(
            f'QMainWindow  {{ background:{BG_DARK}; }}'
            f'QWidget       {{ background:{BG_DARK}; color:{C_TXT}; }}'
            f'QLabel        {{ background:transparent; }}'
            f'QComboBox     {{ background:{BG_CARD}; color:{C_TXT};'
            f'                border:1px solid #45475a; padding:4px; }}'
            f'QPushButton   {{ {_BTN_STYLE} }}'
            f'QPushButton:hover {{ background:#313244; }}'
            f'QLineEdit     {{ {_EDIT_STYLE} }}'
            f'QStatusBar    {{ background:{BG_PANEL}; color:{C_SUB}; }}'
        )

        self._q           = queue.Queue()
        self._reader: SerialReader | None = None
        self._frame_count = 0
        self._connected   = False
        self._recorder    = DataRecorder()
        self._csv_session_path: str | None = None

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setSpacing(5)
        root.setContentsMargins(8, 8, 8, 4)

        root.addLayout(self._build_toolbar())
        root.addWidget(self._build_recorder_bar())
        root.addLayout(self._build_metrics())
        root.addWidget(self._build_plots(), stretch=1)

        self._status = QStatusBar()
        self._status.setFixedHeight(24)
        self.setStatusBar(self._status)
        self._status.showMessage('Desconectado — selecione a porta e clique Conectar')

        self._poll_timer = QTimer(self)
        self._poll_timer.timeout.connect(self._poll_queue)
        self._poll_timer.start(30)

        self._rec_ticker = QTimer(self)
        self._rec_ticker.timeout.connect(self._tick_recording)
        self._rec_ticker.start(1000)

        if args.port:
            self._set_port(args.port)
            self._do_connect()

    # ── Toolbar (porta / baud / conectar) ────────────────────────────────────
    def _build_toolbar(self) -> QHBoxLayout:
        lay = QHBoxLayout()
        lay.setSpacing(8)

        self._port_combo = QComboBox()
        self._port_combo.setMinimumWidth(200)
        self._baud_combo = QComboBox()
        for b in ('460800', '921600', '115200'):
            self._baud_combo.addItem(b)
        self._baud_combo.setMinimumWidth(90)

        btn_refresh = QPushButton('⟳ Portas')
        btn_refresh.setFixedWidth(90)
        btn_refresh.clicked.connect(self._refresh_ports)

        self._btn_connect = QPushButton('Conectar')
        self._btn_connect.setFixedWidth(100)
        self._btn_connect.clicked.connect(self._toggle_connect)

        for lbl_txt, widget in (('Porta:', self._port_combo),
                                 ('Baud:',  self._baud_combo)):
            lbl = QLabel(lbl_txt)
            lbl.setStyleSheet(f'color:{C_SUB};')
            lay.addWidget(lbl)
            lay.addWidget(widget)
        lay.addWidget(btn_refresh)
        lay.addWidget(self._btn_connect)
        lay.addStretch()

        self._refresh_ports()
        return lay

    def _refresh_ports(self):
        self._port_combo.clear()
        for p in serial.tools.list_ports.comports():
            desc = f'{p.device} — {p.description}' if p.description != 'n/a' else p.device
            self._port_combo.addItem(desc, p.device)
        if self._port_combo.count() == 0:
            self._port_combo.addItem('(nenhuma porta encontrada)', '')

    def _set_port(self, port: str):
        for i in range(self._port_combo.count()):
            if self._port_combo.itemData(i) == port:
                self._port_combo.setCurrentIndex(i)
                return

    # ── Barra de gravação ─────────────────────────────────────────────────────
    def _build_recorder_bar(self) -> QFrame:
        """Retorna um QFrame estilizado com todos os controles de gravação."""
        frame = QFrame()
        frame.setStyleSheet(
            f'QFrame {{ background:{BG_PANEL}; border-radius:6px;'
            f'          border:1px solid #313244; padding:2px; }}'
        )
        frame.setFixedHeight(42)

        lay = QHBoxLayout(frame)
        lay.setSpacing(8)
        lay.setContentsMargins(10, 4, 10, 4)

        # Nome da carga
        lbl_carga = QLabel('Carga:')
        lbl_carga.setStyleSheet(f'color:{C_SUB}; border:none; background:transparent;')
        self._rec_label = QLineEdit()
        self._rec_label.setPlaceholderText('nome da carga…')
        self._rec_label.setFixedWidth(160)

        # CSV opcional
        btn_csv = QPushButton('CSV…')
        btn_csv.setFixedWidth(55)
        btn_csv.setToolTip('Escolher arquivo CSV para gravar em streaming')
        btn_csv.clicked.connect(self._browse_csv)

        # Gravar / Parar
        self._btn_rec = QPushButton('⏺  Gravar')
        self._btn_rec.setFixedWidth(100)
        self._btn_rec.clicked.connect(self._toggle_recording)

        # Status / contador
        self._rec_status = QLabel('● Parado')
        self._rec_status.setFont(QFont('Consolas', 9))
        self._rec_status.setStyleSheet(
            f'color:{C_SUB}; border:none; background:transparent; min-width:180px;')

        # Segmentos
        self._btn_segs = QPushButton('Segmentos: 0')
        self._btn_segs.setFixedWidth(130)
        self._btn_segs.setToolTip('Ver / remover segmentos gravados')
        self._btn_segs.clicked.connect(self._open_segments_dialog)

        # Resample
        lbl_rs = QLabel('Resample:')
        lbl_rs.setStyleSheet(f'color:{C_SUB}; border:none; background:transparent;')
        self._rec_resample = QLineEdit()
        self._rec_resample.setPlaceholderText('s')
        self._rec_resample.setFixedWidth(48)
        self._rec_resample.setToolTip('Reamostrar para período fixo antes de exportar (em segundos)')

        # Export
        self._btn_export = QPushButton('⬇ Export (0 seg.)')
        self._btn_export.setFixedWidth(140)
        self._btn_export.clicked.connect(self._export_nilmtk)

        # Consolidar
        btn_cons = QPushButton('🗂 Consolidar…')
        btn_cons.setFixedWidth(120)
        btn_cons.setToolTip('Mesclar arquivos HDF5 de sessões anteriores')
        btn_cons.clicked.connect(self._open_consolidate_dialog)

        for w in (lbl_carga, self._rec_label, btn_csv, self._btn_rec,
                  self._rec_status, self._btn_segs, lbl_rs, self._rec_resample,
                  self._btn_export, btn_cons):
            lay.addWidget(w)
        lay.addStretch()
        return frame

    # ── Métricas numéricas ────────────────────────────────────────────────────
    def _build_metrics(self) -> QGridLayout:
        lay = QGridLayout()
        lay.setSpacing(6)
        self._cards: dict[str, MetricCard] = {}
        defs = [
            ('Vrms',  'V',   C_V,    0, 0),
            ('Irms',  'A',   C_I,    0, 1),
            ('P',     'W',   C_P,    0, 2),
            ('Q',     'VAR', C_Q,    0, 3),
            ('S',     'VA',  C_S,    0, 4),
            ('FP',    '',    C_FP,   0, 5),
            ('THD_V', '%',   C_THDV, 0, 6),
            ('THD_I', '%',   C_THDI, 0, 7),
        ]
        for key, unit, color, row, col in defs:
            card = MetricCard(key, unit, color)
            card.setMinimumHeight(90)
            lay.addWidget(card, row, col)
            self._cards[key] = card
        return lay

    # ── Gráficos ──────────────────────────────────────────────────────────────
    def _build_plots(self) -> QSplitter:
        splitter_h = QSplitter(Qt.Horizontal)
        splitter_h.setHandleWidth(4)
        splitter_h.setStyleSheet('QSplitter::handle { background: #313244; }')

        wave_widget = pg.GraphicsLayoutWidget()
        wave_widget.setBackground(BG_DARK)

        self._plt_v = wave_widget.addPlot(row=0, col=0)
        self._plt_v.setTitle('<b>Tensão — V(t)</b>', color=C_V, size='11pt')
        self._plt_v.setLabel('left', 'Tensão (V)')
        self._plt_v.setLabel('bottom', 'Tempo (ms)')
        self._plt_v.showGrid(x=True, y=True, alpha=0.2)
        self._plt_v.addLegend(offset=(10, 10))
        self._curve_v = self._plt_v.plot(pen=pg.mkPen(C_V, width=2), name='V(t)')
        self._line_vrms_pos = pg.InfiniteLine(
            angle=0, pen=pg.mkPen(C_V, width=1, style=Qt.DashLine))
        self._line_vrms_neg = pg.InfiniteLine(
            angle=0, pen=pg.mkPen(C_V, width=1, style=Qt.DashLine))
        self._plt_v.addItem(self._line_vrms_pos)
        self._plt_v.addItem(self._line_vrms_neg)

        self._plt_i = wave_widget.addPlot(row=1, col=0)
        self._plt_i.setTitle('<b>Corrente — I(t)</b>', color=C_I, size='11pt')
        self._plt_i.setLabel('left', 'Corrente (A)')
        self._plt_i.setLabel('bottom', 'Tempo (ms)')
        self._plt_i.showGrid(x=True, y=True, alpha=0.2)
        self._plt_i.addLegend(offset=(10, 10))
        self._curve_i = self._plt_i.plot(pen=pg.mkPen(C_I, width=2), name='I(t)')
        self._line_irms_pos = pg.InfiniteLine(
            angle=0, pen=pg.mkPen(C_I, width=1, style=Qt.DashLine))
        self._line_irms_neg = pg.InfiniteLine(
            angle=0, pen=pg.mkPen(C_I, width=1, style=Qt.DashLine))
        self._plt_i.addItem(self._line_irms_pos)
        self._plt_i.addItem(self._line_irms_neg)
        self._plt_i.setXLink(self._plt_v)

        harm_widget = pg.GraphicsLayoutWidget()
        harm_widget.setBackground(BG_DARK)
        bar_w = (HARM_FREQS[1] - HARM_FREQS[0]) * 0.6

        self._plt_hv = harm_widget.addPlot(row=0, col=0)
        self._plt_hv.setTitle('<b>Harmônicas de Tensão</b>', color=C_HARM_V, size='11pt')
        self._plt_hv.setLabel('left', 'Magnitude (V pico)')
        self._plt_hv.setLabel('bottom', 'Frequência (Hz)')
        self._plt_hv.showGrid(x=False, y=True, alpha=0.2)
        self._bars_hv = pg.BarGraphItem(
            x=HARM_FREQS, height=np.zeros(HARM_MAX), width=bar_w,
            brush=pg.mkBrush(C_HARM_V + 'cc'), pen=pg.mkPen(C_HARM_V, width=1))
        self._plt_hv.addItem(self._bars_hv)
        self._lbl_fund_v = pg.TextItem('h1', color=C_HARM_V, anchor=(0.5, 1.0))
        self._plt_hv.addItem(self._lbl_fund_v)

        self._plt_hi = harm_widget.addPlot(row=1, col=0)
        self._plt_hi.setTitle('<b>Harmônicas de Corrente</b>', color=C_HARM_I, size='11pt')
        self._plt_hi.setLabel('left', 'Magnitude (A pico)')
        self._plt_hi.setLabel('bottom', 'Frequência (Hz)')
        self._plt_hi.showGrid(x=False, y=True, alpha=0.2)
        self._bars_hi = pg.BarGraphItem(
            x=HARM_FREQS, height=np.zeros(HARM_MAX), width=bar_w,
            brush=pg.mkBrush(C_HARM_I + 'cc'), pen=pg.mkPen(C_HARM_I, width=1))
        self._plt_hi.addItem(self._bars_hi)
        self._lbl_fund_i = pg.TextItem('h1', color=C_HARM_I, anchor=(0.5, 1.0))
        self._plt_hi.addItem(self._lbl_fund_i)

        # Ranges iniciais — evita "Cannot set range [nan, nan]" em plots vazios
        self._plt_v.setXRange(0, T_AXIS_MS[-1], padding=0)
        self._plt_v.setYRange(-250, 250, padding=0)
        self._plt_i.setXRange(0, T_AXIS_MS[-1], padding=0)
        self._plt_i.setYRange(-20, 20, padding=0)
        self._plt_hv.setXRange(0, HARM_FREQS[-1] + F0_HZ, padding=0)
        self._plt_hv.setYRange(0, 1, padding=0)
        self._plt_hi.setXRange(0, HARM_FREQS[-1] + F0_HZ, padding=0)
        self._plt_hi.setYRange(0, 1, padding=0)

        splitter_h.addWidget(wave_widget)
        splitter_h.addWidget(harm_widget)
        splitter_h.setSizes([700, 700])
        return splitter_h

    # ── Conexão serial ────────────────────────────────────────────────────────
    def _toggle_connect(self):
        if self._connected:
            self._do_disconnect()
        else:
            self._do_connect()

    def _do_connect(self):
        port = self._port_combo.currentData() or self._port_combo.currentText().split(' ')[0]
        baud = int(self._baud_combo.currentText())
        if not port:
            self._status.showMessage('Nenhuma porta selecionada')
            return
        self._reader = SerialReader(port, baud, self._q)
        self._reader.start()
        self._connected   = True
        self._frame_count = 0
        self._btn_connect.setText('Desconectar')
        self._status.showMessage(f'Conectado: {port} @ {baud} baud')

    def _do_disconnect(self):
        if self._reader:
            self._reader.stop()
            self._reader = None
        self._connected = False
        self._btn_connect.setText('Conectar')
        self._status.showMessage('Desconectado')

    # ── Poll da queue ─────────────────────────────────────────────────────────
    def _poll_queue(self):
        got_power = False
        try:
            while True:
                kind, data = self._q.get_nowait()
                if kind == 'power':
                    self._apply_power(data)
                    got_power = True
                elif kind == 'harm':
                    self._apply_harm(data)
                elif kind == 'error':
                    self._status.showMessage(f'Erro serial: {data}')
                    self._do_disconnect()
        except queue.Empty:
            pass

        if got_power:
            self._frame_count += 1
            self._status.showMessage(
                f'Conectado — frames:{self._frame_count}  '
                f'Vrms={self._cards["Vrms"]._lbl_val.text()} V  '
                f'Irms={self._cards["Irms"]._lbl_val.text()} A  '
                f'P={self._cards["P"]._lbl_val.text()} W  '
                f'FP={self._cards["FP"]._lbl_val.text()}'
                + (f'  ⏺ REC:{self._recorder.row_count}pts'
                   if self._recorder.recording else '')
            )

    def _apply_power(self, d: dict):
        self._cards['Vrms'].set_value(f"{d['vrms']:.1f}")
        self._cards['Irms'].set_value(f"{d['irms']:.3f}")
        self._cards['P'].set_value(f"{d['preal']:.1f}")
        self._cards['Q'].set_value(f"{d['q']:.1f}")
        self._cards['S'].set_value(f"{d['s']:.1f}")
        self._cards['FP'].set_value(f"{d['fp']:.3f}")

        self._curve_v.setData(T_AXIS_MS, d['v_wave'])
        self._curve_i.setData(T_AXIS_MS, d['i_wave'])

        vrms_pk = d['vrms'] * np.sqrt(2.0)
        irms_pk = d['irms'] * np.sqrt(2.0)
        self._line_vrms_pos.setPos( vrms_pk)
        self._line_vrms_neg.setPos(-vrms_pk)
        self._line_irms_pos.setPos( irms_pk)
        self._line_irms_neg.setPos(-irms_pk)

        # ── Alimenta o gravador ───────────────────────────────────────────────
        self._recorder.append(
            ts    = datetime.datetime.now(datetime.timezone.utc),
            vrms  = d['vrms'],  irms  = d['irms'],
            preal = d['preal'], pap   = d['s'],
            prea  = d['q'],     fp    = d['fp'],
        )

    def _apply_harm(self, d: dict):
        self._cards['THD_V'].set_value(f"{d['thd_v'] * 100.0:.1f}")
        self._cards['THD_I'].set_value(f"{d['thd_i'] * 100.0:.1f}")

        self._bars_hv.setOpts(height=d['harm_v'])
        self._bars_hi.setOpts(height=d['harm_i'])

        if d['harm_v'][0] > 0:
            self._lbl_fund_v.setText(f"h1={d['harm_v'][0]:.1f}V")
            self._lbl_fund_v.setPos(HARM_FREQS[0], d['harm_v'][0])
        if d['harm_i'][0] > 0:
            self._lbl_fund_i.setText(f"h1={d['harm_i'][0]:.3f}A")
            self._lbl_fund_i.setPos(HARM_FREQS[0], d['harm_i'][0])

    # ── Gravação — controles ──────────────────────────────────────────────────
    def _browse_csv(self):
        path, _ = QFileDialog.getSaveFileName(
            self, 'Salvar CSV da sessão', '',
            'CSV (*.csv);;Todos (*.*)',
        )
        if path:
            self._csv_session_path = path

    def _toggle_recording(self):
        if self._recorder.recording:
            n = self._recorder.row_count
            self._recorder.stop()
            self._btn_rec.setText('⏺  Gravar')
            self._btn_rec.setStyleSheet('')
            self._rec_status.setText('● Parado')
            self._rec_status.setStyleSheet(
                f'color:{C_SUB}; border:none; background:transparent;')
        else:
            label    = self._rec_label.text().strip()
            csv_path = self._csv_session_path
            self._csv_session_path = None
            self._recorder.start(label=label, csv_path=csv_path)
            active_label = self._recorder._active['label']
            self._btn_rec.setText('⏹  Parar')
            self._btn_rec.setStyleSheet(
                f'background:#3d1f2e; color:{C_REC}; border:1px solid {C_REC};'
                f'padding:5px 10px; border-radius:4px;')
            self._rec_status.setText(f'⏺ {active_label} …')
            self._rec_status.setStyleSheet(
                f'color:{C_REC}; border:none; background:transparent;')

    def _tick_recording(self):
        """Atualiza o contador de amostras e os rótulos dos botões (1 Hz)."""
        if self._recorder.recording:
            n   = self._recorder.row_count
            ela = self._recorder.elapsed
            lbl = self._recorder._active['label']
            self._rec_status.setText(f'⏺ {lbl}  {n} pts | {ela}')

        n_segs = len(self._recorder.segments)
        self._btn_segs.setText(f'Segmentos: {n_segs}')
        n_committed = len(self._recorder._segments)
        self._btn_export.setText(f'⬇ Export ({n_committed} seg.)')

    # ── Diálogos ──────────────────────────────────────────────────────────────
    def _open_segments_dialog(self):
        dlg = SegmentsDialog(self._recorder, parent=self)
        dlg.exec_()

    def _open_consolidate_dialog(self):
        dlg = ConsolidateDialog(parent=self)
        dlg.exec_()

    def _export_nilmtk(self):
        if self._recorder.total_rows == 0:
            QMessageBox.warning(self, 'Sem dados',
                                'Nenhuma amostra gravada.\n'
                                'Inicie pelo menos uma gravação.')
            return

        hdf5_path, _ = QFileDialog.getSaveFileName(
            self, 'Salvar dataset NILMTK', '',
            'HDF5 (*.h5 *.hdf5);;Todos (*.*)',
        )
        if not hdf5_path:
            return

        resample_s = self._parse_resample()
        if resample_s is False:
            return

        try:
            result = self._recorder.export_nilmtk_hdf5(
                hdf5_path, building=1, resample_s=resample_s)
            lines = [f'  meter{i+1}: {lbl} ({n} pts)'
                     for i, (lbl, n) in enumerate(result.items())]
            meta  = pathlib.Path(hdf5_path).with_suffix('.metadata.json')
            QMessageBox.information(
                self, 'Export concluído',
                f'{len(result)} segmento(s) exportados:\n' + '\n'.join(lines) +
                f'\n\nArquivo:    {hdf5_path}'
                f'\nMetadados:  {meta}'
                f'\n\nPara carregar no NILMTK:\n'
                f'  from nilmtk import DataSet\n'
                f"  ds = DataSet(r'{pathlib.Path(hdf5_path).name}')\n"
                f'  elec = ds.buildings[1].elec',
            )
        except Exception as exc:
            QMessageBox.critical(self, 'Erro ao exportar', str(exc))

    def _parse_resample(self):
        """Retorna int, None, ou False (inválido — já exibe erro)."""
        rs = self._rec_resample.text().strip()
        if not rs:
            return None
        try:
            return int(rs)
        except ValueError:
            QMessageBox.critical(self, 'Resample inválido',
                                 f"'{rs}' não é um inteiro.")
            return False

    # ── Fechar ────────────────────────────────────────────────────────────────
    def closeEvent(self, event):
        self._do_disconnect()
        event.accept()


# ── Entry point ────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description='AccuEnergy Monitor — visualizador serial STM32')
    parser.add_argument('--port', default='',
                        help='Porta serial (ex: COM5, /dev/ttyUSB0)')
    parser.add_argument('--baud', default='460800',
                        help='Baud rate (padrão: 460800)')
    args = parser.parse_args()

    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    win = MainWindow(args)
    win.show()
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()
