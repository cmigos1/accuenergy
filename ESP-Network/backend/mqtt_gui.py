#!/usr/bin/env python3
"""
mqtt_gui.py — Interface gráfica do ingestor MQTT ESP-Network

Uso: python mqtt_gui.py
Deps: pip install -r requirements.txt  (precisa de matplotlib)
"""
import csv
import json
import queue
import ssl
import time
from collections import deque
from datetime import datetime
from pathlib import Path
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import paho.mqtt.client as mqtt

# ── Defaults ──────────────────────────────────────────────────────────────────
DEFAULT_HOST  = "b038913592104262bbc1d6c9c55f6a01.s1.eu.hivemq.cloud"
DEFAULT_PORT  = 8883
DEFAULT_TOPIC = "energia/medidor"
MQTT_USER     = "accuenergy"
MQTT_PASS     = ">!wv;5Y9L4=h.ZX"
CHART_POINTS  = 120          # ~60 s a ~2 Hz
UPDATE_MS     = 300          # refresh da UI

CSV_COLS = ["ts", "vrms", "irms", "preal", "s", "q", "fp", "kwh"]

# ── Paleta ────────────────────────────────────────────────────────────────────
BG      = "#1e1e2e"
BG2     = "#2a2a3e"
BG3     = "#313149"
FG      = "#cdd6f4"
ACCENT  = "#89b4fa"
GREEN   = "#a6e3a1"
YELLOW  = "#f9e2af"
RED     = "#f38ba8"
SUBTEXT = "#6c7086"
FONT    = ("Consolas", 10)
FONT_SM = ("Consolas", 9)
FONT_LG = ("Consolas", 16, "bold")


def _lbl(parent, text, fg=FG, font=FONT, **kw):
    return tk.Label(parent, text=text, bg=parent["bg"], fg=fg, font=font, **kw)


def _btn(parent, text, cmd, bg=ACCENT, fg=BG, **kw):
    return tk.Button(parent, text=text, command=cmd, bg=bg, fg=fg,
                     font=FONT, relief="flat", padx=10, cursor="hand2", **kw)


def _entry(parent, var, width=18):
    return tk.Entry(parent, textvariable=var, width=width,
                    bg=BG, fg=FG, insertbackground=FG,
                    relief="flat", font=FONT)


