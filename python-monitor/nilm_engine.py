#!/usr/bin/env python3
"""
AccuEnergy NILM Engine — detector Hart ΔP+ΔQ

Núcleo reutilizável (HartDetector) com dois modos de operação:
  --live   : assina MQTT e detecta eventos em tempo real
  --input  : processa CSV ou HDF5 já gravado em batch

Uso:
    # Offline (batch):
    python nilm_engine.py --input medicoes.csv --sigs assinaturas.json

    # Live (MQTT):
    python nilm_engine.py --live \\
        --broker abc.s2.eu.hivemq.cloud --user u --password p \\
        --sigs assinaturas.json --output eventos.csv

Formato do arquivo de assinaturas (JSON):
    {
      "Geladeira": {"dp": 180, "dq": 50,  "tol": 0.20},
      "Ar-Cond":   {"dp": 1200,"dq": 300, "tol": 0.15}
    }
    dp/dq: variação esperada [W / VAr]  (positivo=liga, negativo=desliga)
    tol:   tolerância relativa [0..1]    (0.20 = 20% de margem)
"""

from __future__ import annotations

import argparse
import csv
import datetime
import json
import pathlib
import sys
from collections import deque
from typing import Deque

import numpy as np


# ── Campos do CSV de saída ─────────────────────────────────────────────────────
EVENT_FIELDS = [
    'ts', 'delta_p', 'delta_q', 'match', 'confidence',
    'p_before', 'p_after', 'q_before', 'q_after',
]


# ── Núcleo de detecção ─────────────────────────────────────────────────────────
class HartDetector:
    """
    Detector de eventos Hart baseado em variação ΔP e ΔQ.

    Algoritmo:
      - Mantém janela deslizante de baseline_n amostras.
      - A cada nova amostra, computa ΔP = P_atual − mediana(baseline).
      - Se |ΔP| ≥ dp_thresh OU |ΔQ| ≥ dq_thresh → evento detectado.
      - Após evento, reinicia o buffer para evitar detecção repetida
        enquanto o sistema transita para o novo estado estável.
    """

    def __init__(
        self,
        dp_thresh:  float = 10.0,
        dq_thresh:  float = 10.0,
        baseline_n: int   = 5,
    ):
        self.dp_thresh  = dp_thresh
        self.dq_thresh  = dq_thresh
        self.baseline_n = baseline_n
        self._buf: Deque[tuple[float, float, object]] = deque(maxlen=baseline_n * 4)

    def feed(self, p: float, q: float, ts: object) -> dict | None:
        """
        Alimenta uma medição (P em W, Q em VAr, ts qualquer).
        Retorna dict de evento se detectado, None caso contrário.
        """
        self._buf.append((p, q, ts))
        if len(self._buf) < self.baseline_n + 1:
            return None

        window = list(self._buf)[-self.baseline_n - 1:-1]
        p_base = float(np.median([x[0] for x in window]))
        q_base = float(np.median([x[1] for x in window]))

        dp = p - p_base
        dq = q - q_base

        if abs(dp) >= self.dp_thresh or abs(dq) >= self.dq_thresh:
            # Limpa buffer: próximas leituras constroem novo baseline
            self._buf.clear()
            return {
                'ts':       str(ts),
                'delta_p':  round(dp, 2),
                'delta_q':  round(dq, 2),
                'p_before': round(p_base, 2),
                'p_after':  round(p, 2),
                'q_before': round(q_base, 2),
                'q_after':  round(q, 2),
            }
        return None

    @staticmethod
    def match(event: dict, signatures: dict) -> tuple[str, float]:
        """
        Encontra a assinatura mais próxima no espaço (ΔP, ΔQ) normalizado.
        Retorna (label, confiança [0..1]).
        Confiança = max(0, 1 − distância_normalizada).
        Retorna ('desconhecido', 0.0) se nenhuma assinatura dentro da tolerância.
        """
        if not signatures:
            return 'desconhecido', 0.0

        dp, dq = event['delta_p'], event['delta_q']
        best_label = 'desconhecido'
        best_dist  = float('inf')

        for label, sig in signatures.items():
            sdp = sig['dp']
            sdq = sig['dq']
            tol = sig.get('tol', 0.20)

            # Distância euclidiana normalizada pelos valores esperados
            norm_dp = (dp - sdp) / max(abs(sdp), 1.0)
            norm_dq = (dq - sdq) / max(abs(sdq), 1.0)
            dist    = (norm_dp ** 2 + norm_dq ** 2) ** 0.5

            if dist < best_dist:
                best_dist  = dist
                best_label = label
                best_tol   = tol

        if best_dist > best_tol * (2 ** 0.5):
            return 'desconhecido', 0.0

        confidence = max(0.0, 1.0 - best_dist)
        return best_label, round(confidence, 3)


# ── Escrita de eventos ─────────────────────────────────────────────────────────
def _open_event_csv(path: str) -> tuple[object, object]:
    p    = pathlib.Path(path)
    new  = not p.exists()
    f    = open(p, 'a', newline='', encoding='utf-8')
    w    = csv.DictWriter(f, fieldnames=EVENT_FIELDS)
    if new:
        w.writeheader()
    return f, w


def _enrich_and_write(event: dict, signatures: dict, writer: object, f: object):
    label, conf        = HartDetector.match(event, signatures)
    event['match']      = label
    event['confidence'] = conf
    writer.writerow(event)
    f.flush()
    return label, conf


