#!/usr/bin/env python3
"""
ESP-Network Debug GUI — monitor serial do ESP32 (115200 baud)

Protocolo serial esperado:
  METER,<phase>,<vrms>,<irms>,<preal>,<s>,<q>,<fp>,<kwh>
  # [ESP32] up:<s>s pktOk:<n> pktErr:<n> kWh:<f> WiFi:<0/1> MQTT:<0/1> Debug:<0/1>
  # qualquer outra mensagem de log

Uso:
    python esp_debug.py
"""

from __future__ import annotations

import re
import sys
from datetime import datetime

import serial
import serial.tools.list_ports
from PyQt5 import QtCore, QtGui, QtWidgets

BAUDRATE = 115200
MAX_LOG_LINES = 2000

_RE_STATUS = re.compile(
    r'up:(\d+)s\s+pktOk:(\d+)\s+pktErr:(\d+)\s+kWh:([\d.]+)'
    r'\s+WiFi:(\d)\s+MQTT:(\d)\s+Debug:(\d)'
)
# Todos os campos numéricos aceitam sinal: Preal/Q/FP podem ser negativos
# (FP negativo na fase 1 por causa do CT invertido — SCT013_SIGN=-1.0).
_RE_METER = re.compile(
    r'^METER,(\d),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)$'
)


# ── Worker serial (thread separada) ───────────────────────────────────────────

class SerialWorker(QtCore.QThread):
    line_received  = QtCore.pyqtSignal(str)
    error_occurred = QtCore.pyqtSignal(str)

    def __init__(self, port: str, baudrate: int):
        super().__init__()
        self._port     = port
        self._baudrate = baudrate
        self._running  = False
        self._ser: serial.Serial | None = None
        self._lock = QtCore.QMutex()

    def send(self, data: bytes):
        """Envia bytes para a porta serial (thread-safe)."""
        self._lock.lock()
        try:
            if self._ser and self._ser.is_open:
                self._ser.write(data)
        finally:
            self._lock.unlock()

    def run(self):
        try:
            self._ser = serial.Serial(self._port, self._baudrate, timeout=0.1)
        except serial.SerialException as exc:
            self.error_occurred.emit(str(exc))
            return

        self._running = True
        buf = b''
        try:
            while self._running:
                chunk = self._ser.read(512)
                if chunk:
                    buf += chunk
                    while b'\n' in buf:
                        raw, buf = buf.split(b'\n', 1)
                        text = raw.decode('utf-8', errors='replace').strip('\r ')
                        if text:
                            self.line_received.emit(text)
        except serial.SerialException as exc:
            self.error_occurred.emit(str(exc))
        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()

    def stop(self):
        self._running = False
        self.wait(2000)


# ── Painel de medições de uma fase ────────────────────────────────────────────

