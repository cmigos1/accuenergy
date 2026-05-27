#!/usr/bin/env python3
"""
nilm_engine.py — NILM via Hart ΔP+ΔQ step detection
Assina energia/medidor, detecta eventos de liga/desliga por degrau em P e Q,
compara contra base de assinaturas e loga eventos em CSV.

Uso:
    python nilm_engine.py
    python nilm_engine.py --signatures assinaturas.json --out eventos.csv

Instalar dependências:
    pip install -r requirements.txt
"""
import argparse
import csv
import json
import signal
import ssl
import sys
from collections import deque
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt

BROKER_HOST = "b038913592104262bbc1d6c9c55f6a01.s1.eu.hivemq.cloud"
BROKER_PORT  = 8883
TOPIC_METER  = "energia/medidor"
MQTT_USER    = "accuenergy"
MQTT_PASS    = ">!wv;5Y9L4=h.ZX"

# Limiares de detecção de degrau (Hart 1992)
DELTA_P_THRESH = 10.0   # W — mínimo degrau em potência ativa
DELTA_Q_THRESH = 10.0   # VAr — mínimo degrau em reativo

# Janela deslizante para baseline estável (últimos N frames antes do evento)
BASELINE_LEN = 5

EVENT_COLS = ["ts", "delta_p", "delta_q", "match", "confidence", "p_before", "p_after", "q_before", "q_after"]

DEFAULT_SIGNATURES = {
    "lampada_incandescente_60W": {"delta_p": 60,   "delta_q": 0,    "tol_p": 8,  "tol_q": 5},
    "lampada_incandescente_100W":{"delta_p": 100,  "delta_q": 0,    "tol_p": 12, "tol_q": 5},
    "lampada_led_9W":            {"delta_p": 9,    "delta_q": 3,    "tol_p": 4,  "tol_q": 4},
    "ventilador_40W":            {"delta_p": 40,   "delta_q": 20,   "tol_p": 8,  "tol_q": 10},
    "ferro_eletrico_1000W":      {"delta_p": 1000, "delta_q": 0,    "tol_p": 80, "tol_q": 20},
    "microondas_1200W":          {"delta_p": 1200, "delta_q": 400,  "tol_p": 100,"tol_q": 80},
    "geladeira_compressor":      {"delta_p": 150,  "delta_q": 120,  "tol_p": 40, "tol_q": 50},
    "televisao_50W":             {"delta_p": 50,   "delta_q": 10,   "tol_p": 15, "tol_q": 8},
    "carregador_notebook_65W":   {"delta_p": 65,   "delta_q": 15,   "tol_p": 15, "tol_q": 10},
}


class NILMEngine:
    def __init__(self, signatures: dict, out_path: Path):
        self.signatures = signatures
        self.baseline_p: deque = deque(maxlen=BASELINE_LEN)
        self.baseline_q: deque = deque(maxlen=BASELINE_LEN)
        self.prev_p = None
        self.prev_q = None
        self.event_count = 0

        write_header = not out_path.exists()
        self._out = open(out_path, "a", newline="", encoding="utf-8")
        self._writer = csv.writer(self._out)
        if write_header:
            self._writer.writerow(EVENT_COLS)

    def close(self):
        self._out.close()

    def ingest(self, ts: str, p: float, q: float):
        if self.prev_p is None:
            self.prev_p = p
            self.prev_q = q
            self.baseline_p.append(p)
            self.baseline_q.append(q)
            return

        delta_p = p - self.prev_p
        delta_q = q - self.prev_q

        if abs(delta_p) >= DELTA_P_THRESH or abs(delta_q) >= DELTA_Q_THRESH:
            p_before = sum(self.baseline_p) / len(self.baseline_p) if self.baseline_p else self.prev_p
            q_before = sum(self.baseline_q) / len(self.baseline_q) if self.baseline_q else self.prev_q

            match, conf = self._match(delta_p, delta_q)
            self.event_count += 1
            print(f"[NILM] evento #{self.event_count} | ΔP={delta_p:+.1f}W ΔQ={delta_q:+.1f}VAr "
                  f"| match={match} ({conf:.0%}) | ts={ts}")

            self._writer.writerow([ts, f"{delta_p:.2f}", f"{delta_q:.2f}",
                                   match, f"{conf:.3f}",
                                   f"{p_before:.2f}", f"{p:.2f}",
                                   f"{q_before:.2f}", f"{q:.2f}"])
            self._out.flush()
            # Reinicia baseline após evento
            self.baseline_p.clear()
            self.baseline_q.clear()
        else:
            self.baseline_p.append(p)
            self.baseline_q.append(q)

        self.prev_p = p
        self.prev_q = q

    def _match(self, dp: float, dq: float):
        best_name  = "desconhecido"
        best_score = 0.0
        for name, sig in self.signatures.items():
            ep = abs(abs(dp) - sig["delta_p"]) / max(sig["tol_p"], 1)
            eq = abs(abs(dq) - sig["delta_q"]) / max(sig["tol_q"], 1)
            if ep <= 1.0 and eq <= 1.0:
                score = 1.0 - 0.5 * (ep + eq)
                if score > best_score:
                    best_score = score
                    # prefixo "liga" ou "desliga" conforme sinal de ΔP
                    prefix = "liga" if dp > 0 else "desliga"
                    best_name = f"{prefix}_{name}"
        return best_name, best_score