# ── Modo offline (batch) ───────────────────────────────────────────────────────
def run_offline(
    input_path:  str,
    output_path: str,
    signatures:  dict,
    dp_thresh:   float,
    dq_thresh:   float,
    baseline_n:  int,
):
    import pandas as pd

    path = pathlib.Path(input_path)
    if path.suffix in ('.h5', '.hdf5'):
        with pd.HDFStore(str(path), mode='r') as store:
            keys = store.keys()
            if not keys:
                raise ValueError('HDF5 sem chaves.')
            df = store[keys[0]]
        # HDF5 NILMTK — colunas MultiIndex
        p_series = df[('power', 'active')].values   if ('power', 'active')   in df.columns else np.zeros(len(df))
        q_series = df[('power', 'reactive')].values if ('power', 'reactive') in df.columns else np.zeros(len(df))
        ts_series = df.index.astype(str).tolist()
    else:
        df = pd.read_csv(path, parse_dates=['ts'])
        p_series  = df['preal'].values
        q_series  = df['q'].values
        ts_series = df['ts'].astype(str).tolist()

    detector = HartDetector(dp_thresh, dq_thresh, baseline_n)
    f, writer = _open_event_csv(output_path)
    n_events  = 0

    for ts, p, q in zip(ts_series, p_series, q_series):
        ev = detector.feed(float(p), float(q), ts)
        if ev:
            label, conf = _enrich_and_write(ev, signatures, writer, f)
            n_events += 1
            sign = '+' if ev['delta_p'] >= 0 else ''
            print(f"[{ts}]  ΔP={sign}{ev['delta_p']:.1f}W  "
                  f"ΔQ={sign}{ev['delta_q']:.1f}VAr  → {label} ({conf:.0%})")

    f.close()
    print(f'\n{n_events} evento(s) → {output_path}')


# ── Modo live (MQTT) ───────────────────────────────────────────────────────────
def run_live(
    broker:     str,
    port:       int,
    user:       str,
    password:   str,
    topic:      str,
    output_path: str,
    signatures: dict,
    dp_thresh:  float,
    dq_thresh:  float,
    baseline_n: int,
):
    import paho.mqtt.client as mqtt

    detector   = HartDetector(dp_thresh, dq_thresh, baseline_n)
    f, writer  = _open_event_csv(output_path)
    frame_count = 0

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            print(f'[MQTT] conectado a {broker}:{port}')
            client.subscribe(topic)
        else:
            print(f'[MQTT] falha rc={rc}')

    def on_message(client, userdata, msg):
        nonlocal frame_count
        try:
            payload = json.loads(msg.payload)
            p   = float(payload.get('preal', 0))
            q   = float(payload.get('q',     0))
            ts  = payload.get('ts') or datetime.datetime.now(datetime.timezone.utc).isoformat()
            frame_count += 1

            ev = detector.feed(p, q, ts)
            if ev:
                label, conf = _enrich_and_write(ev, signatures, writer, f)
                sign = '+' if ev['delta_p'] >= 0 else ''
                print(f"\n[EVENTO #{frame_count}]  {ts}")
                print(f"  ΔP={sign}{ev['delta_p']:.1f}W  ΔQ={sign}{ev['delta_q']:.1f}VAr  → {label} ({conf:.0%})")
            else:
                sys.stdout.write(
                    f"\r[{frame_count:>6} frames]  P={p:>7.1f}W  Q={q:>7.1f}VAr"
                )
                sys.stdout.flush()
        except Exception as exc:
            print(f'\n[WARN] {exc}')

    def on_disconnect(client, userdata, rc):
        print(f'\n[MQTT] desconectado rc={rc} — reconectando...')

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    client.username_pw_set(user, password)
    client.tls_set()
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect(broker, port, keepalive=60)

    print(f'Gravando eventos em: {output_path}')
    print(f'Limiar: ΔP≥{dp_thresh}W  ΔQ≥{dq_thresh}VAr  baseline={baseline_n} frames')
    print('Ctrl+C para parar.\n')
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print('\nParando...')
    finally:
        f.close()
        client.disconnect()


# ── CLI ────────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(
        description='AccuEnergy NILM Engine — detector Hart ΔP+ΔQ',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--live',  action='store_true', help='Modo live via MQTT')
    mode.add_argument('--input', default='',          help='CSV ou HDF5 para modo offline')

    ap.add_argument('--output',   default='eventos.csv',      help='CSV de saída de eventos')
    ap.add_argument('--sigs',     default='',                  help='JSON de assinaturas manuais')
    ap.add_argument('--dp',       type=float, default=10.0,    help='Limiar ΔP [W]')
    ap.add_argument('--dq',       type=float, default=10.0,    help='Limiar ΔQ [VAr]')
    ap.add_argument('--baseline', type=int,   default=5,       help='Frames de baseline (mediana)')

    # Opções MQTT (só para --live)
    ap.add_argument('--broker',   default='', help='Host HiveMQ Cloud')
    ap.add_argument('--port',     type=int, default=8883, help='Porta TLS')
    ap.add_argument('--user',     default='', help='Usuário MQTT')
    ap.add_argument('--password', default='', help='Senha MQTT')
    ap.add_argument('--topic',    default='energia/medidor', help='Tópico MQTT')

    args = ap.parse_args()

    signatures: dict = {}
    if args.sigs:
        with open(args.sigs, encoding='utf-8') as fj:
            signatures = json.load(fj)
        print(f'Assinaturas carregadas: {list(signatures.keys())}')

    if args.live:
        if not all([args.broker, args.user, args.password]):
            ap.error('--live requer --broker, --user e --password')
        run_live(
            args.broker, args.port, args.user, args.password,
            args.topic, args.output, signatures,
            args.dp, args.dq, args.baseline,
        )
    else:
        run_offline(
            args.input, args.output, signatures,
            args.dp, args.dq, args.baseline,
        )


if __name__ == '__main__':
    main()
