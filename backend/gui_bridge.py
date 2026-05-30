"""
GUI para STM32_UART_Bridge.

Protocolo ESP32 -> Host (115200 baud, 8N1):
  METER,Vrms[V],Irms[A],Preal[W],Paparente[VA],Preativa[VAr],FP\r\n
  # linhas de debug comecam com '#'

Estrutura do pacote binario STM32->ESP32 (28 bytes, 230400 baud):
  uint16 sync=0xAA55 | float Vrms | float Irms | float Preal |
  float Paparente | float Preativa | float FP | uint16 checksum
"""

import sys
import csv
import json
import time
import math
import queue
import pathlib
import datetime
import threading
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from collections import deque

import numpy as np
import serial
import serial.tools.list_ports
import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

SERIAL_BAUD = 115200
HISTORY_LEN = 300


# ── DataRecorder ───────────────────────────────────────────────────────────────
class DataRecorder:
    """
    Grava medições em CSV (streaming) e exporta para HDF5 compatível com NILMTK.

    Suporta múltiplos segmentos nomeados na mesma sessão: cada carga testada
    é um segmento independente e vira um meter separado no HDF5 exportado.

    Fluxo típico (múltiplas cargas):
        rec = DataRecorder()

        rec.start(label="Ventilador")
        # ... recebe pacotes METER ...
        rec.stop()                     # salva segmento "Ventilador"

        rec.start(label="Ferro de Passar")
        # ...
        rec.stop()

        result = rec.export_nilmtk_hdf5("dataset.h5")
        # → /building1/elec/meter1  (Ventilador)
        # → /building1/elec/meter2  (Ferro de Passar)

    Mapeamento de colunas → MultiIndex NILMTK:
        ('power',   'active')   ← Preal     [W]
        ('power',   'apparent') ← Paparente [VA]
        ('power',   'reactive') ← Preativa  [VAr]
        ('voltage', '')         ← Vrms      [V]
        ('current', '')         ← Irms      [A]
        ('power',   'factor')   ← FP
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
        # Segmentos finalizados: list[{label, rows, start_time}]
        self._segments: list[dict] = []
        # Segmento em gravação no momento
        self._active: dict | None  = None
        self._csv_file   = None
        self._csv_writer = None

    # ── Propriedades ──────────────────────────────────────────────────────────
    @property
    def recording(self) -> bool:
        return self._active is not None

    @property
    def row_count(self) -> int:
        """Amostras do segmento ativo."""
        return len(self._active['rows']) if self._active else 0

    @property
    def total_rows(self) -> int:
        """Total de amostras em todos os segmentos (ativos + finalizados)."""
        n = sum(len(s['rows']) for s in self._segments)
        if self._active:
            n += len(self._active['rows'])
        return n

    @property
    def segments(self) -> list[tuple[str, int]]:
        """Lista de (label, n_amostras) para exibição na UI."""
        out = [(s['label'], len(s['rows'])) for s in self._segments]
        if self._active:
            out.append((self._active['label'] + ' ⏺', len(self._active['rows'])))
        return out

    @property
    def elapsed(self) -> str:
        """Tempo decorrido do segmento ativo (HH:MM:SS)."""
        if not self._active:
            return '00:00:00'
        delta = datetime.datetime.now(datetime.timezone.utc) - self._active['start_time']
        h, rem = divmod(int(delta.total_seconds()), 3600)
        m, s   = divmod(rem, 60)
        return f'{h:02d}:{m:02d}:{s:02d}'

    # ── Controle de gravação ──────────────────────────────────────────────────
    def start(self, label: str = '', csv_path: str | None = None):
        """
        Inicia novo segmento de gravação. Se já houver um ativo, o finaliza
        antes de começar o novo (permite encadear sem chamar stop()).

        Args:
            label:    Nome da carga (ex: "Ventilador 50W"). Se vazio, gera
                      "Carga N" automaticamente.
            csv_path: Se fornecido, grava também em CSV incremental. O CSV
                      acumula todos os segmentos da sessão (coluna 'label').
        """
        if self._active:
            self.stop()
        label = label.strip() or f'Carga {len(self._segments) + 1}'
        self._active = {
            'label': label,
            'rows': [],
            'start_time': datetime.datetime.now(datetime.timezone.utc),
        }
        if csv_path and not self._csv_file:
            self._csv_file = open(csv_path, 'w', newline='', encoding='utf-8')
            self._csv_writer = csv.writer(self._csv_file)
            self._csv_writer.writerow(self.CSV_HEADER)

    def stop(self):
        """Finaliza o segmento ativo e o move para a lista de segmentos."""
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

    # ── Gestão de segmentos ───────────────────────────────────────────────────
    def remove_last_segment(self) -> str | None:
        """Remove e retorna o label do último segmento finalizado."""
        if self._segments:
            return self._segments.pop()['label']
        return None

    def clear_all(self):
        """Descarta todos os segmentos (finalizados e ativo)."""
        if self._active:
            self._active = None
            if self._csv_file:
                self._csv_file.close()
                self._csv_file   = None
                self._csv_writer = None
        self._segments.clear()

    # ── Exportação NILMTK ─────────────────────────────────────────────────────
    def export_nilmtk_hdf5(
        self,
        hdf5_path:  str,
        building:   int = 1,
        timezone:   str = 'America/Sao_Paulo',
        resample_s: int | None = None,
    ) -> dict[str, int]:
        """
        Exporta todos os segmentos para HDF5 no formato NILMTK.

        Cada segmento → um meter:
            /building{N}/elec/meter1  ← primeiro segmento
            /building{N}/elec/meter2  ← segundo segmento
            ...

        Também gera <hdf5_path>.metadata.json com metadados de todos os
        meters prontos para conversão em YAML pelo nilmtk-contrib.

        Args:
            hdf5_path:  Arquivo .h5 de saída (sobrescrito).
            building:   Índice do edifício (padrão 1).
            timezone:   Fuso horário local das medições.
            resample_s: Reamostrar para período fixo em segundos, se fornecido.

        Returns:
            dict {label: n_linhas_exportadas} para cada segmento.

        Raises:
            RuntimeError: pandas/tables não instalados.
            ValueError:   nenhum dado gravado.
        """
        try:
            import pandas as pd
        except ImportError:
            raise RuntimeError("pandas não instalado — execute: pip install pandas tables")

        # Inclui segmento ativo se tiver dados (exportação sem parar a gravação)
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
                df = _rows_to_df(seg['rows'], col_idx, timezone, resample_s)
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

        _write_metadata_json(hdf5_path, building, meters_meta, timezone, self._METER_MSMTS)
        return result


# ── Helpers internos ───────────────────────────────────────────────────────────
def _rows_to_df(rows: list[dict], col_idx, timezone: str, resample_s: int | None):
    """Converte lista de dicts de medição em DataFrame NILMTK."""
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


def _write_metadata_json(
    hdf5_path: pathlib.Path,
    building:  int,
    meters:    dict,
    timezone:  str,
    measurements: list,
):
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
    meta_path = hdf5_path.with_suffix('.metadata.json')
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding='utf-8')


# ── Consolidação de arquivos externos ─────────────────────────────────────────
def consolidate_hdf5_files(
    inputs:     list[tuple[str, str]],
    output_path: str,
    building:   int = 1,
    timezone:   str = 'America/Sao_Paulo',
    resample_s: int | None = None,
) -> dict[str, int]:
    """
    Mescla arquivos HDF5 exportados anteriormente em um único dataset NILMTK.

    Cada arquivo de entrada vira um meter separado no HDF5 de saída,
    independentemente de quantos meters tinha o arquivo original.

    Args:
        inputs:      Lista de (caminho_h5, label_da_carga). O label é o nome
                     que aparecerá nos metadados e na interface.
        output_path: Arquivo H5 consolidado de saída (sobrescrito).
        building:    Índice do edifício no dataset resultante.
        timezone:    Fuso horário para conversão de índice.
        resample_s:  Reamostrar para período fixo em segundos (None = não reamostrar).

    Returns:
        dict {label: n_linhas} para cada arquivo de entrada processado.

    Exemplo:
        result = consolidate_hdf5_files(
            inputs=[
                ("ventilador.h5",     "Ventilador"),
                ("ferro.h5",          "Ferro de Passar"),
                ("geladeira.h5",      "Geladeira"),
            ],
            output_path="dataset_consolidado.h5",
            resample_s=1,
        )
        # → dataset com /building1/elec/meter1..3

    Raises:
        RuntimeError: pandas/tables não instalados.
        FileNotFoundError: algum arquivo de entrada não existe.
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
                # Usa o primeiro meter disponível no arquivo de origem
                df = src[keys[0]]

            # Garante timezone correto
            if df.index.tzinfo is None:
                df.index = df.index.tz_localize('UTC').tz_convert(timezone)
            else:
                df.index = df.index.tz_convert(timezone)

            # Garante MultiIndex de colunas correto (compatibilidade com exports antigos)
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

    _write_metadata_json(output_path, building, meters_meta, timezone,
                         DataRecorder._METER_MSMTS)
    return result