class PhasePanel(QtWidgets.QGroupBox):
    _FIELDS = [
        ('Vrms',  'V',   'vrms'),
        ('Irms',  'A',   'irms'),
        ('Preal', 'W',   'preal'),
        ('S',     'VA',  's'),
        ('Q',     'VAr', 'q'),
        ('FP',    '',    'fp'),
        ('kWh',   'kWh', 'kwh'),
    ]

    def __init__(self, phase: int, parent=None):
        super().__init__(f'Fase {phase}', parent)
        self._labels: dict[str, QtWidgets.QLabel] = {}
        grid = QtWidgets.QGridLayout(self)
        grid.setSpacing(4)

        bold = QtGui.QFont()
        bold.setBold(True)
        bold.setPointSize(11)

        for row, (name, unit, key) in enumerate(self._FIELDS):
            lbl_name = QtWidgets.QLabel(f'{name}:')
            lbl_val  = QtWidgets.QLabel('—')
            lbl_unit = QtWidgets.QLabel(unit)
            lbl_val.setFont(bold)
            lbl_val.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
            lbl_val.setMinimumWidth(90)
            grid.addWidget(lbl_name, row, 0)
            grid.addWidget(lbl_val,  row, 1)
            grid.addWidget(lbl_unit, row, 2)
            self._labels[key] = lbl_val

        self._frames = 0
        self._fps_label = QtWidgets.QLabel('0 fps')
        self._fps_label.setAlignment(QtCore.Qt.AlignRight)
        grid.addWidget(QtWidgets.QLabel('Taxa:'), len(self._FIELDS), 0)
        grid.addWidget(self._fps_label, len(self._FIELDS), 1, 1, 2)

        self._fps_timer = QtCore.QTimer(self)
        self._fps_timer.setInterval(1000)
        self._fps_timer.timeout.connect(self._update_fps)
        self._fps_timer.start()

    def _update_fps(self):
        self._fps_label.setText(f'{self._frames} fps')
        self._frames = 0

    def update_meter(self, vrms, irms, preal, s, q, fp, kwh):
        self._frames += 1
        vals = dict(vrms=vrms, irms=irms, preal=preal, s=s, q=q, fp=fp, kwh=kwh)
        fmts = dict(vrms='.1f', irms='.3f', preal='.1f', s='.1f',
                    q='.1f', fp='.4f', kwh='.6f')
        for key, lbl in self._labels.items():
            lbl.setText(format(vals[key], fmts[key]))


# ── Painel de status do ESP32 ─────────────────────────────────────────────────

class StatusPanel(QtWidgets.QGroupBox):
    def __init__(self, parent=None):
        super().__init__('Status ESP32', parent)
        grid = QtWidgets.QGridLayout(self)
        grid.setSpacing(4)

        self._indicators: dict[str, QtWidgets.QLabel] = {}
        rows = [
            ('wifi',   'WiFi'),
            ('mqtt',   'MQTT'),
            ('debug',  'Debug'),
            ('pktok',  'pktOk'),
            ('pkterr', 'pktErr'),
            ('uptime', 'Uptime'),
        ]
        bold = QtGui.QFont(); bold.setBold(True); bold.setPointSize(11)
        for i, (key, name) in enumerate(rows):
            grid.addWidget(QtWidgets.QLabel(f'{name}:'), i, 0)
            lbl = QtWidgets.QLabel('—')
            lbl.setFont(bold)
            lbl.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
            grid.addWidget(lbl, i, 1)
            self._indicators[key] = lbl

    def update_status(self, up_s: int, pkt_ok: int, pkt_err: int,
                      kwh: float, wifi: int, mqtt: int, debug: int):
        h, rem = divmod(int(up_s), 3600)
        m, s   = divmod(rem, 60)
        self._indicators['uptime'].setText(f'{h:02d}:{m:02d}:{s:02d}')
        self._indicators['pktok'].setText(str(pkt_ok))
        self._indicators['pkterr'].setText(str(pkt_err))

        for key, val, on_txt, off_txt in [
            ('wifi',  wifi,  '● Online', '○ Offline'),
            ('mqtt',  mqtt,  '● Conectado', '○ Desconectado'),
            ('debug', debug, '● ATIVO', '○ normal'),
        ]:
            lbl = self._indicators[key]
            lbl.setText(on_txt if val else off_txt)
            lbl.setStyleSheet(
                'color: #2ecc71;' if val else
                ('color: #e74c3c;' if key != 'debug' else 'color: #f39c12;')
            )


# ── Janela principal ──────────────────────────────────────────────────────────

