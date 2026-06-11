#!/usr/bin/env python3
"""
AccuEnergy MQTT Ingestor — assina energia/medidor e energia/harmonicas
e salva CSV sequencial com timestamp UTC.

Uso:
    python mqtt_ingest.py
    python mqtt_ingest.py --output dados.csv --harm harmonicas.csv
"""

import argparse
import csv
import datetime
import json
import pathlib
import sys

import paho.mqtt.client as mqtt

# ── Broker local (Mosquitto) ──────────────────────────────────────────────────
_BROKER = '127.0.0.1'
_PORT   = 1883

CSV_HEADER = ['ts', 'vrms', 'irms', 'preal', 's', 'q', 'fp', 'kwh']
HARM_HEADER = (
    ['ts', 'thd_v', 'thd_i']
    + [f'hv{i}' for i in range(1, 51)]
    + [f'hi{i}' for i in range(1, 51)]
)


class Ingestor:
    def __init__(self, csv_path: str, harm_path: str | None):
        self._csv_path  = pathlib.Path(csv_path)
        self._harm_path = pathlib.Path(harm_path) if harm_path else None
        self._f_main: object  = None
        self._w_main: object  = None
        self._f_harm: object  = None
        self._w_harm: object  = None
        self._count = 0

    def open(self):
        new_main = not self._csv_path.exists()
        self._f_main = open(self._csv_path, 'a', newline='', encoding='utf-8')
        self._w_main = csv.writer(self._f_main)
        if new_main:
            self._w_main.writerow(CSV_HEADER)

        if self._harm_path:
            new_harm = not self._harm_path.exists()
            self._f_harm = open(self._harm_path, 'a', newline='', encoding='utf-8')
            self._w_harm = csv.writer(self._f_harm)
            if new_harm:
                self._w_harm.writerow(HARM_HEADER)

    def close(self):
        for f in (self._f_main, self._f_harm):
            if f:
                try:
                    f.close()
                except Exception:
                    pass

    def handle_medidor(self, payload: dict):
        ts = payload.get('ts') or datetime.datetime.now(datetime.timezone.utc).isoformat()
        self._w_main.writerow([
            ts,
            payload.get('vrms', ''), payload.get('irms', ''),
            payload.get('preal', ''), payload.get('s', ''),
            payload.get('q', ''),    payload.get('fp', ''),
            payload.get('kwh', ''),
        ])
        self._f_main.flush()
        self._count += 1

    def handle_harmonicas(self, payload: dict):
        if not self._w_harm:
            return
        ts    = payload.get('ts') or datetime.datetime.now(datetime.timezone.utc).isoformat()
        thd_v = payload.get('thd_v', '')
        thd_i = payload.get('thd_i', '')
        hv    = payload.get('harm_v') or [''] * 50
        hi    = payload.get('harm_i') or [''] * 50
        self._w_harm.writerow([ts, thd_v, thd_i] + list(hv) + list(hi))
        self._f_harm.flush()


def build_client(
    broker: str, port: int,
    topic_med: str, topic_harm: str | None,
    ingestor: Ingestor,
) -> mqtt.Client:

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            print(f'[MQTT] conectado a {broker}:{port}')
            client.subscribe(topic_med)
            if topic_harm:
                client.subscribe(topic_harm)
        else:
            print(f'[MQTT] falha de conexão rc={rc}')

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload)
            if msg.topic == topic_med:
                ingestor.handle_medidor(payload)
                sys.stdout.write(
                    f"\r[{ingestor._count:>6} frames]  "
                    f"P={float(payload.get('preal', 0)):>8.1f}W  "
                    f"I={float(payload.get('irms', 0)):>6.3f}A  "
                    f"kWh={float(payload.get('kwh', 0)):>8.3f}"
                )
                sys.stdout.flush()
            elif msg.topic == topic_harm:
                ingestor.handle_harmonicas(payload)
        except Exception as exc:
            print(f'\n[WARN] parse error: {exc}')

    def on_disconnect(client, userdata, rc):
        print(f'\n[MQTT] desconectado rc={rc} — reconectando automaticamente...')

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    return client


def main():
    ap = argparse.ArgumentParser(
        description='AccuEnergy MQTT Ingestor → CSV',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument('--broker',     default=_BROKER,            help='Host MQTT')
    ap.add_argument('--port',       type=int, default=_PORT,    help='Porta MQTT')
    ap.add_argument('--output',     default='medicoes.csv',     help='CSV de saída (medições)')
    ap.add_argument('--harm',       default='harmonicas.csv',   help='CSV de saída (harmônicos); vazio=não gravar')
    ap.add_argument('--topic-med',  default='energia/medidor',  help='Tópico de medições')
    ap.add_argument('--topic-harm', default='energia/harmonicas', help='Tópico de harmônicos')
    args = ap.parse_args()

    harm_path = args.harm or None
    ingestor  = Ingestor(args.output, harm_path)
    ingestor.open()

    client = build_client(
        args.broker, args.port,
        args.topic_med, args.topic_harm, ingestor,
    )
    client.connect(args.broker, args.port, keepalive=60)

    print(f'Gravando medições em: {args.output}')
    if harm_path:
        print(f'Gravando harmônicos em: {harm_path}')
    print('Ctrl+C para parar.\n')

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print('\nParando...')
    finally:
        ingestor.close()
        client.disconnect()
        print(f'Total gravado: {ingestor._count} frames → {args.output}')


if __name__ == '__main__':
    main()