# ── App ───────────────────────────────────────────────────────────────────────
class MqttGui:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("ESP-Network — Monitor de Energia")
        self.root.configure(bg=BG)
        self.root.minsize(900, 700)

        self._q: queue.Queue = queue.Queue()
        self._client = None          # mqtt.Client
        self._connected = False
        self._recording = False
        self._csv_file = None
        self._csv_writer = None
        self._msg_count = 0
        self._new_data = False
        self._t_start = time.monotonic()

        self._t_buf: deque = deque(maxlen=CHART_POINTS)
        self._p_buf: deque = deque(maxlen=CHART_POINTS)

        self._build_ui()
        self._schedule_update()

    # ── Construção da UI ──────────────────────────────────────────────────────
    def _build_ui(self):
        ttk.Style().theme_use("clam")

        # ── Barra de conexão ──────────────────────────────────────────────────
        top = tk.Frame(self.root, bg=BG2, pady=8)
        top.pack(fill="x")

        _lbl(top, "Broker:", fg=SUBTEXT).pack(side="left", padx=(12, 4))
        self._v_host = tk.StringVar(value=DEFAULT_HOST)
        _entry(top, self._v_host, 18).pack(side="left", padx=2)

        _lbl(top, "Porta:", fg=SUBTEXT).pack(side="left", padx=(8, 4))
        self._v_port = tk.StringVar(value=str(DEFAULT_PORT))
        _entry(top, self._v_port, 6).pack(side="left", padx=2)

        _lbl(top, "Tópico:", fg=SUBTEXT).pack(side="left", padx=(8, 4))
        self._v_topic = tk.StringVar(value=DEFAULT_TOPIC)
        _entry(top, self._v_topic, 24).pack(side="left", padx=2)

        self._btn_con = _btn(top, "Conectar", self._toggle_connect)
        self._btn_con.pack(side="left", padx=(16, 4))

        self._lbl_status = _lbl(top, "● Desconectado", fg=SUBTEXT)
        self._lbl_status.pack(side="left", padx=12)

        # ── Métricas ──────────────────────────────────────────────────────────
        grid = tk.Frame(self.root, bg=BG, pady=8)
        grid.pack(fill="x", padx=10)

        fields = [
            ("Vrms",   "V",    "vrms",  ACCENT),
            ("Irms",   "A",    "irms",  ACCENT),
            ("FP",     "",     "fp",    YELLOW),
            ("Preal",  "W",    "preal", GREEN),
            ("S",      "VA",   "s",     FG),
            ("Q",      "VAr",  "q",     FG),
            ("kWh",    "kWh",  "kwh",   YELLOW),
            ("Frames", "",     "cnt",   SUBTEXT),
        ]
        self._mvars: dict = {}
        for col, (label, unit, key, color) in enumerate(fields):
            card = tk.Frame(grid, bg=BG2, padx=10, pady=6)
            card.grid(row=0, column=col, padx=4, sticky="ew")
            grid.columnconfigure(col, weight=1)
            _lbl(card, label, fg=SUBTEXT, font=FONT_SM).pack()
            var = tk.StringVar(value="—")
            self._mvars[key] = var
            tk.Label(card, textvariable=var, bg=BG2, fg=color,
                     font=FONT_LG).pack()
            if unit:
                _lbl(card, unit, fg=SUBTEXT, font=FONT_SM).pack()

        # ── Último pacote recebido ────────────────────────────────────────────
        pkt_frame = tk.Frame(self.root, bg=BG2, padx=8, pady=6)
        pkt_frame.pack(fill="x", padx=10, pady=(0, 4))

        hdr = tk.Frame(pkt_frame, bg=BG2)
        hdr.pack(fill="x")
        _lbl(hdr, "Último pacote MQTT", fg=SUBTEXT, font=FONT_SM).pack(side="left")
        self._lbl_pkt_topic = _lbl(hdr, "", fg=ACCENT, font=FONT_SM)
        self._lbl_pkt_topic.pack(side="left", padx=(8, 0))
        self._lbl_pkt_age = _lbl(hdr, "", fg=SUBTEXT, font=FONT_SM)
        self._lbl_pkt_age.pack(side="right")

        self._pkt_text = tk.Text(
            pkt_frame, height=3, bg=BG, fg=FG, font=FONT_SM,
            relief="flat", state="disabled", wrap="none")
        self._pkt_text.pack(fill="x", pady=(4, 0))
        self._pkt_text.tag_config("key",   foreground=ACCENT)
        self._pkt_text.tag_config("val",   foreground=FG)
        self._pkt_text.tag_config("ts",    foreground=SUBTEXT)
        self._pkt_last_time: float = 0.0

        # ── Gráfico ───────────────────────────────────────────────────────────
        chart_frame = tk.Frame(self.root, bg=BG)
        chart_frame.pack(fill="both", expand=True, padx=10, pady=(0, 4))

        self._fig = Figure(figsize=(8, 2.8), dpi=96, facecolor=BG)
        self._ax  = self._fig.add_subplot(111, facecolor=BG2)
        self._ax.tick_params(colors=SUBTEXT, labelsize=8)
        for spine in self._ax.spines.values():
            spine.set_color(BG3)
        self._ax.set_xlabel("tempo (s)", color=SUBTEXT, fontsize=8)
        self._ax.set_ylabel("Preal (W)", color=SUBTEXT, fontsize=8)
        self._ax.grid(color=BG3, linewidth=0.5, linestyle="--")
        self._line, = self._ax.plot([], [], color=GREEN, linewidth=1.6)
        self._fig.tight_layout(pad=1.2)

        self._canvas = FigureCanvasTkAgg(self._fig, master=chart_frame)
        self._canvas.get_tk_widget().pack(fill="both", expand=True)

        # ── Barra CSV ─────────────────────────────────────────────────────────
        csv_bar = tk.Frame(self.root, bg=BG2, pady=7)
        csv_bar.pack(fill="x")

        _lbl(csv_bar, "CSV:", fg=SUBTEXT).pack(side="left", padx=(12, 4))
        self._v_csv = tk.StringVar(
            value=f"medidor_{datetime.now():%Y%m%d_%H%M%S}.csv")
        _entry(csv_bar, self._v_csv, 38).pack(side="left", padx=2)
        _btn(csv_bar, "…", self._browse_csv, bg=BG3, fg=FG).pack(
            side="left", padx=2)

        self._btn_rec = tk.Button(
            csv_bar, text="⏺  Gravar: OFF", command=self._toggle_recording,
            bg=BG, fg=SUBTEXT, font=FONT, relief="flat",
            padx=12, cursor="hand2")
        self._btn_rec.pack(side="left", padx=16)

        self._lbl_ts = _lbl(csv_bar, "", fg=SUBTEXT, font=FONT_SM)
        self._lbl_ts.pack(side="right", padx=12)

        # ── Log ───────────────────────────────────────────────────────────────
        log_frame = tk.Frame(self.root, bg=BG)
        log_frame.pack(fill="x", padx=10, pady=(0, 8))

        self._log = scrolledtext.ScrolledText(
            log_frame, height=5, bg=BG2, fg=SUBTEXT,
            font=FONT_SM, relief="flat", state="disabled", wrap="word")
        self._log.pack(fill="x")

        # tags de cor para o log
        self._log.tag_config("green",  foreground=GREEN)
        self._log.tag_config("yellow", foreground=YELLOW)
        self._log.tag_config("red",    foreground=RED)
        self._log.tag_config("dim",    foreground=SUBTEXT)

    # ── Log ───────────────────────────────────────────────────────────────────
    def _log_msg(self, text: str, tag: str = ""):
        ts = datetime.now().strftime("%H:%M:%S")
        self._log.configure(state="normal")
        self._log.insert("end", f"[{ts}] ", "dim")
        self._log.insert("end", text + "\n", tag or "")
        self._log.see("end")
        self._log.configure(state="disabled")

    # ── MQTT ──────────────────────────────────────────────────────────────────
    def _toggle_connect(self):
        if self._connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        host  = self._v_host.get().strip()
        topic = self._v_topic.get().strip()
        try:
            port = int(self._v_port.get())
        except ValueError:
            self._log_msg("Porta inválida.", "red")
            return

        self._client = mqtt.Client()
        self._client.username_pw_set(MQTT_USER, MQTT_PASS)
        self._client.tls_set(tls_version=ssl.PROTOCOL_TLS)
        self._client.on_connect    = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message    = self._on_message
        self._client.reconnect_delay_set(1, 30)

        try:
            self._client.connect_async(host, port, keepalive=60)
            self._client.loop_start()
            self._log_msg(f"Conectando a {host}:{port} …")
        except Exception as exc:
            self._log_msg(f"Erro: {exc}", "red")

    def _disconnect(self):
        if self._client:
            self._client.loop_stop()
            try:
                self._client.disconnect()
            except Exception:
                pass
        self._connected = False
        self._set_status(False)
        self._log_msg("Desconectado.")

    def _on_connect(self, client, userdata, flags, rc):
        topic = self._v_topic.get().strip()
        if rc == 0:
            client.subscribe(topic)
            self._q.put(("conn_ok", topic))
        else:
            self._q.put(("conn_err", rc))

    def _on_disconnect(self, client, userdata, rc):
        self._q.put(("disconn", rc))

    def _on_message(self, client, userdata, msg):
        self._q.put(("data", msg.payload))

    # ── CSV ───────────────────────────────────────────────────────────────────
    def _browse_csv(self):
        p = filedialog.asksaveasfilename(
            defaultextension=".csv",
            filetypes=[("CSV", "*.csv"), ("Todos", "*.*")],
            initialfile=self._v_csv.get())
        if p:
            self._v_csv.set(p)

    def _toggle_recording(self):
        if self._recording:
            self._stop_rec()
        else:
            self._start_rec()

    def _start_rec(self):
        path = Path(self._v_csv.get())
        write_hdr = not path.exists()
        try:
            self._csv_file   = open(path, "a", newline="", encoding="utf-8")
            self._csv_writer = csv.writer(self._csv_file)
            if write_hdr:
                self._csv_writer.writerow(CSV_COLS)
            self._recording = True
            self._btn_rec.config(text="⏹  Gravar: ON",
                                  fg=RED, bg=BG2, font=(*FONT, "bold"))
            self._log_msg(f"Gravando em '{path}'", "green")
        except OSError as exc:
            self._log_msg(f"Erro ao abrir CSV: {exc}", "red")

    def _stop_rec(self):
        self._recording = False
        if self._csv_file:
            self._csv_file.close()
            self._csv_file = self._csv_writer = None
        self._btn_rec.config(text="⏺  Gravar: OFF",
                              fg=SUBTEXT, bg=BG, font=FONT)
        self._log_msg("Gravação pausada.", "yellow")

    # ── Loop de atualização ───────────────────────────────────────────────────
    def _schedule_update(self):
        self._drain_queue()
        if self._new_data:
            self._redraw_chart()
            self._new_data = False
        # atualiza idade do último pacote
        if self._pkt_last_time > 0:
            age = time.monotonic() - self._pkt_last_time
            color = GREEN if age < 1.0 else (YELLOW if age < 5.0 else RED)
            self._lbl_pkt_age.config(text=f"{age:.1f}s atrás", fg=color)
        self.root.after(UPDATE_MS, self._schedule_update)

    def _drain_queue(self):
        try:
            while True:
                item = self._q.get_nowait()
                kind = item[0]
                if kind == "conn_ok":
                    self._connected = True
                    self._set_status(True)
                    self._log_msg(f"Conectado — assinando '{item[1]}'", "green")
                elif kind == "conn_err":
                    self._log_msg(f"Falha na conexão rc={item[1]}", "red")
                elif kind == "disconn":
                    self._connected = False
                    self._set_status(False)
                    if item[1] != 0:
                        self._log_msg(
                            f"Desconexão inesperada rc={item[1]}", "yellow")
                elif kind == "data":
                    self._handle_data(item[1])
        except queue.Empty:
            pass

    def _handle_data(self, raw: bytes):
        try:
            d = json.loads(raw)
        except json.JSONDecodeError:
            self._log_msg(f"JSON inválido: {raw[:80]}", "red")
            return

        self._msg_count += 1

        def _f(key, dec=2):
            try:
                return f"{float(d.get(key, 0)):.{dec}f}"
            except (TypeError, ValueError):
                return "—"

        self._mvars["vrms"].set(_f("vrms", 1))
        self._mvars["irms"].set(_f("irms", 3))
        self._mvars["fp"].set(_f("fp", 4))
        self._mvars["preal"].set(_f("preal", 1))
        self._mvars["s"].set(_f("s", 1))
        self._mvars["q"].set(_f("q", 1))
        self._mvars["kwh"].set(_f("kwh", 6))
        self._mvars["cnt"].set(str(self._msg_count))

        ts = d.get("ts", "—")
        self._lbl_ts.config(text=f"último: {ts}")

        try:
            preal = float(d.get("preal", 0))
        except (TypeError, ValueError):
            preal = 0.0
        self._t_buf.append(time.monotonic() - self._t_start)
        self._p_buf.append(preal)
        self._new_data = True

        # ── Atualiza painel "último pacote" ───────────────────────────────────
        self._pkt_last_time = time.monotonic()
        topic = self._v_topic.get().strip()
        self._lbl_pkt_topic.config(text=f"[{topic}]")
        self._pkt_text.configure(state="normal")
        self._pkt_text.delete("1.0", "end")
        # linha 1: timestamp + contagem
        self._pkt_text.insert("end", f"#{self._msg_count}  ts: ", "key")
        self._pkt_text.insert("end", f"{ts}\n", "ts")
        # linha 2: campos numéricos
        pairs = [
            ("vrms", _f("vrms", 2), "V"),
            ("irms", _f("irms", 4), "A"),
            ("preal", _f("preal", 2), "W"),
            ("s",    _f("s",    2), "VA"),
            ("q",    _f("q",    2), "VAr"),
            ("fp",   _f("fp",   4), ""),
            ("kwh",  _f("kwh",  6), "kWh"),
        ]
        for key, val, unit in pairs:
            self._pkt_text.insert("end", f"  {key}=", "key")
            self._pkt_text.insert("end", f"{val}{unit}", "val")
        self._pkt_text.insert("end", "\n")
        # linha 3: payload raw compacto
        self._pkt_text.insert("end", "  raw: ", "key")
        self._pkt_text.insert("end", raw.decode(errors="replace")[:200], "ts")
        self._pkt_text.configure(state="disabled")

        if self._recording and self._csv_writer:
            self._csv_writer.writerow([d.get(c, "") for c in CSV_COLS])
            self._csv_file.flush()

        # log a cada 10 mensagens (reduzido de 50)
        if self._msg_count % 10 == 0:
            self._log_msg(
                f"#{self._msg_count}  {ts}  "
                f"V={_f('vrms',1)}V  I={_f('irms',3)}A  "
                f"P={_f('preal',1)}W  FP={_f('fp',3)}  kWh={_f('kwh',6)}")

    def _redraw_chart(self):
        if len(self._t_buf) < 2:
            return
        t = list(self._t_buf)
        p = list(self._p_buf)
        self._line.set_data(t, p)
        self._ax.set_xlim(t[0], max(t[-1], t[0] + 1.0))
        lo, hi = min(p), max(p)
        pad = max((hi - lo) * 0.12, 5.0)
        self._ax.set_ylim(lo - pad, hi + pad)
        self._canvas.draw_idle()

    def _set_status(self, ok: bool):
        if ok:
            self._lbl_status.config(text="● Conectado", fg=GREEN)
            self._btn_con.config(text="Desconectar", bg=RED, fg=BG)
        else:
            self._lbl_status.config(text="● Desconectado", fg=SUBTEXT)
            self._btn_con.config(text="Conectar", bg=ACCENT, fg=BG)

    # ── Encerramento ──────────────────────────────────────────────────────────
    def on_close(self):
        if self._recording:
            self._stop_rec()
        if self._client:
            self._client.loop_stop()
            try:
                self._client.disconnect()
            except Exception:
                pass
        self.root.destroy()


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    root = tk.Tk()
    app = MqttGui(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
