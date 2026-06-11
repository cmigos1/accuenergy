#!/usr/bin/env python3
"""
AccuEnergy — NILM Classifier  (Etapa 2 do pipeline NILM)

Treina KNN, SVM e Random Forest sobre o dataset rotulado pelo cluster_signatures.py.
Gera todas as figuras e métricas necessárias para o artigo.

Uso:
    # Básico:
    python nilm_classifier.py --input eventos_rotulados.csv

    # Ablation study (P+Q vs P+Q+Harmônicos) — tabela do artigo:
    python nilm_classifier.py --input eventos_rotulados.csv --ablation

    # Features manuais:
    python nilm_classifier.py --input eventos_rotulados.csv --features dp dq thd_v thd_i

Saídas (em --out-dir):
    confusion_matrix.png          Matriz de confusão do melhor modelo
    feature_importance.png        Importância das features (Random Forest)
    comparacao_classificadores.png Barplot F1 por algoritmo (ablation)
    classification_report.txt     Precision / Recall / F1 por classe
    resultados.json               Métricas completas (para importar no artigo)
    nilm_<clf>_model.pkl          Modelo treinado (inferência online)
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import warnings

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.neighbors import KNeighborsClassifier
from sklearn.metrics import (ConfusionMatrixDisplay, classification_report,
                             confusion_matrix)
from sklearn.model_selection import StratifiedKFold, cross_validate
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import LabelEncoder, StandardScaler
from sklearn.svm import SVC
import joblib

warnings.filterwarnings('ignore')


# ── Classificadores avaliados ──────────────────────────────────────────────────

CLASSIFIERS: dict[str, object] = {
    'KNN': KNeighborsClassifier(n_neighbors=5, metric='euclidean'),
    'SVM': SVC(kernel='rbf', C=10, gamma='scale', probability=True),
    'RF':  RandomForestClassifier(n_estimators=300, random_state=42, n_jobs=-1),
}

# Aliases curtos → nome de coluna no CSV
_FEAT_ALIAS: dict[str, str] = {
    'dp': 'delta_p', 'dq': 'delta_q',
    **{f: f for f in ['thd_v', 'thd_i',
                       'hv1','hv2','hv3','hv4','hv5',
                       'hi1','hi2','hi3','hi4','hi5']},
}

FEATS_BASE = ['dp', 'dq']
FEATS_HARM = ['dp', 'dq', 'thd_v', 'thd_i',
              'hv1', 'hv2', 'hv3', 'hv4', 'hv5',
              'hi1', 'hi2', 'hi3', 'hi4', 'hi5']


# ── Carga e preparação ─────────────────────────────────────────────────────────

def _load(path: str, feat_aliases: list[str], label_col: str,
          drop_noise: bool) -> tuple[np.ndarray, np.ndarray, list[str]]:
    df = pd.read_csv(path)

    if label_col not in df.columns:
        raise ValueError(f'Coluna de rótulo "{label_col}" não encontrada.')

    if drop_noise:
        df = df[df[label_col] != 'ruido']

    df = df.dropna(subset=[label_col])

    # Resolve aliases
    feat_cols: list[str] = []
    for alias in feat_aliases:
        col = _FEAT_ALIAS.get(alias, alias)
        if col in df.columns:
            feat_cols.append(col)
        else:
            print(f'  [WARN] feature "{alias}" → "{col}" não encontrada, ignorada.')

    if not feat_cols:
        raise ValueError('Nenhuma feature válida.')

    df = df.dropna(subset=feat_cols)

    if len(df) < 10:
        raise ValueError(f'Amostras insuficientes ({len(df)}) após remoção de NaN.')

    X = df[feat_cols].values.astype(np.float32)
    y = df[label_col].values
    return X, y, feat_cols


# ── Avaliação cross-validation ─────────────────────────────────────────────────

def _cv_evaluate(X: np.ndarray, y_enc: np.ndarray, cv_folds: int,
                 fs_name: str) -> dict[str, dict]:
    n_min = np.bincount(y_enc).min()
    folds = min(cv_folds, int(n_min))
    if folds < 2:
        print(f'  [WARN] {fs_name}: classe com {n_min} amostra(s) — CV desativado.')
        return {}

    cv = StratifiedKFold(n_splits=folds, shuffle=True, random_state=42)
    results: dict[str, dict] = {}

    for name, clf in CLASSIFIERS.items():
        pipe = Pipeline([('scaler', StandardScaler()), ('clf', clf)])
        r = cross_validate(pipe, X, y_enc, cv=cv,
                           scoring=['f1_macro', 'precision_macro', 'recall_macro'])
        results[name] = {
            'f1':        round(float(r['test_f1_macro'].mean()), 4),
            'f1_std':    round(float(r['test_f1_macro'].std()),  4),
            'precision': round(float(r['test_precision_macro'].mean()), 4),
            'recall':    round(float(r['test_recall_macro'].mean()), 4),
        }
        m = results[name]
        print(f'    {name:>4}  F1={m["f1"]:.3f}±{m["f1_std"]:.3f}'
              f'  P={m["precision"]:.3f}  R={m["recall"]:.3f}')

    return results


# ── Figuras ────────────────────────────────────────────────────────────────────

def _plot_comparison(all_results: dict[str, dict], out_dir: pathlib.Path):
    feat_sets = list(all_results.keys())
    clf_names = list(CLASSIFIERS.keys())
    x     = np.arange(len(clf_names))
    width = 0.75 / len(feat_sets)
    cmap  = plt.cm.get_cmap('Set2', len(feat_sets))

    fig, ax = plt.subplots(figsize=(8, 5))
    for i, fs in enumerate(feat_sets):
        f1s = [all_results[fs].get(c, {}).get('f1', 0) for c in clf_names]
        bars = ax.bar(x + i * width, f1s, width, label=fs, color=cmap(i), edgecolor='white')
        for bar, v in zip(bars, f1s):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.008,
                    f'{v:.2f}', ha='center', va='bottom', fontsize=8)

    ax.set_xticks(x + width * (len(feat_sets) - 1) / 2)
    ax.set_xticklabels(clf_names, fontsize=11)
    ax.set_ylabel('F1-macro (cross-validation)')
    ax.set_ylim(0, 1.15)
    ax.set_title('Comparação de Classificadores NILM — AccuEnergy')
    ax.legend(title='Feature set', fontsize=9)
    ax.grid(axis='y', alpha=0.3)
    fig.tight_layout()
    p = out_dir / 'comparacao_classificadores.png'
    fig.savefig(p, dpi=150); plt.close(fig)
    print(f'  Comparação → {p}')


def _plot_confusion(y_enc: np.ndarray, y_pred: np.ndarray,
                    classes: np.ndarray, clf_name: str, out_dir: pathlib.Path):
    cm = confusion_matrix(y_enc, y_pred)
    n  = len(classes)
    fig, ax = plt.subplots(figsize=(max(5, n), max(4, n - 1)))
    ConfusionMatrixDisplay(cm, display_labels=classes).plot(
        ax=ax, colorbar=True, cmap='Blues')
    ax.set_title(f'Matriz de Confusão — {clf_name}')
    plt.xticks(rotation=35, ha='right', fontsize=8)
    plt.yticks(fontsize=8)
    fig.tight_layout()
    p = out_dir / 'confusion_matrix.png'
    fig.savefig(p, dpi=150); plt.close(fig)
    print(f'  Confusion matrix → {p}')


def _plot_importance(importances: np.ndarray, feat_cols: list[str],
                     out_dir: pathlib.Path):
    idx = np.argsort(importances)
    fig, ax = plt.subplots(figsize=(7, max(3, len(feat_cols) * 0.45)))
    ax.barh([feat_cols[i] for i in idx], importances[idx], color='steelblue')
    ax.set_xlabel('Importância (impureza de Gini)')
    ax.set_title('Feature Importance — Random Forest')
    fig.tight_layout()
    p = out_dir / 'feature_importance.png'
    fig.savefig(p, dpi=150); plt.close(fig)
    print(f'  Feature importance → {p}')


# ── Modelo final ───────────────────────────────────────────────────────────────

def _train_final(X: np.ndarray, y: np.ndarray, le: LabelEncoder,
                 feat_cols: list[str], clf_name: str,
                 cv_folds: int, out_dir: pathlib.Path):
    y_enc   = le.transform(y)
    classes = le.classes_
    n_min   = np.bincount(y_enc).min()
    folds   = min(cv_folds, int(n_min))

    clf  = CLASSIFIERS[clf_name]
    pipe = Pipeline([('scaler', StandardScaler()), ('clf', clf)])

    # Predições via CV para métricas honestas (sem data leakage)
    y_pred = np.zeros_like(y_enc)
    if folds >= 2:
        cv = StratifiedKFold(n_splits=folds, shuffle=True, random_state=42)
        for train_idx, test_idx in cv.split(X, y_enc):
            pipe.fit(X[train_idx], y_enc[train_idx])
            y_pred[test_idx] = pipe.predict(X[test_idx])
    else:
        pipe.fit(X, y_enc)
        y_pred = pipe.predict(X)
        print('  [WARN] amostras insuficientes para CV — métricas calculadas no treino.')

    report = classification_report(y_enc, y_pred, target_names=classes, zero_division=0)
    print(f'\n── Relatório final ({clf_name}, {folds}-fold CV) ─────────────────')
    print(report)
    (out_dir / 'classification_report.txt').write_text(
        f'Classificador: {clf_name}\nFeatures: {feat_cols}\n\n{report}',
        encoding='utf-8',
    )

    _plot_confusion(y_enc, y_pred, classes, clf_name, out_dir)

    # Treina no dataset completo para salvar o modelo e feature importance
    pipe.fit(X, y_enc)

    if clf_name == 'RF':
        _plot_importance(pipe.named_steps['clf'].feature_importances_,
                         feat_cols, out_dir)

    model_path = out_dir / f'nilm_{clf_name.lower()}_model.pkl'
    joblib.dump({'pipeline': pipe, 'label_encoder': le, 'features': feat_cols},
                model_path)
    print(f'  Modelo salvo → {model_path}')


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description='AccuEnergy — NILM Classifier (etapa 2)',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument('--input',    required=True, help='CSV rotulado (cluster_signatures.py)')
    ap.add_argument('--label',    default='label', help='Coluna com rótulos')
    ap.add_argument('--features', nargs='+', default=None,
                    help='Features (aliases: dp dq thd_v thd_i hv1..hv5 hi1..hi5). '
                         'Padrão: dp dq.')
    ap.add_argument('--cv',       type=int, default=5,   help='Folds para cross-validation')
    ap.add_argument('--out-dir',  default='.',            help='Diretório para figuras/modelo')
    ap.add_argument('--no-noise', action='store_true',    help='Remove rótulo "ruido"')
    ap.add_argument('--ablation', action='store_true',
                    help='Compara P+Q vs P+Q+Harm (ablation study para o artigo)')
    args = ap.parse_args()

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Define os feature sets a avaliar
    if args.ablation:
        feature_sets = {'P+Q': FEATS_BASE, 'P+Q+Harm': FEATS_HARM}
    else:
        feature_sets = {'features': args.features or FEATS_BASE}

    all_results: dict[str, dict] = {}
    best_context: dict = {}

    for fs_name, fs_aliases in feature_sets.items():
        print(f'\n── Feature set: {fs_name} ──────────────────────────────────')
        try:
            X, y, feat_cols = _load(args.input, fs_aliases, args.label, args.no_noise)
        except ValueError as exc:
            print(f'  [ERRO] {exc}')
            continue

        print(f'  Features: {feat_cols}')
        print(f'  Amostras: {len(X)}  |  Classes: {np.unique(y).tolist()}')

        le    = LabelEncoder()
        y_enc = le.fit_transform(y)

        results = _cv_evaluate(X, y_enc, args.cv, fs_name)
        if not results:
            continue

        all_results[fs_name] = results
        best_context = dict(X=X, y=y, le=le, feat_cols=feat_cols, fs_name=fs_name)

    if not all_results:
        print('\nNenhum resultado. Verifique o dataset e os rótulos.', file=sys.stderr)
        sys.exit(1)

    # Gráfico de comparação (útil só no ablation ou com múltiplos feature sets)
    if len(all_results) > 1:
        _plot_comparison(all_results, out_dir)

    # Melhor clf no último feature set avaliado
    last_fs   = best_context['fs_name']
    best_clf  = max(all_results[last_fs], key=lambda c: all_results[last_fs][c]['f1'])
    best_f1   = all_results[last_fs][best_clf]['f1']
    print(f'\nMelhor classificador: {best_clf}  (F1={best_f1:.3f}  feature set: {last_fs})')

    _train_final(
        best_context['X'], best_context['y'], best_context['le'],
        best_context['feat_cols'], best_clf, args.cv, out_dir,
    )

    # Exporta resultados completos para importar no artigo
    results_path = out_dir / 'resultados.json'
    results_path.write_text(json.dumps(all_results, indent=2), encoding='utf-8')
    print(f'  Resultados JSON → {results_path}')

    print('\nConcluído. Arquivos gerados:')
    for p in sorted(out_dir.glob('*.png')) + sorted(out_dir.glob('*.txt')) + \
             sorted(out_dir.glob('*.json')) + sorted(out_dir.glob('*.pkl')):
        print(f'  {p}')


if __name__ == '__main__':
    main()
