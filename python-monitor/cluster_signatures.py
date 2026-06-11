#!/usr/bin/env python3
"""
AccuEnergy — Cluster Signatures  (Etapa 1 do pipeline NILM)

Clusteriza eventos ΔP/ΔQ extraídos pelo nilm_engine.py, opcionalmente
enriquece com features harmônicas, e exporta dataset rotulado + signatures.json.

Uso:
    # Básico (só ΔP/ΔQ):
    python cluster_signatures.py --events eventos.csv

    # Com harmônicos:
    python cluster_signatures.py --events eventos.csv --harm harmonicas.csv

    # K fixo, sem prompt interativo (CI/automação):
    python cluster_signatures.py --events eventos.csv --k 8 --no-interactive

    # Comparação ablation para o artigo:
    python cluster_signatures.py --events eventos.csv --harm harmonicas.csv --out-dir resultados/
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from sklearn.cluster import KMeans
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import silhouette_score


# ── Carga ──────────────────────────────────────────────────────────────────────

def _load_events(path: str) -> pd.DataFrame:
    df = pd.read_csv(path, parse_dates=['ts'])
    for col in ('delta_p', 'delta_q'):
        if col not in df.columns:
            raise ValueError(f'Coluna obrigatória "{col}" ausente em {path}')
    return df.sort_values('ts').reset_index(drop=True)


def _load_harmonics(path: str) -> pd.DataFrame | None:
    if not path or not pathlib.Path(path).exists():
        return None
    df = pd.read_csv(path, parse_dates=['ts'])
    return df.sort_values('ts').reset_index(drop=True)


def _join_harmonics(events: pd.DataFrame, harm: pd.DataFrame,
                    tol_s: float) -> pd.DataFrame:
    """merge_asof: para cada evento, junta o frame harmônico mais próximo."""
    ev  = events.copy()
    h   = harm.copy()

    # Garante timestamps sem timezone para o merge
    for df in (ev, h):
        if df['ts'].dt.tz is not None:
            df['ts'] = df['ts'].dt.tz_localize(None)

    harm_cols = [c for c in h.columns if c != 'ts']
    merged = pd.merge_asof(
        ev, h[['ts'] + harm_cols],
        on='ts', direction='nearest',
        tolerance=pd.Timedelta(seconds=tol_s),
    )
    n_joined = merged[harm_cols[0]].notna().sum() if harm_cols else 0
    print(f'  Harmônicos unidos: {n_joined}/{len(merged)} eventos (tolerância {tol_s}s)')
    return merged


# ── Seleção de K ───────────────────────────────────────────────────────────────

def _select_k(Xs: np.ndarray, k_min: int, k_max: int,
              out_dir: pathlib.Path) -> int:
    scores, ks = [], range(k_min, min(k_max + 1, len(Xs)))
    for k in ks:
        labels = KMeans(n_clusters=k, n_init=20, random_state=42).fit_predict(Xs)
        scores.append(silhouette_score(Xs, labels))

    best_k = list(ks)[int(np.argmax(scores))]

    fig, ax = plt.subplots(figsize=(6, 3))
    ax.plot(list(ks), scores, 'o-', color='steelblue')
    ax.axvline(best_k, color='tomato', linestyle='--', label=f'K={best_k} (ótimo)')
    ax.set_xlabel('K (número de clusters)')
    ax.set_ylabel('Silhouette Score')
    ax.set_title('Seleção automática de K')
    ax.legend(); fig.tight_layout()
    p = out_dir / 'silhouette.png'
    fig.savefig(p, dpi=150); plt.close(fig)
    print(f'  Silhouette plot → {p}')
    return best_k


# ── Visualização ───────────────────────────────────────────────────────────────

def _plot_clusters(df: pd.DataFrame, labels: np.ndarray,
                   centroids_dp_dq: np.ndarray, label_map: dict,
                   out_dir: pathlib.Path):
    K = centroids_dp_dq.shape[0]
    cmap = plt.cm.get_cmap('tab20', K)

    fig, ax = plt.subplots(figsize=(9, 6))
    for k in range(K):
        mask = labels == k
        name = label_map.get(k, f'cluster_{k}')
        ax.scatter(df.loc[mask, 'delta_p'], df.loc[mask, 'delta_q'],
                   color=cmap(k), alpha=0.55, s=45, label=name)
        ax.annotate(name,
                    xy=(centroids_dp_dq[k, 0], centroids_dp_dq[k, 1]),
                    fontsize=7, ha='center', va='bottom',
                    bbox=dict(boxstyle='round,pad=0.2', fc='white', alpha=0.75))

    ax.scatter(centroids_dp_dq[:, 0], centroids_dp_dq[:, 1],
               marker='X', s=180, c='black', zorder=5)
    ax.axhline(0, color='gray', lw=0.5)
    ax.axvline(0, color='gray', lw=0.5)
    ax.set_xlabel('ΔP (W)'); ax.set_ylabel('ΔQ (VAr)')
    ax.set_title('Eventos NILM — clusters detectados')
    ax.legend(fontsize=7, ncol=2, loc='best')
    fig.tight_layout()
    p = out_dir / 'clusters.png'
    fig.savefig(p, dpi=150); plt.close(fig)
    print(f'  Cluster plot → {p}')


# ── Rotulagem interativa ────────────────────────────────────────────────────────

def _interactive_label(centroids_orig: np.ndarray) -> dict:
    K = centroids_orig.shape[0]
    print('\n── Centroides detectados ──────────────────────────────')
    print(f'  {"#":>3}  {"ΔP (W)":>10}  {"ΔQ (VAr)":>10}')
    for k in range(K):
        print(f'  {k:>3}  {centroids_orig[k, 0]:>10.1f}  {centroids_orig[k, 1]:>10.1f}')
    print('\nDigite o rótulo de cada cluster. Enter em branco = "ruido" (descartado).')
    label_map = {}
    for k in range(K):
        try:
            name = input(f'  [{k:>2}] ΔP={centroids_orig[k,0]:>8.1f}W  ΔQ={centroids_orig[k,1]:>8.1f}VAr  → ').strip()
        except (EOFError, KeyboardInterrupt):
            name = ''
        label_map[k] = name or 'ruido'
    return label_map


# ── Exportação ─────────────────────────────────────────────────────────────────

def _export_signatures(df_labeled: pd.DataFrame, label_map: dict,
                       centroids_orig: np.ndarray, out_path: pathlib.Path) -> dict:
    sigs: dict = {}
    for k, label in label_map.items():
        if label == 'ruido':
            continue
        grp = df_labeled[df_labeled['cluster'] == k]
        if grp.empty:
            continue
        dp_mean = float(grp['delta_p'].mean())
        dp_std  = float(grp['delta_p'].std(ddof=1)) if len(grp) > 1 else abs(dp_mean) * 0.20
        tol = round(min(0.45, max(0.10, dp_std / max(abs(dp_mean), 1.0) + 0.05)), 2)
        sigs[label] = {
            'dp':  round(dp_mean, 1),
            'dq':  round(float(grp['delta_q'].mean()), 1),
            'tol': tol,
        }
    out_path.write_text(json.dumps(sigs, indent=2, ensure_ascii=False), encoding='utf-8')
    print(f'  Assinaturas → {out_path}  ({len(sigs)} cargas)')
    return sigs


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description='AccuEnergy — Cluster Signatures (etapa 1 NILM)',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument('--events',       required=True, help='CSV de eventos (nilm_engine.py)')
    ap.add_argument('--harm',         default='',    help='CSV de harmônicos (mqtt_ingest.py)')
    ap.add_argument('--k',            type=int, default=0,
                    help='Número de clusters (0 = automático por silhouette)')
    ap.add_argument('--k-min',        type=int, default=3)
    ap.add_argument('--k-max',        type=int, default=14)
    ap.add_argument('--harm-tol',     type=float, default=5.0,
                    help='Janela (s) para join de harmônicos')
    ap.add_argument('--out-dir',      default='.', help='Diretório de saída')
    ap.add_argument('--no-interactive', action='store_true',
                    help='Numera os clusters sem perguntar rótulos')
    args = ap.parse_args()

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # ── Carrega ────────────────────────────────────────────────────────────────
    print(f'Carregando eventos: {args.events}')
    events = _load_events(args.events)
    print(f'  {len(events)} eventos')

    harm = _load_harmonics(args.harm)
    if harm is not None:
        print(f'Carregando harmônicos: {args.harm}  ({len(harm)} frames)')
        events = _join_harmonics(events, harm, args.harm_tol)

    # ── Features ───────────────────────────────────────────────────────────────
    base_feats = ['delta_p', 'delta_q']
    harm_feats = [c for c in
                  ['thd_v', 'thd_i', 'hv1', 'hv2', 'hv3', 'hv4', 'hv5',
                   'hi1', 'hi2', 'hi3', 'hi4', 'hi5']
                  if c in events.columns and events[c].notna().any()]
    feat_cols = base_feats + harm_feats

    df_valid = events[feat_cols].dropna()
    events_v = events.loc[df_valid.index].copy().reset_index(drop=True)
    X = df_valid.values
    print(f'Features: {feat_cols}  |  amostras válidas: {len(X)}')

    if len(X) < 4:
        print('Eventos insuficientes para clusterização (mínimo 4).', file=sys.stderr)
        sys.exit(1)

    # ── Clusterização ──────────────────────────────────────────────────────────
    scaler = StandardScaler()
    Xs = scaler.fit_transform(X)

    K = args.k
    if K == 0:
        print(f'Buscando K ótimo em [{args.k_min}, {args.k_max}]…')
        K = _select_k(Xs, args.k_min, args.k_max, out_dir)
        print(f'  K selecionado: {K}')

    km = KMeans(n_clusters=K, n_init=20, random_state=42)
    labels = km.fit_predict(Xs)
    events_v['cluster'] = labels

    # Centroides no espaço original
    centroids_orig    = scaler.inverse_transform(km.cluster_centers_)
    centroids_dp_dq   = centroids_orig[:, :2]   # primeiras 2 colunas = ΔP, ΔQ

    # ── Rotulagem ──────────────────────────────────────────────────────────────
    if args.no_interactive:
        label_map = {k: f'cluster_{k}' for k in range(K)}
    else:
        label_map = _interactive_label(centroids_orig)

    events_v['label'] = events_v['cluster'].map(label_map)

    # ── Saídas ─────────────────────────────────────────────────────────────────
    print('\nGerando saídas…')
    _plot_clusters(events_v, labels, centroids_dp_dq, label_map, out_dir)

    labeled_path = out_dir / 'eventos_rotulados.csv'
    events_v.to_csv(labeled_path, index=False)
    print(f'  Dataset rotulado → {labeled_path}')

    sigs_path = out_dir / 'signatures.json'
    _export_signatures(events_v, label_map, centroids_orig, sigs_path)

    # ── Resumo ─────────────────────────────────────────────────────────────────
    print('\n── Resumo de clusters ─────────────────────────────────')
    print(f'  {"Rótulo":30s}  {"n":>5}  {"ΔP médio (W)":>14}  {"ΔQ médio (VAr)":>15}')
    for k in range(K):
        mask = labels == k
        grp  = events_v[events_v['cluster'] == k]
        print(f'  {label_map[k]:30s}  {mask.sum():>5}  '
              f'{grp["delta_p"].mean():>14.1f}  {grp["delta_q"].mean():>15.1f}')


if __name__ == '__main__':
    main()