# ── Estado global ─────────────────────────────────────────────────────────────
_engine: NILMEngine = None
_msg_count = 0


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] conectado ({userdata['host']}:{userdata['port']})")
        client.subscribe(TOPIC_METER)
        print(f"[MQTT] assinando '{TOPIC_METER}'")
    else:
        print(f"[MQTT] falha rc={rc}", file=sys.stderr)


def _on_message(client, userdata, msg):
    global _msg_count
    try:
        data = json.loads(msg.payload)
        ts    = data.get("ts", datetime.utcnow().isoformat())
        p     = float(data.get("preal", 0))
        q     = float(data.get("q",     0))
        _engine.ingest(ts, p, q)
        _msg_count += 1
        if _msg_count % 500 == 0:
            print(f"[NILM] {_msg_count} frames processados | {_engine.event_count} eventos")
    except (json.JSONDecodeError, ValueError, KeyError) as exc:
        print(f"[MQTT] payload inválido: {exc}", file=sys.stderr)


def _on_disconnect(client, userdata, rc):
    if rc != 0:
        print(f"[MQTT] desconectado rc={rc} — reconectando...", file=sys.stderr)


def main():
    global _engine

    ap = argparse.ArgumentParser(description="NILM engine — detecção Hart ΔP+ΔQ")
    ap.add_argument("--host",       default=BROKER_HOST)
    ap.add_argument("--port",       type=int, default=BROKER_PORT)
    ap.add_argument("--signatures", default=None,
                    help="JSON com assinaturas de eletrodomésticos (usa padrão interno se omitido)")
    ap.add_argument("--out",        default=f"nilm_eventos_{datetime.now():%Y%m%d_%H%M%S}.csv")
    args = ap.parse_args()

    sigs = DEFAULT_SIGNATURES
    if args.signatures:
        sig_path = Path(args.signatures)
        if sig_path.exists():
            with open(sig_path, encoding="utf-8") as f:
                sigs = json.load(f)
            print(f"[NILM] {len(sigs)} assinaturas carregadas de '{sig_path}'")
        else:
            print(f"[NILM] AVISO: '{sig_path}' não encontrado — usando assinaturas padrão")
    else:
        print(f"[NILM] usando {len(sigs)} assinaturas padrão internas")

    out_path = Path(args.out)
    _engine  = NILMEngine(sigs, out_path)
    print(f"[NILM] eventos em '{out_path}' | limiares ΔP≥{DELTA_P_THRESH}W ΔQ≥{DELTA_Q_THRESH}VAr")

    userdata = {"host": args.host, "port": args.port}
    client = mqtt.Client(userdata=userdata)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.tls_set(tls_version=ssl.PROTOCOL_TLS)
    client.on_connect    = _on_connect
    client.on_message    = _on_message
    client.on_disconnect = _on_disconnect
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    client.connect(args.host, args.port, keepalive=60)

    def _shutdown(sig, frame):
        print(f"\n[NILM] encerrando. {_msg_count} frames | {_engine.event_count} eventos em '{out_path}'")
        _engine.close()
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT,  _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    client.loop_forever()


if __name__ == "__main__":
    main()
