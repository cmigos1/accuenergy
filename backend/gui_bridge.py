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
import time
import math
import queue
import threading
import tkinter as tk
from tkinter import ttk, messagebox
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

        left = ttk.Frame(self.root, padding=(10, 10), width=265)
        left.grid(row=0, column=0, sticky="nswe")
        left.grid_propagate(False)
        self._build_conn_panel(left)
        self._build_stats_panel(left)
        self._build_log_panel(left)

        right = ttk.Frame(self.root, padding=(6, 10))
        right.grid(row=0, column=1, sticky="nswe")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(1, weight=1)
        self._build_metrics_panel(right)
        self._build_notebook(right)

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
