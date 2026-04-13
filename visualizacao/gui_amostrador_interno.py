import sys
import math
import struct
import queue
import threading
import tkinter as tk
from tkinter import ttk, messagebox

import numpy as np
import serial
import serial.tools.list_ports
import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

# ── Protocolo binário (deve espelhar envio.h) ─────────────────────────────────
#
#  DATA  (0x01): magic(2) + type(1) + 6×f32(24) + n_samples(1) + step(1)
#                + n×(V int16, I int16)(n×4) + footer(2)  → 31 + n×4 bytes
#  CALPOT(0x02): magic(2) + type(1) + 6×f32(24) + footer(2) → 29 bytes
#  CALPIN(0x03): magic(2) + type(1) + 4×f32(16) + footer(2) → 21 bytes
#
#  Offsets dentro do frame (base = 0):
#    [0:2]  magic 0xAB 0xCD
#    [2]    type
#    [3:27] payload floats (struct '<6f' ou '<4f')
#    DATA:  [27] n_samples  [28] step  [29:29+n*4] amostras  [29+n*4:] footer
# ─────────────────────────────────────────────────────────────────────────────
MAGIC        = bytes([0xAB, 0xCD])
FOOTER       = bytes([0xEF, 0xFE])
FRAME_DATA   = 0x01
FRAME_CALPOT = 0x02
FRAME_CALPIN = 0x03
CALPOT_SIZE  = 29
CALPIN_SIZE  = 21

SERIAL_BAUD  = 230400
DT_BASE_MS   = 1000.0 / 7200.0   # ≈ 0.1389 ms por amostra original


class AmostradorGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Medidor AC — ESP32 ADC Interno")
        self.root.geometry("1400x880")
        self.root.minsize(1100, 720)

        self.serial_port = None
        self.is_reading  = False
        self.read_thread = None
        self._data_queue = queue.SimpleQueue()

        self._v_rms_raw = 0.0
        self._i_rms_raw = 0.0

        # Blitting: backgrounds dos axes de forma de onda
        self._bg_v = None
        self._bg_i = None
        self._last_xlim = (0.0, 1.0)
        self._last_vlim = (-1.0, 1.0)
        self._last_ilim = (-1.0, 1.0)

        self._setup_style()
        self._setup_ui()
        self.update_ports()
        self.root.after(20, self._poll_queue)

    # ── Estilo ────────────────────────────────────────────────────────────────
    def _setup_style(self):
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TFrame",       background="#1e1e2e")
        style.configure("TLabel",       background="#1e1e2e", foreground="#cdd6f4")
        style.configure("TLabelframe",  background="#1e1e2e", foreground="#89b4fa")
        style.configure("TLabelframe.Label", background="#1e1e2e", foreground="#89b4fa",
                        font=("Segoe UI", 9, "bold"))
        style.configure("TButton",      background="#313244", foreground="#cdd6f4",
                        relief="flat", padding=4)
        style.configure("TEntry",       fieldbackground="#313244", foreground="#cdd6f4",
                        insertcolor="#cdd6f4")
        style.configure("TCombobox",    fieldbackground="#313244", foreground="#cdd6f4")
        style.map("TButton",            background=[("active", "#45475a")])
        style.configure("TNotebook",    background="#1e1e2e", tabmargins=[0, 0, 0, 0])
        style.configure("TNotebook.Tab", background="#313244", foreground="#cdd6f4",
                        padding=[10, 4])
        style.map("TNotebook.Tab",       background=[("selected", "#45475a")])
        self.root.configure(bg="#1e1e2e")

    # ── Layout principal ──────────────────────────────────────────────────────
    def _setup_ui(self):
        self.root.columnconfigure(1, weight=1)
        self.root.rowconfigure(0, weight=1)

        left = ttk.Frame(self.root, padding=(10, 10), width=248)
        left.grid(row=0, column=0, sticky="nswe")
        left.grid_propagate(False)

        self._build_conn_panel(left)
        self._build_calib_panel(left)
        self._build_multimeter_calib_panel(left)

        right = ttk.Frame(self.root, padding=(6, 10))
        right.grid(row=0, column=1, sticky="nswe")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(1, weight=1)

        self._build_metrics_panel(right)
        self._build_notebook(right)

    # ── Painel de Conexão ─────────────────────────────────────────────────────
    def _build_conn_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Conexão Serial", padding=(8, 8))
        frm.pack(fill=tk.X, pady=(0, 8))

        ttk.Label(frm, text="Porta:").grid(row=0, column=0, sticky="w", pady=3)
        self.port_cb = ttk.Combobox(frm, width=12)
        self.port_cb.grid(row=0, column=1, padx=4, pady=3)
        ttk.Button(frm, text="↺", command=self.update_ports, width=3).grid(
            row=0, column=2, padx=2)

        self.btn_connect    = ttk.Button(frm, text="Conectar",    command=self.connect)
        self.btn_disconnect = ttk.Button(frm, text="Desconectar", command=self.disconnect,
                                         state=tk.DISABLED)
        self.btn_connect.grid(   row=1, column=0, columnspan=2, sticky="we", pady=4, padx=2)
        self.btn_disconnect.grid(row=1, column=2,               sticky="we", pady=4, padx=2)

        self.status_var  = tk.StringVar(value="● Desconectado")
        self._lbl_status = ttk.Label(frm, textvariable=self.status_var,
                                     font=("Segoe UI", 9, "bold"), foreground="#f38ba8")
        self._lbl_status.grid(row=2, column=0, columnspan=3, sticky="w", pady=2)

    # ── Calibração de Escala ──────────────────────────────────────────────────
    def _build_calib_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Calibração de Escala  (M × Fator + Offset)",
                             padding=(8, 8))
        frm.pack(fill=tk.X, pady=(0, 8))

        for r, (lbl, attr, default) in enumerate([
            ("Fator Tensão:",    "v_calib",  1.0),
            ("Offset Tensão:",   "v_offset", 0.0),
            ("Fator Corrente:",  "i_calib",  1.0),
            ("Offset Corrente:", "i_offset", 0.0),
        ]):
            ttk.Label(frm, text=lbl).grid(row=r, column=0, sticky="w", pady=2, padx=4)
            var = tk.DoubleVar(value=default)
            setattr(self, f"{attr}_var", var)
            ttk.Entry(frm, textvariable=var, width=10).grid(row=r, column=1, padx=4, pady=2)

    # ── Calibração por Multímetro ─────────────────────────────────────────────
    def _build_multimeter_calib_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Calibração por Multímetro", padding=(8, 8))
        frm.pack(fill=tk.X, pady=(0, 8))

        ttk.Label(frm, text="V medido:").grid(row=1, column=0, sticky="w", pady=2)
        self.v_ref_var = tk.DoubleVar(value=0.0)
        ttk.Entry(frm, textvariable=self.v_ref_var, width=10).grid(row=1, column=1, padx=4)

        ttk.Label(frm, text="I medido:").grid(row=2, column=0, sticky="w", pady=2)
        self.i_ref_var = tk.DoubleVar(value=0.0)
        ttk.Entry(frm, textvariable=self.i_ref_var, width=10).grid(row=2, column=1, padx=4)

        ttk.Button(frm, text="✔ Calcular Fator",
                   command=self._apply_multimeter_calib).grid(
            row=3, column=0, columnspan=2, sticky="we", pady=(8, 2), padx=2)

        self.calib_result_var = tk.StringVar(value="")
        ttk.Label(frm, textvariable=self.calib_result_var, font=("Segoe UI", 8),
                  foreground="#a6e3a1").grid(row=4, column=0, columnspan=2, sticky="w", pady=2)

    # ── Métricas em Tempo Real ────────────────────────────────────────────────
    def _build_metrics_panel(self, parent):
        frm = ttk.LabelFrame(parent, text="Medições em Tempo Real", padding=(8, 6))
        frm.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        for c in range(7):
            frm.columnconfigure(c, weight=1)

        for col, (attr, init, unit, label, color) in enumerate([
            ("lbl_v_rms",    "0.00", "V",   "Tensão RMS",        "#89b4fa"),
            ("lbl_i_rms",    "0.00", "A",   "Corrente RMS",      "#f38ba8"),
            ("lbl_p_active", "0.00", "W",   "Pot. Ativa",        "#a6e3a1"),
            ("lbl_p_react",  "0.00", "VAr", "Pot. Reativa",      "#fab387"),
            ("lbl_p_appar",  "0.00", "VA",  "Pot. Aparente",     "#cba6f7"),
            ("lbl_pf",       "0.00", "",    "Fator de Potência", "#f9e2af"),
            ("lbl_angle",    "0.0",  "°",   "Ângulo φ",          "#94e2d5"),
        ]):
            v = ttk.Label(frm, text=f"{init} {unit}",
                          font=("Segoe UI", 22, "bold"), foreground=color)
            v.grid(row=0, column=col, padx=6, pady=(4, 0))
            setattr(self, attr, v)
            ttk.Label(frm, text=label, font=("Segoe UI", 9),
                      foreground="#a6adc8").grid(row=1, column=col, padx=6, pady=(0, 4))

    # ── Notebook ──────────────────────────────────────────────────────────────
    def _build_notebook(self, parent):
        nb = ttk.Notebook(parent)
        nb.grid(row=1, column=0, sticky="nswe")

        tab_wave = ttk.Frame(nb)
        tab_harm = ttk.Frame(nb)
        tab_cal  = ttk.Frame(nb)
        nb.add(tab_wave, text="  Formas de Onda  ")
        nb.add(tab_harm, text="  Harmônicos  ")
        nb.add(tab_cal,  text="  Calibração ADC  ")

        self._build_waveform_tab(tab_wave)
        self._build_harmonics_tab(tab_harm)
        self._build_calibration_tab(tab_cal)

    # ── Aba: Formas de Onda ───────────────────────────────────────────────────
    def _build_waveform_tab(self, parent):
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(0, weight=1)

        fig = plt.figure(figsize=(10, 4.5), facecolor="#1e1e2e")
        gs  = gridspec.GridSpec(2, 1, figure=fig, hspace=0.50,
                                left=0.07, right=0.97, top=0.93, bottom=0.10)

        self.ax_v = fig.add_subplot(gs[0])
        self.ax_i = fig.add_subplot(gs[1])

        for ax, title, color, ylabel in [
            (self.ax_v, "Tensão AC (V)",   "#89b4fa", "V"),
            (self.ax_i, "Corrente AC (A)", "#f38ba8", "A"),
        ]:
            ax.set_facecolor("#181825")
            ax.tick_params(colors="#a6adc8", labelsize=8)
            ax.set_title(title, color=color, fontsize=10, pad=4)
            ax.set_xlabel("Tempo (ms)", color="#a6adc8", fontsize=8)
            ax.set_ylabel(ylabel, color=color, fontsize=9)
            ax.grid(color="#313244", linestyle="--", linewidth=0.6)

        # animated=True excluiria as linhas do draw() normal, quebrando o blitting manual
        self.line_v, = self.ax_v.plot([], [], color="#89b4fa", linewidth=1.3)
        self.line_i, = self.ax_i.plot([], [], color="#f38ba8", linewidth=1.3)

        self.ax_v.set_xlim(0, 20)
        self.ax_i.set_xlim(0, 20)

        self.canvas_wave = FigureCanvasTkAgg(fig, master=parent)
        self.canvas_wave.get_tk_widget().grid(row=0, column=0, sticky="nswe")

        # Captura o background inicial para blitting
        fig.canvas.draw()
        self._bg_v = fig.canvas.copy_from_bbox(self.ax_v.bbox)
        self._bg_i = fig.canvas.copy_from_bbox(self.ax_i.bbox)

        # Invalida o background ao redimensionar
        self.canvas_wave.mpl_connect('resize_event', self._on_wave_resize)

    def _on_wave_resize(self, _event):
        self._bg_v = None
        self._bg_i = None

    # ── Aba: Harmônicos (FFT) ─────────────────────────────────────────────────
    def _build_harmonics_tab(self, parent):
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(0, weight=1)

        fig = plt.figure(figsize=(10, 4.5), facecolor="#1e1e2e")
        gs  = gridspec.GridSpec(2, 1, figure=fig, hspace=0.55,
                                left=0.08, right=0.97, top=0.93, bottom=0.12)

        self.ax_fft_v = fig.add_subplot(gs[0])
        self.ax_fft_i = fig.add_subplot(gs[1])

        for ax, title, color, ylabel in [
            (self.ax_fft_v, "Espectro Tensão (V)",   "#89b4fa", "V"),
            (self.ax_fft_i, "Espectro Corrente (A)", "#f38ba8", "A"),
        ]:
            ax.set_facecolor("#181825")
            ax.tick_params(colors="#a6adc8", labelsize=8)
            ax.set_title(title, color=color, fontsize=10, pad=4)
            ax.set_xlabel("Frequência (Hz)", color="#a6adc8", fontsize=8)
            ax.set_ylabel(ylabel, color=color, fontsize=9)
            ax.grid(color="#313244", linestyle="--", linewidth=0.6)
            ax.set_xlim(0, 720)  # até a 12ª harmônica de 60 Hz

            # Marcadores verticais das harmônicas ímpares (1, 3, 5, ...)
            for n in range(1, 13):
                lc = "#cba6f7" if n % 2 == 1 else "#45475a"
                ax.axvline(n * 60, color=lc, linewidth=0.7, linestyle=":", alpha=0.7)

        self.line_fft_v, = self.ax_fft_v.plot([], [], color="#89b4fa", linewidth=1.4)
        self.line_fft_i, = self.ax_fft_i.plot([], [], color="#f38ba8", linewidth=1.4)

        # Área preenchida sob o espectro
        self._fill_fft_v = self.ax_fft_v.fill_between([], [], alpha=0.25, color="#89b4fa")
        self._fill_fft_i = self.ax_fft_i.fill_between([], [], alpha=0.25, color="#f38ba8")

        self.canvas_harm = FigureCanvasTkAgg(fig, master=parent)
        self.canvas_harm.get_tk_widget().grid(row=0, column=0, sticky="nswe")

    # ── Aba: Calibração ADC ───────────────────────────────────────────────────
    def _build_calibration_tab(self, parent):
        parent.columnconfigure(0, weight=1)
        parent.columnconfigure(1, weight=1)
        parent.rowconfigure(1, weight=1)

        # Controles de modo
        ctrl = ttk.LabelFrame(parent, text="Modo de Operação do ESP32", padding=(8, 8))
        ctrl.grid(row=0, column=0, columnspan=2, sticky="ew", padx=8, pady=8)
        ctrl.columnconfigure(0, weight=1)
        ctrl.columnconfigure(1, weight=1)
        ctrl.columnconfigure(2, weight=1)

        ttk.Button(ctrl, text="● Modo Normal",
                   command=lambda: self._send_mode("NORMAL")).grid(
            row=0, column=0, padx=4, pady=4, sticky="we")
        ttk.Button(ctrl, text="⊕ Cal. Potenciômetro",
                   command=lambda: self._send_mode("CAL_POT")).grid(
            row=0, column=1, padx=4, pady=4, sticky="we")
        ttk.Button(ctrl, text="⇌ Cal. Pino-a-Pino",
                   command=lambda: self._send_mode("CAL_PIN")).grid(
            row=0, column=2, padx=4, pady=4, sticky="we")

        self.mode_status_var = tk.StringVar(value="Modo atual: NORMAL")
        ttk.Label(ctrl, textvariable=self.mode_status_var,
                  font=("Segoe UI", 9, "bold"), foreground="#f9e2af").grid(
            row=1, column=0, columnspan=3, sticky="w", padx=4, pady=(2, 0))

        # ── Painel CAL_POT ────────────────────────────────────────────────────
        pot = ttk.LabelFrame(parent,
                             text="Calibração por Potenciômetro  (mV brutos do ADC)",
                             padding=(10, 8))
        pot.grid(row=1, column=0, sticky="nsew", padx=(8, 4), pady=4)

        ttk.Label(pot,
                  text="Como usar:\n"
                       "1. Conecte a saída do pot ao pino V (GPIO34)\n"
                       "   e/ou ao pino I (GPIO35)\n"
                       "2. Envie 'Cal. Potenciômetro'\n"
                       "3. Gire o pot de 0 → 3,3 V\n"
                       "4. Verifique faixa linear: ~150–2450 mV\n"
                       "5. DC em repouso: ~1650 mV (VCC/2)",
                  foreground="#a6adc8", justify="left",
                  font=("Segoe UI", 8)).grid(row=0, column=0, columnspan=2,
                                              sticky="w", pady=(0, 8))

        for r, (lbl, attr) in enumerate([
            ("Canal V — DC offset (mV):",  "lbl_cal_v_dc"),
            ("Canal V — Pico-a-pico (mV):", "lbl_cal_v_pp"),
            ("Canal V — AC RMS (mV):",     "lbl_cal_v_ac"),
            ("Canal I — DC offset (mV):",  "lbl_cal_i_dc"),
            ("Canal I — Pico-a-pico (mV):", "lbl_cal_i_pp"),
            ("Canal I — AC RMS (mV):",     "lbl_cal_i_ac"),
        ]):
            ttk.Label(pot, text=lbl).grid(row=r+1, column=0, sticky="w", pady=3, padx=4)
            var = tk.StringVar(value="—")
            ttk.Label(pot, textvariable=var,
                      font=("Courier New", 11, "bold"),
                      foreground="#f9e2af").grid(row=r+1, column=1, sticky="w", padx=8)
            setattr(self, attr, var)

        # ── Painel CAL_PIN ────────────────────────────────────────────────────
        pin = ttk.LabelFrame(parent,
                             text="Calibração Pino-a-Pino  (mesmo sinal em V e I)",
                             padding=(10, 8))
        pin.grid(row=1, column=1, sticky="nsew", padx=(4, 8), pady=4)

        ttk.Label(pin,
                  text="Como usar:\n"
                       "1. Conecte o mesmo sinal AC a GPIO34 e GPIO35\n"
                       "   (use resistores de proteção ~10 kΩ)\n"
                       "2. Envie 'Cal. Pino-a-Pino'\n"
                       "3. Ratio = 1,000 → ganhos iguais\n"
                       "4. Ratio ≠ 1 → diferença de ganho entre canais\n"
                       "   (normal: < 2 % no ESP32 com eFuse cal)",
                  foreground="#a6adc8", justify="left",
                  font=("Segoe UI", 8)).grid(row=0, column=0, columnspan=2,
                                              sticky="w", pady=(0, 8))

        for r, (lbl, attr, color) in enumerate([
            ("Canal V — DC offset (mV):",  "lbl_pin_v_dc",    "#f9e2af"),
            ("Canal I — DC offset (mV):",  "lbl_pin_i_dc",    "#f9e2af"),
            ("Canal V — AC RMS (mV):",     "lbl_pin_v_ac",    "#89b4fa"),
            ("Canal I — AC RMS (mV):",     "lbl_pin_i_ac",    "#f38ba8"),
            ("Ratio AC  V / I:",           "lbl_pin_ratio",   "#a6e3a1"),
            ("Diferença de ganho:",        "lbl_pin_gain_err","#fab387"),
        ]):
            ttk.Label(pin, text=lbl).grid(row=r+1, column=0, sticky="w", pady=3, padx=4)
            var = tk.StringVar(value="—")
            ttk.Label(pin, textvariable=var,
                      font=("Courier New", 11, "bold"),
                      foreground=color).grid(row=r+1, column=1, sticky="w", padx=8)
            setattr(self, attr, var)

    # ── Envio de modo ─────────────────────────────────────────────────────────
    def _send_mode(self, mode: str):
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showwarning("Sem Conexão", "Conecte à serial primeiro.")
            return
        try:
            self.serial_port.write((mode + "\n").encode())
            self.mode_status_var.set(f"Modo atual: {mode}")
        except Exception as e:
            messagebox.showerror("Erro", str(e))

    def _apply_multimeter_calib(self):
        msgs = []
        v_ref = self._safe_get(self.v_ref_var, 0.0)
        i_ref = self._safe_get(self.i_ref_var, 0.0)
        if v_ref > 0 and self._v_rms_raw > 0:
            fv = v_ref / self._v_rms_raw
            self.v_calib_var.set(round(fv, 6))
            msgs.append(f"Fator V = {fv:.4f}")
        if i_ref > 0 and self._i_rms_raw > 0:
            fi = i_ref / self._i_rms_raw
            self.i_calib_var.set(round(fi, 6))
            msgs.append(f"Fator I = {fi:.4f}")
        if msgs:
            self.calib_result_var.set(" | ".join(msgs))

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
            # timeout curto: read() retorna rápido se não há dados
            self.serial_port = serial.Serial(port, SERIAL_BAUD, timeout=0.05)
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

    # ── Thread de leitura: parsing binário com buffer acumulador ─────────────
    def _read_serial(self):
        buf = bytearray()
        while self.is_reading:
            try:
                chunk = self.serial_port.read(512)
                if chunk:
                    buf.extend(chunk)
                    self._parse_frames(buf)
            except Exception:
                pass

    def _parse_frames(self, buf: bytearray):
        """Extrai frames do buffer in-place. buf é modificado (bytes consumidos)."""
        while True:
            # Localiza magic 0xAB 0xCD
            idx = -1
            for i in range(len(buf) - 1):
                if buf[i] == 0xAB and buf[i + 1] == 0xCD:
                    idx = i
                    break
            if idx == -1:
                # Sem magic: descarta tudo exceto o último byte (pode ser start do magic)
                del buf[:-1]
                return
            if idx > 0:
                del buf[:idx]  # descarta lixo antes do magic

            if len(buf) < 3:
                return  # aguarda type byte

            ftype = buf[2]

            # ── DATA ──────────────────────────────────────────────────────────
            if ftype == FRAME_DATA:
                if len(buf) < 29:
                    return  # aguarda header fixo (sem amostras)
                n    = buf[27]
                step = buf[28]
                frame_size = 29 + n * 4 + 2
                if len(buf) < frame_size:
                    return  # aguarda amostras + footer
                fp = 29 + n * 4
                if buf[fp] != 0xEF or buf[fp + 1] != 0xFE:
                    del buf[:2]  # magic falso, tenta próximo
                    continue
                # Métricas: offsets 3–26 (6 floats)
                v_rms, i_rms, pf, p_act, p_rea, p_app = struct.unpack_from('<6f', buf, 3)
                # Amostras: int16 pairs a partir do offset 29
                v_data, i_data = [], []
                off = 29
                for _ in range(n):
                    v, ic = struct.unpack_from('<hh', buf, off)
                    v_data.append(float(v))
                    i_data.append(float(ic))
                    off += 4
                del buf[:frame_size]
                self._data_queue.put(('data', v_rms, i_rms, pf, p_act, p_rea, p_app,
                                      v_data, i_data, step))

            # ── CALPOT ────────────────────────────────────────────────────────
            elif ftype == FRAME_CALPOT:
                if len(buf) < CALPOT_SIZE:
                    return
                fp = CALPOT_SIZE - 2
                if buf[fp] != 0xEF or buf[fp + 1] != 0xFE:
                    del buf[:2]
                    continue
                vals = struct.unpack_from('<6f', buf, 3)
                del buf[:CALPOT_SIZE]
                self._data_queue.put(('calpot', *vals))

            # ── CALPIN ────────────────────────────────────────────────────────
            elif ftype == FRAME_CALPIN:
                if len(buf) < CALPIN_SIZE:
                    return
                fp = CALPIN_SIZE - 2
                if buf[fp] != 0xEF or buf[fp + 1] != 0xFE:
                    del buf[:2]
                    continue
                vals = struct.unpack_from('<4f', buf, 3)
                del buf[:CALPIN_SIZE]
                self._data_queue.put(('calpin', *vals))

            else:
                del buf[:2]  # tipo desconhecido

    # ── Poll da fila (UI thread, a cada 20 ms = 50 Hz) ───────────────────────
    def _poll_queue(self):
        try:
            while True:
                item = self._data_queue.get_nowait()
                if item[0] == 'data':
                    _, v_rms, i_rms, pf, p_act, p_rea, p_app, v_data, i_data, step = item
                    self._update_measurements(v_rms, i_rms, pf, p_act, p_rea, p_app)
                    self._update_waveforms(v_data, i_data, step)
                    self._update_harmonics(v_data, i_data, step)
                elif item[0] == 'calpot':
                    _, v_dc, v_pp, v_ac, i_dc, i_pp, i_ac = item
                    self.lbl_cal_v_dc.set(f"{v_dc:.1f}")
                    self.lbl_cal_v_pp.set(f"{v_pp:.1f}")
                    self.lbl_cal_v_ac.set(f"{v_ac:.2f}")
                    self.lbl_cal_i_dc.set(f"{i_dc:.1f}")
                    self.lbl_cal_i_pp.set(f"{i_pp:.1f}")
                    self.lbl_cal_i_ac.set(f"{i_ac:.2f}")
                elif item[0] == 'calpin':
                    _, v_dc, i_dc, v_ac, i_ac = item
                    ratio = (v_ac / i_ac) if i_ac > 0.1 else float('inf')
                    gain_err_pct = abs(ratio - 1.0) * 100.0
                    self.lbl_pin_v_dc.set(f"{v_dc:.1f}")
                    self.lbl_pin_i_dc.set(f"{i_dc:.1f}")
                    self.lbl_pin_v_ac.set(f"{v_ac:.2f}")
                    self.lbl_pin_i_ac.set(f"{i_ac:.2f}")
                    self.lbl_pin_ratio.set(f"{ratio:.4f}")
                    self.lbl_pin_gain_err.set(f"{gain_err_pct:.2f} %")
        except queue.Empty:
            pass
        self.root.after(20, self._poll_queue)

    @staticmethod
    def _safe_get(var: tk.DoubleVar, default: float) -> float:
        try:
            return var.get()
        except tk.TclError:
            return default

    # ── Atualização de métricas ───────────────────────────────────────────────
    def _update_measurements(self, v_rms_raw, i_rms_raw, pf_raw,
                              p_act_raw, p_rea_raw, p_app_raw):
        self._v_rms_raw = v_rms_raw
        self._i_rms_raw = i_rms_raw

        vc = self._safe_get(self.v_calib_var,  1.0)
        vo = self._safe_get(self.v_offset_var, 0.0)
        ic = self._safe_get(self.i_calib_var,  1.0)
        io = self._safe_get(self.i_offset_var, 0.0)

        v_rms  = v_rms_raw * vc + vo
        i_rms  = i_rms_raw * ic + io
        p_act  = p_act_raw * vc * ic
        p_rea  = p_rea_raw * vc * ic
        p_app  = p_app_raw * vc * ic
        pf     = max(-1.0, min(1.0, pf_raw))
        angle  = math.degrees(math.acos(abs(pf)))

        self.lbl_v_rms.config(   text=f"{v_rms:.2f} V")
        self.lbl_i_rms.config(   text=f"{i_rms:.3f} A")
        self.lbl_p_active.config(text=f"{p_act:.2f} W")
        self.lbl_p_react.config( text=f"{p_rea:.2f} VAr")
        self.lbl_p_appar.config( text=f"{p_app:.2f} VA")
        self.lbl_pf.config(      text=f"{pf:.3f}")
        self.lbl_angle.config(   text=f"{angle:.1f} °")

    # ── Formas de onda com blitting ───────────────────────────────────────────
    def _update_waveforms(self, v_data: list, i_data: list, step: int):
        if not v_data:
            return

        vc = self._safe_get(self.v_calib_var,  1.0)
        ic = self._safe_get(self.i_calib_var,  1.0)
        ZMPT = 204.1 * vc
        SCT  = (2000.0 / 22.0) * ic
        vo   = self._safe_get(self.v_offset_var, 0.0)
        io   = self._safe_get(self.i_offset_var, 0.0)

        dc_v = sum(v_data) / len(v_data)
        dc_i = sum(i_data) / len(i_data) if i_data else 0.0
        v_plot = [(c - dc_v) / 1000.0 * ZMPT + vo for c in v_data]
        i_plot = [(c - dc_i) / 1000.0 * SCT  + io for c in i_data]

        dt_ms  = DT_BASE_MS * step
        t_axis = [k * dt_ms for k in range(len(v_plot))]

        new_xlim = (0.0, t_axis[-1] if t_axis else 1.0)
        new_vlim = self._nice_lim(v_plot)
        new_ilim = self._nice_lim(i_plot)

        needs_full = (
            self._bg_v is None
            or abs(new_xlim[1] - self._last_xlim[1]) > 0.5
            or not self._lims_close(new_vlim, self._last_vlim)
            or not self._lims_close(new_ilim, self._last_ilim)
        )

        if needs_full:
            self._last_xlim = new_xlim
            self._last_vlim = new_vlim
            self._last_ilim = new_ilim
            self.ax_v.set_xlim(*new_xlim)
            self.ax_v.set_ylim(*new_vlim)
            self.ax_i.set_xlim(*new_xlim)
            self.ax_i.set_ylim(*new_ilim)
            # Captura background SEM linhas para o blitting funcionar corretamente
            self.line_v.set_data([], [])
            self.line_i.set_data([], [])
            self.canvas_wave.draw()
            self._bg_v = self.canvas_wave.copy_from_bbox(self.ax_v.bbox)
            self._bg_i = self.canvas_wave.copy_from_bbox(self.ax_i.bbox)

        # Sempre blit as linhas sobre o background limpo
        self.line_v.set_data(t_axis, v_plot)
        self.line_i.set_data(t_axis[:len(i_plot)], i_plot)

        self.canvas_wave.restore_region(self._bg_v)
        self.ax_v.draw_artist(self.line_v)
        self.canvas_wave.blit(self.ax_v.bbox)

        self.canvas_wave.restore_region(self._bg_i)
        self.ax_i.draw_artist(self.line_i)
        self.canvas_wave.blit(self.ax_i.bbox)

        self.canvas_wave.flush_events()

    @staticmethod
    def _nice_lim(data, margin=0.18):
        if not data:
            return (-1.0, 1.0)
        mn, mx = min(data), max(data)
        span   = max(mx - mn, 1e-3)
        center = (mn + mx) / 2.0
        half   = span * (0.5 + margin)
        return (center - half, center + half)

    @staticmethod
    def _lims_close(a, b, tol=0.08):
        span = abs(b[1] - b[0]) + 1e-9
        return abs(a[0] - b[0]) < tol * span and abs(a[1] - b[1]) < tol * span

    # ── Harmônicos (FFT via numpy) ────────────────────────────────────────────
    def _update_harmonics(self, v_data: list, i_data: list, step: int):
        n = len(v_data)
        if n < 16:
            return

        dt_s = (DT_BASE_MS * step) / 1000.0
        fs   = 1.0 / dt_s if dt_s > 0 else 7200.0

        vc = self._safe_get(self.v_calib_var, 1.0)
        ic = self._safe_get(self.i_calib_var, 1.0)
        ZMPT = 204.1 * vc
        SCT  = (2000.0 / 22.0) * ic

        dc_v = sum(v_data) / n
        dc_i = sum(i_data) / len(i_data) if i_data else 0.0
        v_ac = np.array([(c - dc_v) / 1000.0 * ZMPT for c in v_data], dtype=np.float32)
        i_ac = np.array([(c - dc_i) / 1000.0 * SCT  for c in i_data], dtype=np.float32)

        win  = np.hanning(n).astype(np.float32)
        fqs  = np.fft.rfftfreq(n, d=1.0 / fs)
        fv   = np.abs(np.fft.rfft(v_ac * win)) * 2.0 / n
        fi   = np.abs(np.fft.rfft(i_ac * win)) * 2.0 / n

        mask = fqs <= 720
        fqs_m = fqs[mask]
        fv_m  = fv[mask]
        fi_m  = fi[mask]

        # Atualiza linhas + área preenchida (sem cla, evita apagar marcadores)
        self.line_fft_v.set_data(fqs_m, fv_m)
        self.line_fft_i.set_data(fqs_m, fi_m)

        # Remove áreas antigas e desenha novas
        self._fill_fft_v.remove()
        self._fill_fft_i.remove()
        self._fill_fft_v = self.ax_fft_v.fill_between(fqs_m, fv_m, alpha=0.25, color="#89b4fa")
        self._fill_fft_i = self.ax_fft_i.fill_between(fqs_m, fi_m, alpha=0.25, color="#f38ba8")

        for ax in (self.ax_fft_v, self.ax_fft_i):
            ax.relim()
            ax.autoscale_view(scalex=False)

        # draw_idle agenda o redraw sem bloquear a UI
        self.canvas_harm.draw_idle()


if __name__ == "__main__":
    root = tk.Tk()
    app  = AmostradorGUI(root)
    root.protocol("WM_DELETE_WINDOW", lambda: sys.exit(0))
    root.mainloop()