class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('ESP-Network Debug Monitor')
        self.setMinimumSize(900, 620)
        self._worker: SerialWorker | None = None

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QVBoxLayout(central)
        root.setSpacing(6)
        root.setContentsMargins(8, 8, 8, 8)

        # ── Barra de conexão ──────────────────────────────────────────────────
        bar = QtWidgets.QHBoxLayout()
        self._port_combo = QtWidgets.QComboBox()
        self._port_combo.setMinimumWidth(160)
        self._refresh_btn = QtWidgets.QPushButton('↺')
        self._refresh_btn.setFixedWidth(32)
        self._refresh_btn.setToolTip('Atualizar portas')
        self._refresh_btn.clicked.connect(self._refresh_ports)
        self._baud_label = QtWidgets.QLabel(f'  {BAUDRATE} baud  ')
        self._connect_btn = QtWidgets.QPushButton('Conectar')
        self._connect_btn.setCheckable(True)
        self._connect_btn.clicked.connect(self._toggle_connect)
        self._clear_btn = QtWidgets.QPushButton('Limpar log')
        self._clear_btn.clicked.connect(self._clear_log)
        self._skip_cal_btn = QtWidgets.QPushButton('⚡ Ignorar Cal')
        self._skip_cal_btn.setToolTip(
            'Envia "skip_cal" ao ESP32 → STM32 seta pisos mínimos imediatamente.\n'
            'Alternativa: segurar botão KEY do STM32 durante o boot.'
        )
        self._skip_cal_btn.setStyleSheet('color: #f39c12; font-weight: bold;')
        self._skip_cal_btn.clicked.connect(self._send_skip_cal)
        self._status_bar = QtWidgets.QLabel('Desconectado')
        self._status_bar.setStyleSheet('color: gray;')

        bar.addWidget(QtWidgets.QLabel('Porta:'))
        bar.addWidget(self._port_combo)
        bar.addWidget(self._refresh_btn)
        bar.addWidget(self._baud_label)
        bar.addWidget(self._connect_btn)
        bar.addWidget(self._clear_btn)
        bar.addWidget(self._skip_cal_btn)
        bar.addStretch()
        bar.addWidget(self._status_bar)
        root.addLayout(bar)

        # ── Linha do meio: fases + status ─────────────────────────────────────
        mid = QtWidgets.QHBoxLayout()
        self._phase1 = PhasePanel(1)
        self._phase2 = PhasePanel(2)
        self._status = StatusPanel()

        mid.addWidget(self._phase1, 2)
        mid.addWidget(self._phase2, 2)
        mid.addWidget(self._status, 1)
        root.addLayout(mid)

        # ── Log serial ────────────────────────────────────────────────────────
        log_header = QtWidgets.QHBoxLayout()
        log_header.addWidget(QtWidgets.QLabel('Log serial:'))
        log_header.addStretch()
        self._autoscroll_chk = QtWidgets.QCheckBox('Auto-scroll')
        self._autoscroll_chk.setChecked(True)
        log_header.addWidget(self._autoscroll_chk)
        root.addLayout(log_header)

        self._log = QtWidgets.QPlainTextEdit()
        self._log.setReadOnly(True)
        self._log.setMaximumBlockCount(MAX_LOG_LINES)
        self._log.setFont(QtGui.QFont('Consolas', 9))
        self._log.setStyleSheet('background:#1e1e1e; color:#d4d4d4;')
        root.addWidget(self._log, 1)

        self._refresh_ports()

    # ── Portas ────────────────────────────────────────────────────────────────

    def _refresh_ports(self):
        self._port_combo.clear()
        ports = serial.tools.list_ports.comports()
        for p in sorted(ports, key=lambda x: x.device):
            self._port_combo.addItem(f'{p.device}  —  {p.description}', p.device)
        if not ports:
            self._port_combo.addItem('(nenhuma porta encontrada)', '')

    # ── Conexão ───────────────────────────────────────────────────────────────

    def _toggle_connect(self, checked: bool):
        if checked:
            port = self._port_combo.currentData()
            if not port:
                self._connect_btn.setChecked(False)
                return
            self._worker = SerialWorker(port, BAUDRATE)
            self._worker.line_received.connect(self._on_line)
            self._worker.error_occurred.connect(self._on_error)
            self._worker.finished.connect(self._on_worker_finished)
            self._worker.start()
            self._connect_btn.setText('Desconectar')
            self._status_bar.setText(f'Conectado em {port}')
            self._status_bar.setStyleSheet('color: #2ecc71;')
            self._log_line(f'=== Conectado em {port} @ {BAUDRATE} baud ===')
        else:
            self._disconnect()

    def _disconnect(self):
        if self._worker:
            self._worker.stop()
            self._worker = None
        self._connect_btn.setText('Conectar')
        self._connect_btn.setChecked(False)
        self._status_bar.setText('Desconectado')
        self._status_bar.setStyleSheet('color: gray;')
        self._log_line('=== Desconectado ===')

    def _on_worker_finished(self):
        self._disconnect()

    def _on_error(self, msg: str):
        self._log_line(f'[ERRO] {msg}', color='#e74c3c')
        self._disconnect()

    # ── Parsing de linhas ─────────────────────────────────────────────────────

    def _on_line(self, line: str):
        m_meter = _RE_METER.match(line)
        if m_meter:
            phase, vrms, irms, preal, s, q, fp, kwh = (
                int(m_meter.group(1)),
                *[float(m_meter.group(i)) for i in range(2, 9)],
            )
            panel = self._phase1 if phase == 1 else self._phase2
            panel.update_meter(vrms, irms, preal, s, q, fp, kwh)
            return   # não polui o log com METER

        m_status = _RE_STATUS.search(line)
        if m_status:
            self._status.update_status(
                int(m_status.group(1)),   # uptime s
                int(m_status.group(2)),   # pktOk
                int(m_status.group(3)),   # pktErr
                float(m_status.group(4)), # kWh
                int(m_status.group(5)),   # WiFi
                int(m_status.group(6)),   # MQTT
                int(m_status.group(7)),   # Debug
            )

        # Colore mensagens de erro/warn no log
        if 'falha' in line.lower() or 'erro' in line.lower() or 'error' in line.lower():
            color = '#e74c3c'
        elif 'conectado' in line.lower() or 'ip:' in line.lower():
            color = '#2ecc71'
        elif line.startswith('#'):
            color = '#9cdcfe'
        else:
            color = '#d4d4d4'

        self._log_line(line, color=color)

    # ── Log ───────────────────────────────────────────────────────────────────

    def _log_line(self, text: str, color: str = '#d4d4d4'):
        ts  = datetime.now().strftime('%H:%M:%S')
        fmt = (f'<span style="color:#666;">[{ts}]</span> '
               f'<span style="color:{color};">{_html_escape(text)}</span>')
        self._log.appendHtml(fmt)
        if self._autoscroll_chk.isChecked():
            sb = self._log.verticalScrollBar()
            sb.setValue(sb.maximum())

    def _send_skip_cal(self):
        if self._worker:
            self._worker.send(b'skip_cal\n')
            self._log_line('→ skip_cal enviado ao ESP32', color='#f39c12')
        else:
            self._log_line('[AVISO] Conecte ao ESP32 antes de enviar o comando.', color='#e74c3c')

    def _clear_log(self):
        self._log.clear()

    def closeEvent(self, event):
        self._disconnect()
        super().closeEvent(event)


def _html_escape(text: str) -> str:
    return (text.replace('&', '&amp;')
                .replace('<', '&lt;')
                .replace('>', '&gt;'))


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    app = QtWidgets.QApplication(sys.argv)
    app.setStyle('Fusion')

    palette = QtGui.QPalette()
    palette.setColor(QtGui.QPalette.Window,          QtGui.QColor(45, 45, 45))
    palette.setColor(QtGui.QPalette.WindowText,      QtGui.QColor(220, 220, 220))
    palette.setColor(QtGui.QPalette.Base,            QtGui.QColor(30, 30, 30))
    palette.setColor(QtGui.QPalette.AlternateBase,   QtGui.QColor(53, 53, 53))
    palette.setColor(QtGui.QPalette.Button,          QtGui.QColor(60, 60, 60))
    palette.setColor(QtGui.QPalette.ButtonText,      QtGui.QColor(220, 220, 220))
    palette.setColor(QtGui.QPalette.Highlight,       QtGui.QColor(42, 130, 218))
    palette.setColor(QtGui.QPalette.HighlightedText, QtCore.Qt.white)
    app.setPalette(palette)

    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()
