#!/usr/bin/env python3
"""
csv_to_hdf5.py — US11
Converte CSV gerado pelo mqtt_ingest.py em HDF5 compatível com NILMTK.

Estrutura gerada no H5:
    /building1/elec/meter1  →  DataFrame com MultiIndex de colunas
    Colunas: (power, active), (power, apparent), (power, reactive),
             (voltage, ''), (current, ''), (energy, active)

Para dataset NILMTK completo (DataSet.load()), também é necessário gerar
os arquivos de metadados YAML (building1.yaml, dataset.yaml).
Consulte: https://nilmtk.github.io/nilmtk/master/dataset_converter.html

Uso:
    python csv_to_hdf5.py entrada.csv saida.h5
"""
import sys
from pathlib import Path

import numpy as np
import pandas as pd

NILMTK_PATH = "/building1/elec/meter1"

# Mapeamento coluna CSV → chave NILMTK (type, ac_type)
_COL_MAP: dict[tuple[str, str], str] = {
    ("power",   "active"):   "preal",
    ("power",   "apparent"): "s",
    ("power",   "reactive"): "q",
    ("voltage", ""):         "vrms",
    ("current", ""):         "irms",
}


def _load_csv(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    if "ts" not in df.columns:
        raise ValueError("CSV não contém coluna 'ts'")

    df["ts"] = pd.to_datetime(df["ts"], errors="coerce")
    df = df.dropna(subset=["ts"])
    df = df.set_index("ts")
    df.index.name = "datetime"
    df = df.sort_index()
    return df


def csv_to_hdf5(csv_path: str, h5_path: str) -> None:
    df = _load_csv(Path(csv_path))

    if df.empty:
        print("CSV vazio ou sem timestamps válidos.", file=sys.stderr)
        sys.exit(1)

    # Monta colunas NILMTK
    series: dict[tuple[str, str], pd.Series] = {}
    for nilmtk_col, src_col in _COL_MAP.items():
        if src_col in df.columns:
            series[nilmtk_col] = pd.to_numeric(df[src_col], errors="coerce")

    # Energia ativa: kWh → Wh (unidade NILMTK)
    if "kwh" in df.columns:
        kwh_series = pd.to_numeric(df["kwh"], errors="coerce")
        series[("energy", "active")] = kwh_series * 1000.0

    if not series:
        print("Nenhuma coluna mapeada — verifique o CSV.", file=sys.stderr)
        sys.exit(1)

    out = pd.DataFrame(series)
    out.columns = pd.MultiIndex.from_tuples(out.columns, names=["type", "ac_type"])
    out.index.freq = None   # NILMTK não exige frequência fixa

    with pd.HDFStore(h5_path, "w", complevel=5, complib="blosc") as store:
        store.put(NILMTK_PATH, out, format="table")

    _report(h5_path, out)


def _report(h5_path: str, df: pd.DataFrame) -> None:
    dur = df.index[-1] - df.index[0]
    print(f"HDF5 gerado : {h5_path}")
    print(f"  Registros : {len(df)}")
    print(f"  Período   : {df.index[0]}  →  {df.index[-1]}  ({dur})")
    print(f"  Colunas   : {list(df.columns)}")
    print(f"  Caminho H5: {NILMTK_PATH}")

    if ("energy", "active") in df.columns:
        kwh_total = df[("energy", "active")].iloc[-1] / 1000.0
        print(f"  kWh total : {kwh_total:.4f}")

    print()
    print("Para carregar no NILMTK:")
    print(f"  from nilmtk import DataSet")
    print(f"  ds = DataSet('{h5_path}')  # requer metadata/ YAML")
    print()
    print("Ou diretamente via pandas:")
    print(f"  import pandas as pd")
    print(f"  df = pd.read_hdf('{h5_path}', '{NILMTK_PATH}')")


def main():
    if len(sys.argv) != 3:
        print(f"Uso: {sys.argv[0]} entrada.csv saida.h5", file=sys.stderr)
        sys.exit(1)
    csv_to_hdf5(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