class BridgeGui:
    def __init__(self, root):
        self.root = root
        self.root.title("Medidor AC — STM32 UART Bridge")
        self.root.geometry("1340x840")
        self.root.minsize(1040, 680)

        self.serial_port = None
        self.is_reading  = False
        self.read_thread = None
        self._data_queue = queue.SimpleQueue()

        self._hist = {k: deque(maxlen=HISTORY_LEN)
                      for k in ("Vrms", "Irms", "Preal", "Paparente", "Preativa", "FP")}
        self._pkt_ok  = 0
        self._pkt_err = 0
        self._hz_win  = deque(maxlen=20)
        self._recorder = DataRecorder()

        self._setup_style()
        self._setup_ui()
        self.update_ports()
        self.root.after(50, self._poll_queue)

    # ── Estilo ────────────────────────────────────────────────────────────────
    def _setup_style(self):
        s = ttk.Style()
        s.theme_use("clam")
        s.configure("TFrame",      background="#1e1e2e")
        s.configure("TLabel",      background="#1e1e2e", foreground="#cdd6f4")
        s.configure("TLabelframe", background="#1e1e2e", foreground="#89b4fa")
        s.configure("TLabelframe.Label", background="#1e1e2e", foreground="#89b4fa",
                    font=("Segoe UI", 9, "bold"))
        s.configure("TButton",     background="#313244", foreground="#cdd6f4",
                    relief="flat", padding=4)
        s.configure("TEntry",      fieldbackground="#313244", foreground="#cdd6f4",
                    insertcolor="#cdd6f4")
        s.configure("TCombobox",   fieldbackground="#313244", foreground="#cdd6f4")
        s.map("TButton",           background=[("active", "#45475a")])
        s.configure("TNotebook",   background="#1e1e2e")
        s.configure("TNotebook.Tab", background="#313244", foreground="#cdd6f4",
                    padding=[10, 4])
        s.map("TNotebook.Tab",     background=[("selected", "#45475a")])
        self.root.configure(bg="#1e1e2e")

    # ── Layout ────────────────────────────────────────────────────────────────
    def _setup_ui(self):
        self.root.columnconfigure(1, weight=1)
        self.root.rowconfigure(0, weight=1)

        left = ttk.Frame(self.root, padding=(10, 10), width=295)
        left.grid(row=0, column=0, sticky="nswe")
        left.grid_propagate(False)
        self._build_conn_panel(left)
        self._build_stats_panel(left)
        self._build_record_panel(left)
        self._build_log_panel(left)

        right = ttk.Frame(self.root, padding=(6, 10))
        right.grid(row=0, column=1, sticky="nswe")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(1, weight=1)
        self._build_metrics_panel(right)
        self._build_notebook(right)

    # ── Painel de gravação / export NILMTK ───────────────────────────────────
    def _build_record_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Gravação / Export NILMTK", padding=(8, 6))
        frm.pack(fill=tk.X, pady=(0, 8))
        frm.columnconfigure(1, weight=1)

        # ── Linha 0: nome da carga ────────────────────────────────────────────
        ttk.Label(frm, text="Carga:", font=("Segoe UI", 8)).grid(
            row=0, column=0, sticky="w")
        self._label_var = tk.StringVar(value="")
        ttk.Entry(frm, textvariable=self._label_var, width=13).grid(
            row=0, column=1, sticky="we", padx=3)
        ttk.Button(frm, text="CSV…", width=5,
                   command=self._browse_csv).grid(row=0, column=2)

        # ── Linha 1: botão gravar ─────────────────────────────────────────────
        self._btn_rec = ttk.Button(frm, text="⏺ Gravar", command=self._toggle_recording)
        self._btn_rec.grid(row=1, column=0, columnspan=3, sticky="we", pady=(4, 2))

        # ── Linha 2: status ticker ────────────────────────────────────────────
        self._rec_status_var = tk.StringVar(value="● Parado")
        self._lbl_rec = ttk.Label(
            frm, textvariable=self._rec_status_var,
            font=("Courier New", 7, "bold"), foreground="#585b70")
        self._lbl_rec.grid(row=2, column=0, columnspan=3, sticky="w")

        ttk.Separator(frm, orient=tk.HORIZONTAL).grid(
            row=3, column=0, columnspan=3, sticky="ew", pady=5)

        # ── Linha 4: lista de segmentos ───────────────────────────────────────
        ttk.Label(frm, text="Segmentos gravados:", font=("Segoe UI", 8)).grid(
            row=4, column=0, columnspan=3, sticky="w")

        lb_frm = ttk.Frame(frm)
        lb_frm.grid(row=5, column=0, columnspan=3, sticky="nswe", pady=(2, 0))
        lb_frm.columnconfigure(0, weight=1)

        self._seg_listbox = tk.Listbox(
            lb_frm, height=5, bg="#181825", fg="#cdd6f4",
            font=("Courier New", 7), relief="flat",
            selectbackground="#313244", activestyle="none",
            exportselection=False)
        sb = ttk.Scrollbar(lb_frm, orient=tk.VERTICAL,
                           command=self._seg_listbox.yview)
        self._seg_listbox.configure(yscrollcommand=sb.set)
        self._seg_listbox.grid(row=0, column=0, sticky="nswe")
        sb.grid(row=0, column=1, sticky="ns")

        # Botões de gestão
        btn_frm = ttk.Frame(frm)
        btn_frm.grid(row=6, column=0, columnspan=3, sticky="we", pady=(3, 0))
        ttk.Button(btn_frm, text="✕ Remover último",
                   command=self._remove_last_segment).pack(side=tk.LEFT, expand=True, fill=tk.X)
        ttk.Button(btn_frm, text="🗑 Limpar tudo",
                   command=self._clear_all_segments).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=(4, 0))

        ttk.Separator(frm, orient=tk.HORIZONTAL).grid(
            row=7, column=0, columnspan=3, sticky="ew", pady=5)

        # ── Linha 8: resampling ───────────────────────────────────────────────
        rs_frm = ttk.Frame(frm)
        rs_frm.grid(row=8, column=0, columnspan=3, sticky="we")
        ttk.Label(rs_frm, text="Resample:", font=("Segoe UI", 8)).pack(side=tk.LEFT)
        self._resample_var = tk.StringVar(value="")
        ttk.Entry(rs_frm, textvariable=self._resample_var, width=4).pack(
            side=tk.LEFT, padx=3)
        ttk.Label(rs_frm, text="s", font=("Segoe UI", 8),
                  foreground="#585b70").pack(side=tk.LEFT)

        # ── Linha 9-10: botões de export ──────────────────────────────────────
        self._btn_export = ttk.Button(
            frm, text="⬇ Export sessão (0 seg.)", command=self._export_nilmtk)
        self._btn_export.grid(row=9, column=0, columnspan=3,
                              sticky="we", pady=(4, 2))

        ttk.Button(frm, text="🗂 Consolidar arquivos H5…",
                   command=self._consolidate_dialog).grid(
            row=10, column=0, columnspan=3, sticky="we")

        self.root.after(1000, self._tick_recording)

    # ── CSV path ──────────────────────────────────────────────────────────────
    def _browse_csv(self):
        path = filedialog.asksaveasfilename(
            defaultextension=".csv",
            filetypes=[("CSV", "*.csv"), ("Todos", "*.*")],
            title="Salvar CSV da sessão",
        )
        if path:
            # Armazena para uso no primeiro start()
            self._csv_session_path = path

    # ── Gravar / Parar ────────────────────────────────────────────────────────
    def _toggle_recording(self):
        if self._recorder.recording:
            n = self._recorder.row_count
            self._recorder.stop()
            self._btn_rec.config(text="⏺ Gravar")
            self._rec_status_var.set("● Parado")
            self._lbl_rec.config(foreground="#585b70")
            self._update_segments_list()
            self._append_log(f"# [REC] '{self._label_var.get() or '?'}' — {n} amostras")
        else:
            label    = self._label_var.get().strip()
            csv_path = getattr(self, '_csv_session_path', None)
            self._recorder.start(label=label, csv_path=csv_path)
            self._csv_session_path = None  # CSV só abre uma vez por sessão
            self._btn_rec.config(text="⏹ Parar")
            self._rec_status_var.set(f"● {label or 'Carga ?'} …")
            self._lbl_rec.config(foreground="#f38ba8")
            self._append_log(f"# [REC] Iniciando '{self._recorder._active['label']}'")

    def _tick_recording(self):
        if self._recorder.recording:
            n   = self._recorder.row_count
            ela = self._recorder.elapsed
            self._rec_status_var.set(f"● {n} amostras | {ela}")
        # Atualiza rótulo do botão de export
        n_segs = len(self._recorder.segments)
        self._btn_export.config(text=f"⬇ Export sessão ({n_segs} seg.)")
        self.root.after(1000, self._tick_recording)

    # ── Gestão da listbox de segmentos ────────────────────────────────────────
    def _update_segments_list(self):
        self._seg_listbox.delete(0, tk.END)
        for i, (label, n) in enumerate(self._recorder.segments, start=1):
            tag = '⏺' if label.endswith(' ⏺') else f'm{i}'
            lbl = label.rstrip(' ⏺')
            self._seg_listbox.insert(tk.END, f" {tag}  {lbl:<18} {n:>6} pts")

    def _remove_last_segment(self):
        if self._recorder.recording:
            messagebox.showinfo("Gravação ativa",
                                "Pare a gravação antes de remover o segmento.")
            return
        removed = self._recorder.remove_last_segment()
        if removed:
            self._update_segments_list()
            self._append_log(f"# [REC] Segmento removido: '{removed}'")

    def _clear_all_segments(self):
        if not messagebox.askyesno("Limpar tudo",
                                   "Descartar TODOS os segmentos gravados?\n"
                                   "Esta ação não pode ser desfeita."):
            return
        self._recorder.clear_all()
        self._update_segments_list()
        self._append_log("# [REC] Todos os segmentos removidos")

    # ── Export da sessão ──────────────────────────────────────────────────────
    def _export_nilmtk(self):
        if self._recorder.total_rows == 0:
            messagebox.showwarning("Sem dados",
                                   "Nenhuma amostra gravada.\n"
                                   "Inicie pelo menos uma gravação.")
            return

        hdf5_path = filedialog.asksaveasfilename(
            defaultextension=".h5",
            filetypes=[("HDF5", "*.h5 *.hdf5"), ("Todos", "*.*")],
            title="Salvar dataset NILMTK",
        )
        if not hdf5_path:
            return

        resample_s = self._parse_resample()
        if resample_s is False:
            return

        try:
            result = self._recorder.export_nilmtk_hdf5(
                hdf5_path, building=1,
                resample_s=resample_s,
            )
            lines = [f"  meter{i+1}: {lbl} ({n} pts)"
                     for i, (lbl, n) in enumerate(result.items())]
            meta_path = pathlib.Path(hdf5_path).with_suffix('.metadata.json')
            msg = (
                f"{len(result)} segmento(s) exportados:\n" + "\n".join(lines) +
                f"\n\nArquivo: {hdf5_path}"
                f"\nMetadados: {meta_path}"
                f"\n\nPara carregar no NILMTK:\n"
                f"  from nilmtk import DataSet\n"
                f"  ds = DataSet(r'{pathlib.Path(hdf5_path).name}')\n"
                f"  elec = ds.buildings[1].elec\n"
                f"  elec['meter1'].power_series_all_data().plot()"
            )
            messagebox.showinfo("Export concluído", msg)
            for lbl, n in result.items():
                self._append_log(f"# [NILMTK] {lbl}: {n} linhas → {hdf5_path}")
        except Exception as exc:
            messagebox.showerror("Erro ao exportar", str(exc))
            self._append_log(f"# [NILMTK] ERRO: {exc}")

    # ── Diálogo de consolidação de arquivos externos ──────────────────────────
    def _consolidate_dialog(self):
        dlg = tk.Toplevel(self.root)
        dlg.title("Consolidar arquivos HDF5")
        dlg.geometry("560x400")
        dlg.configure(bg="#1e1e2e")
        dlg.grab_set()

        ttk.Label(dlg, text=(
            "Adicione arquivos HDF5 exportados por esta GUI e atribua um\n"
            "label a cada um. Todos serão mesclados em um único dataset."
        ), font=("Segoe UI", 9)).pack(padx=10, pady=(10, 4))

        # Lista de arquivos selecionados: [(path, label), ...]
        file_list: list[list] = []   # [[path, label_var], ...]

        list_frm = ttk.Frame(dlg)
        list_frm.pack(fill=tk.BOTH, expand=True, padx=10, pady=4)

        canvas  = tk.Canvas(list_frm, bg="#181825", bd=0, highlightthickness=0)
        vscroll = ttk.Scrollbar(list_frm, orient=tk.VERTICAL, command=canvas.yview)
        inner   = ttk.Frame(canvas)
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=vscroll.set)
        inner.bind("<Configure>",
                   lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vscroll.pack(side=tk.RIGHT, fill=tk.Y)

        def refresh_rows():
            for w in inner.winfo_children():
                w.destroy()
            for idx, (path, lv) in enumerate(file_list):
                row = ttk.Frame(inner)
                row.pack(fill=tk.X, pady=1)
                ttk.Label(row, text=pathlib.Path(path).name, width=24,
                          font=("Courier New", 8)).pack(side=tk.LEFT)
                ttk.Entry(row, textvariable=lv, width=18).pack(side=tk.LEFT, padx=4)
                ttk.Button(row, text="✕", width=2,
                           command=lambda i=idx: remove_row(i)).pack(side=tk.LEFT)

        def remove_row(idx):
            file_list.pop(idx)
            refresh_rows()

        def add_files():
            paths = filedialog.askopenfilenames(
                filetypes=[("HDF5", "*.h5 *.hdf5"), ("Todos", "*.*")],
                title="Selecionar arquivos HDF5",
            )
            for p in paths:
                stem  = pathlib.Path(p).stem
                lv    = tk.StringVar(value=stem)
                file_list.append([p, lv])
            refresh_rows()

        # Barra inferior
        bot = ttk.Frame(dlg)
        bot.pack(fill=tk.X, padx=10, pady=6)

        ttk.Button(bot, text="＋ Adicionar arquivos…",
                   command=add_files).pack(side=tk.LEFT)

        rs_frm = ttk.Frame(bot)
        rs_frm.pack(side=tk.LEFT, padx=12)
        ttk.Label(rs_frm, text="Resample:", font=("Segoe UI", 8)).pack(side=tk.LEFT)
        dlg_rs = tk.StringVar(value=self._resample_var.get())
        ttk.Entry(rs_frm, textvariable=dlg_rs, width=4).pack(side=tk.LEFT, padx=2)
        ttk.Label(rs_frm, text="s", font=("Segoe UI", 8)).pack(side=tk.LEFT)

        def do_consolidate():
            if not file_list:
                messagebox.showwarning("Sem arquivos", "Adicione pelo menos um arquivo.",
                                       parent=dlg)
                return
            inputs = [(p, lv.get().strip() or pathlib.Path(p).stem)
                      for p, lv in file_list]
            rs_str = dlg_rs.get().strip()
            resample_s = None
            if rs_str:
                try:
                    resample_s = int(rs_str)
                except ValueError:
                    messagebox.showerror("Resample inválido", f"'{rs_str}' não é inteiro.",
                                         parent=dlg)
                    return
            out_path = filedialog.asksaveasfilename(
                defaultextension=".h5",
                filetypes=[("HDF5", "*.h5 *.hdf5"), ("Todos", "*.*")],
                title="Salvar dataset consolidado",
                parent=dlg,
            )
            if not out_path:
                return
            try:
                result = consolidate_hdf5_files(inputs, out_path,
                                                resample_s=resample_s)
                lines  = [f"  meter{i+1}: {lbl} ({n} pts)"
                          for i, (lbl, n) in enumerate(result.items())]
                msg = (f"{len(result)} arquivo(s) consolidados:\n"
                       + "\n".join(lines)
                       + f"\n\nSaída: {out_path}")
                messagebox.showinfo("Consolidação concluída", msg, parent=dlg)
                self._append_log(f"# [NILMTK] Consolidado: {out_path} "
                                 f"({sum(result.values())} pts total)")
                dlg.destroy()
            except Exception as exc:
                messagebox.showerror("Erro", str(exc), parent=dlg)
                self._append_log(f"# [NILMTK] ERRO consolidação: {exc}")

        ttk.Button(bot, text="⬇ Consolidar e salvar",
                   command=do_consolidate).pack(side=tk.RIGHT)

    # ── Helpers ───────────────────────────────────────────────────────────────
    def _parse_resample(self):
        """Retorna int ou None. Retorna False se inválido (já exibe erro)."""
        rs_str = self._resample_var.get().strip()
        if not rs_str:
            return None
        try:
            return int(rs_str)
        except ValueError:
            messagebox.showerror("Resample inválido", f"'{rs_str}' não é inteiro.")
            return False

    # ── Painel de conexão ─────────────────────────────────────────────────────
    def _build_conn_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Conexão Serial", padding=(8, 8))
        frm.pack(fill=tk.X, pady=(0, 8))

        ttk.Label(frm, text="Porta:").grid(row=0, column=0, sticky="w", pady=3)
        self.port_cb = ttk.Combobox(frm, width=11)
        self.port_cb.grid(row=0, column=1, padx=4, pady=3)
        ttk.Button(frm, text="↺", command=self.update_ports, width=3).grid(
            row=0, column=2, padx=2)

        ttk.Label(frm, text="Baud:").grid(row=1, column=0, sticky="w")
        self.baud_var = tk.StringVar(value=str(SERIAL_BAUD))
        ttk.Entry(frm, textvariable=self.baud_var, width=9).grid(row=1, column=1, padx=4)

        self.btn_connect    = ttk.Button(frm, text="Conectar",    command=self.connect)
        self.btn_disconnect = ttk.Button(frm, text="Desconectar", command=self.disconnect,
                                          state=tk.DISABLED)
        self.btn_connect.grid(   row=2, column=0, columnspan=2, sticky="we", pady=4, padx=2)
        self.btn_disconnect.grid(row=2, column=2,               sticky="we", pady=4, padx=2)

        self.status_var  = tk.StringVar(value="● Desconectado")
        self._lbl_status = ttk.Label(frm, textvariable=self.status_var,
                                      font=("Segoe UI", 9, "bold"), foreground="#f38ba8")
        self._lbl_status.grid(row=3, column=0, columnspan=3, sticky="w", pady=2)

    # ── Estatísticas ──────────────────────────────────────────────────────────
    def _build_stats_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Estatísticas", padding=(8, 6))
        frm.pack(fill=tk.X, pady=(0, 8))

        rows = [
            ("Pacotes OK:", "stat_ok_var",  "#a6e3a1"),
            ("Erros:",       "stat_err_var", "#f38ba8"),
            ("Taxa (Hz):",   "stat_hz_var",  "#f9e2af"),
        ]
        for r, (lbl, attr, color) in enumerate(rows):
            ttk.Label(frm, text=lbl, font=("Segoe UI", 8)).grid(
                row=r, column=0, sticky="w", pady=2)
            var = tk.StringVar(value="0")
            ttk.Label(frm, textvariable=var,
                      font=("Courier New", 10, "bold"), foreground=color).grid(
                row=r, column=1, sticky="w", padx=8)
            setattr(self, attr, var)

        ttk.Button(frm, text="Resetar contadores", command=self._reset_stats).grid(
            row=len(rows), column=0, columnspan=2, sticky="we", pady=(6, 2))

    # ── Log serial ────────────────────────────────────────────────────────────
    def _build_log_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Log / Debug", padding=(8, 6))
        frm.pack(fill=tk.BOTH, expand=True)

        self.log_text = tk.Text(
            frm, bg="#181825", fg="#a6adc8",
            font=("Courier New", 7), relief="flat",
            wrap=tk.WORD, state=tk.DISABLED)
        sb = ttk.Scrollbar(frm, orient=tk.VERTICAL, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=sb.set)
        self.log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)

    # ── Painel de métricas ────────────────────────────────────────────────────
    def _build_metrics_panel(self, parent):
        outer = ttk.LabelFrame(parent, text="Medições em Tempo Real", padding=(6, 6))
        outer.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        for c in range(6):
            outer.columnconfigure(c, weight=1)

        METRICS = [
            ("Tensão RMS",     "V",   "#89b4fa"),
            ("Corrente RMS",   "A",   "#f38ba8"),
            ("Pot. Ativa",     "W",   "#a6e3a1"),
            ("Pot. Aparente",  "VA",  "#cba6f7"),
            ("Pot. Reativa",   "VAr", "#fab387"),
            ("Fator Potência", "",    "#f9e2af"),
        ]
        self._metric_lbls = []
        for c, (label, unit, color) in enumerate(METRICS):
            ttk.Label(outer, text=label, font=("Segoe UI", 8),
                      foreground="#a6adc8").grid(row=0, column=c, padx=4, pady=(2, 0))
            placeholder = f"-.-- {unit}" if unit else "--.----"
            lbl = ttk.Label(outer, text=placeholder,
                            font=("Segoe UI", 20, "bold"), foreground=color)
            lbl.grid(row=1, column=c, padx=4, pady=(0, 4))
            self._metric_lbls.append((lbl, unit))

    # ── Notebook ──────────────────────────────────────────────────────────────
    def _build_notebook(self, parent):
        self._nb = ttk.Notebook(parent)
        self._nb.grid(row=1, column=0, sticky="nswe")

        tab_hist  = ttk.Frame(self._nb)
        tab_power = ttk.Frame(self._nb)
        self._nb.add(tab_hist,  text="  Histórico  ")
        self._nb.add(tab_power, text="  Triângulo de Potências  ")

        self._build_history_tab(tab_hist)
        self._build_power_tab(tab_power)

    # ── Aba: histórico ────────────────────────────────────────────────────────
    def _build_history_tab(self, parent):
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(0, weight=1)

        fig = plt.figure(figsize=(11, 5.2), facecolor="#1e1e2e")
        gs  = gridspec.GridSpec(3, 2, figure=fig, hspace=0.62, wspace=0.30,
                                left=0.07, right=0.97, top=0.93, bottom=0.07)

        PLOTS = [
            (gs[0, 0], "Tensão RMS",     "#89b4fa", "V",   "Vrms"),
            (gs[0, 1], "Corrente RMS",   "#f38ba8", "A",   "Irms"),
            (gs[1, 0], "Pot. Ativa",     "#a6e3a1", "W",   "Preal"),
            (gs[1, 1], "Pot. Aparente",  "#cba6f7", "VA",  "Paparente"),
            (gs[2, 0], "Pot. Reativa",   "#fab387", "VAr", "Preativa"),
            (gs[2, 1], "Fator Potência", "#f9e2af", "",    "FP"),
        ]

        self._hist_axes  = []
        self._hist_lines = []
        self._hist_keys  = []

        for subspec, title, color, ylabel, key in PLOTS:
            ax = fig.add_subplot(subspec)
            ax.set_facecolor("#181825")
            ax.tick_params(colors="#a6adc8", labelsize=7)
            ax.set_title(title, color=color, fontsize=9, pad=2)
            if ylabel:
                ax.set_ylabel(ylabel, color=color, fontsize=8)
            ax.grid(color="#313244", linestyle="--", linewidth=0.5)
            ax.set_xlim(0, HISTORY_LEN)
            ax.set_ylim(0, 1)
            line, = ax.plot([], [], color=color, linewidth=1.2)
            self._hist_axes.append(ax)
            self._hist_lines.append(line)
            self._hist_keys.append(key)

        self._hist_fig  = fig
        self.canvas_hist = FigureCanvasTkAgg(fig, master=parent)
        self.canvas_hist.get_tk_widget().grid(row=0, column=0, sticky="nswe")

    # ── Aba: triângulo de potências ───────────────────────────────────────────
    def _build_power_tab(self, parent):
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(0, weight=1)

        fig = plt.figure(figsize=(8, 5), facecolor="#1e1e2e")
        ax  = fig.add_subplot(111)
        ax.set_facecolor("#181825")
        ax.tick_params(colors="#a6adc8", labelsize=8)
        ax.set_title("Triângulo de Potências", color="#cdd6f4", fontsize=11, pad=6)
        ax.set_xlabel("P — Potência Ativa (W)",    color="#a6e3a1", fontsize=9)
        ax.set_ylabel("Q — Potência Reativa (VAr)", color="#fab387", fontsize=9)
        ax.grid(color="#313244", linestyle="--", linewidth=0.5)

        self._pwr_ax  = ax
        self._pwr_fig = fig
        self.canvas_power = FigureCanvasTkAgg(fig, master=parent)
        self.canvas_power.get_tk_widget().grid(row=0, column=0, sticky="nswe")

    # ── Serial ────────────────────────────────────────────────────────────────
    def update_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb["values"] = ports
        if ports:
            self.port_cb.current(0)

    def connect(self):
        port = self.port_cb.get()
        if not port:
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            baud = SERIAL_BAUD
        try:
            self.serial_port = serial.Serial(port, baud, timeout=0.2)
            self.is_reading  = True
            self.btn_connect.config(state=tk.DISABLED)
            self.btn_disconnect.config(state=tk.NORMAL)
            self.status_var.set(f"● Conectado ({port})")
            self._lbl_status.config(foreground="#a6e3a1")
            self.read_thread = threading.Thread(target=self._read_serial, daemon=True)
            self.read_thread.start()
        except Exception as e:
            messagebox.showerror("Erro de Conexão", str(e))

    def disconnect(self):
        self.is_reading = False
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        self.btn_connect.config(state=tk.NORMAL)
        self.btn_disconnect.config(state=tk.DISABLED)
        self.status_var.set("● Desconectado")
        self._lbl_status.config(foreground="#f38ba8")

    def _read_serial(self):
        while self.is_reading:
            try:
                raw = self.serial_port.readline()
                if raw:
                    line = raw.decode("ascii", errors="ignore").strip()
                    if line:
                        self._data_queue.put(line)
            except Exception:
                pass

    # ── Polling ───────────────────────────────────────────────────────────────
    def _poll_queue(self):
        latest_meter = None
        try:
            while True:
                line = self._data_queue.get_nowait()
                if line.startswith("METER,"):
                    latest_meter = line
                elif line.startswith("#"):
                    self._append_log(line)
        except queue.Empty:
            pass

        if latest_meter:
            self._process_meter(latest_meter)

        self.root.after(50, self._poll_queue)

    # ── Processamento do pacote METER ─────────────────────────────────────────
    def _process_meter(self, line: str):
        parts = line.split(",")
        if len(parts) != 7:
            self._pkt_err += 1
            self.stat_err_var.set(str(self._pkt_err))
            return
        try:
            vrms  = float(parts[1])
            irms  = float(parts[2])
            preal = float(parts[3])
            pap   = float(parts[4])
            prea  = float(parts[5])
            fp    = float(parts[6])
        except ValueError:
            self._pkt_err += 1
            self.stat_err_var.set(str(self._pkt_err))
            return

        self._pkt_ok += 1
        self.stat_ok_var.set(str(self._pkt_ok))

        # Alimenta o gravador com timestamp UTC preciso
        self._recorder.append(
            ts    = datetime.datetime.now(datetime.timezone.utc),
            vrms  = vrms,  irms = irms,
            preal = preal, pap  = pap,
            prea  = prea,  fp   = fp,
        )

        now = time.monotonic()
        self._hz_win.append(now)
        if len(self._hz_win) >= 2:
            dt = self._hz_win[-1] - self._hz_win[0]
            hz = (len(self._hz_win) - 1) / dt if dt > 0 else 0.0
            self.stat_hz_var.set(f"{hz:.1f}")

        self._hist["Vrms"].append(vrms)
        self._hist["Irms"].append(irms)
        self._hist["Preal"].append(preal)
        self._hist["Paparente"].append(pap)
        self._hist["Preativa"].append(prea)
        self._hist["FP"].append(fp)

        vals = [
            f"{vrms:.2f} V",
            f"{irms:.4f} A",
            f"{preal:.2f} W",
            f"{pap:.2f} VA",
            f"{prea:.2f} VAr",
            f"{fp:.4f}",
        ]
        for (lbl, _unit), text in zip(self._metric_lbls, vals):
            lbl.config(text=text)

        current_tab = self._nb.index(self._nb.select())
        if current_tab == 0:
            self._update_history()
        elif current_tab == 1:
            self._update_power_triangle(preal, prea, pap, fp)

    # ── Gráficos de histórico ─────────────────────────────────────────────────
    def _update_history(self):
        for ax, line, key in zip(self._hist_axes, self._hist_lines, self._hist_keys):
            data = list(self._hist[key])
            n = len(data)
            if n < 2:
                continue
            x = list(range(n))
            line.set_data(x, data)
            ax.set_xlim(0, max(n, HISTORY_LEN))
            mn, mx = min(data), max(data)
            span = max(mx - mn, abs(mx) * 0.02, 1e-3)
            ax.set_ylim(mn - span * 0.15, mx + span * 0.15)
        self.canvas_hist.draw_idle()

    # ── Triângulo de potências ────────────────────────────────────────────────
    def _update_power_triangle(self, P: float, Q: float, S: float, fp: float):
        ax = self._pwr_ax
        ax.cla()
        ax.set_facecolor("#181825")
        ax.tick_params(colors="#a6adc8", labelsize=8)
        ax.set_title("Triângulo de Potências", color="#cdd6f4", fontsize=11, pad=6)
        ax.set_xlabel("P — Potência Ativa (W)",     color="#a6e3a1", fontsize=9)
        ax.set_ylabel("Q — Potência Reativa (VAr)",  color="#fab387", fontsize=9)
        ax.axhline(0, color="#45475a", linewidth=0.8)
        ax.axvline(0, color="#45475a", linewidth=0.8)
        ax.grid(color="#313244", linestyle="--", linewidth=0.5)

        eps = 1.0
        if abs(P) < eps and abs(Q) < eps:
            ax.text(0.5, 0.5, "Aguardando dados…", color="#585b70",
                    ha="center", va="center", transform=ax.transAxes, fontsize=11)
            self.canvas_power.draw_idle()
            return

        aw = {"arrowstyle": "-|>", "lw": 2.2, "mutation_scale": 14}
        ax.annotate("", xy=(P, 0),  xytext=(0, 0),  arrowprops={**aw, "color": "#a6e3a1"})
        ax.annotate("", xy=(P, Q),  xytext=(P, 0),  arrowprops={**aw, "color": "#fab387"})
        ax.annotate("", xy=(P, Q),  xytext=(0, 0),  arrowprops={**aw, "color": "#cba6f7"})

        sign_q = 1 if Q >= 0 else -1
        ax.text(P / 2,  -sign_q * abs(S) * 0.06 - sign_q * eps,
                f"P = {P:.2f} W",  color="#a6e3a1", fontsize=9, ha="center", va="top" if Q >= 0 else "bottom")
        ax.text(P + abs(P) * 0.03 + eps, Q / 2,
                f"Q = {Q:.2f} VAr", color="#fab387", fontsize=9, ha="left", va="center")
        ax.text(P / 2,  Q / 2,
                f"S = {S:.2f} VA",  color="#cba6f7", fontsize=9, ha="center",
                rotation=math.degrees(math.atan2(Q, P)) if P != 0 else 90)

        ax.text(0.02, 0.97, f"FP = {fp:.4f}", color="#f9e2af", fontsize=10,
                fontweight="bold", ha="left", va="top", transform=ax.transAxes)

        if abs(fp) <= 1.0 and S > eps:
            angle_deg = math.degrees(math.acos(min(abs(fp), 1.0)))
            r_arc = S * 0.20
            theta = np.linspace(0, math.radians(angle_deg), 50)
            ax.plot(r_arc * np.cos(theta), sign_q * r_arc * np.sin(theta),
                    color="#94e2d5", linewidth=1.4)
            ax.text(r_arc * 1.12, sign_q * r_arc * 0.3,
                    f"{angle_deg:.1f}°", color="#94e2d5", fontsize=8, va="center")

        margin = max(abs(P), abs(S)) * 0.18 + eps * 3
        ax.set_xlim(-margin, P + margin)
        q_lo = min(Q, 0) - margin
        q_hi = max(Q, 0) + margin
        ax.set_ylim(q_lo, q_hi)
        ax.set_aspect("equal", adjustable="datalim")
        self.canvas_power.draw_idle()

    # ── Log ───────────────────────────────────────────────────────────────────
    def _append_log(self, line: str):
        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, line + "\n")
        nlines = int(self.log_text.index("end-1c").split(".")[0])
        if nlines > 250:
            self.log_text.delete("1.0", f"{nlines - 200}.0")
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)

    # ── Utilitários ───────────────────────────────────────────────────────────
    def _reset_stats(self):
        self._pkt_ok = 0
        self._pkt_err = 0
        self.stat_ok_var.set("0")
        self.stat_err_var.set("0")
        self.stat_hz_var.set("0")
        self._hz_win.clear()


if __name__ == "__main__":
    root = tk.Tk()
    app  = BridgeGui(root)
    root.protocol("WM_DELETE_WINDOW", lambda: sys.exit(0))
    root.mainloop()
