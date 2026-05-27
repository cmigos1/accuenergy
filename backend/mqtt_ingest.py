#!/usr/bin/env python3
"""
mqtt_ingest.py — US10 + US11
Assina energia/medidor, recebe JSONs do ESP32 e salva em CSV sequencial.

Uso:
    python mqtt_ingest.py
    python mqtt_ingest.py --host 192.168.1.100 --out dados_20260511.csv

Instalar dependências:
    pip install -r requirements.txt
"""
import argparse
import csv
import json
import signal
import ssl
import sys
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt

BROKER_HOST = "b038913592104262bbc1d6c9c55f6a01.s1.eu.hivemq.cloud"
BROKER_PORT = 8883
TOPIC       = "energia/medidor"
MQTT_USER   = "accuenergy"
MQTT_PASS   = ">!wv;5Y9L4=h.ZX"
CSV_COLS    = ["ts", "vrms", "irms", "preal", "s", "q", "fp", "kwh"]

# ── Estado global ─────────────────────────────────────────────────────────────
_out_file   = None
_csv_writer = None
_msg_count  = 0


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] conectado ao broker ({userdata['host']}:{userdata['port']})")
        client.subscribe(TOPIC)
        print(f"[MQTT] assinando tópico '{TOPIC}'")
    else:
        print(f"[MQTT] falha na conexão rc={rc}", file=sys.stderr)


def _on_message(client, userdata, msg):
    global _msg_count
    try:
        data = json.loads(msg.payload)
        row = [data.get(col, "") for col in CSV_COLS]
        _csv_writer.writerow(row)
        _out_file.flush()
        _msg_count += 1
        if _msg_count % 100 == 0:
            print(f"[MQTT] {_msg_count} msgs | último ts: {data.get('ts', '?')} | kWh: {data.get('kwh', '?')}")
    except (json.JSONDecodeError, KeyError) as exc:
        print(f"[MQTT] payload inválido: {exc}", file=sys.stderr)


def _on_disconnect(client, userdata, rc):
    if rc != 0:
        print(f"[MQTT] desconectado inesperadamente rc={rc} — reconectando...", file=sys.stderr)


def main():
    global _out_file, _csv_writer

    ap = argparse.ArgumentParser(description="Assina MQTT e grava CSV")
    ap.add_argument("--host", default=BROKER_HOST, help="Host do broker MQTT")
    ap.add_argument("--port", type=int, default=BROKER_PORT)
    ap.add_argument("--topic", default=TOPIC)
    ap.add_argument(
        "--out",
        default=f"medidor_{datetime.now():%Y%m%d_%H%M%S}.csv",
        help="Arquivo CSV de saída (append se existir)"
    )
    args = ap.parse_args()

    out_path     = Path(args.out)
    write_header = not out_path.exists()
    _out_file    = open(out_path, "a", newline="", encoding="utf-8")
    _csv_writer  = csv.writer(_out_file)
    if write_header:
        _csv_writer.writerow(CSV_COLS)

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
        print(f"\n[MQTT] encerrando. {_msg_count} mensagens salvas em '{out_path}'")
        _out_file.close()
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    print(f"[MQTT] gravando em '{out_path}' | Ctrl+C para parar")
    client.loop_forever()


if __name__ == "__main__":
    main()
