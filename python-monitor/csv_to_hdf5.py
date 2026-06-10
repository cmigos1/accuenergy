#!/usr/bin/env python3
"""
AccuEnergy CSV → HDF5 NILMTK
Converte o CSV gerado por mqtt_ingest.py para formato compatível com NILMTK.

Uso:
    python csv_to_hdf5.py medicoes.csv dataset.h5 --label "Geladeira Cozinha"
    python csv_to_hdf5.py medicoes.csv dataset.h5 --resample 1
"""

import argparse
import datetime
import json
import pathlib
import sys

import numpy as np
import pandas as pd

COLUMNS = [
    ('power',   'active'),
    ('power',   'apparent'),
    ('power',   'reactive'),
    ('voltage', ''),
    ('current', ''),
    ('power',   'factor'),
    ('energy',  'active'),
]

_METER_MSMTS = [
    {'physical_quantity': 'power',   'type': 'active',   'unit': 'W',   'lower_limit': -10000, 'upper_limit': 10000},
    {'physical_quantity': 'power',   'type': 'apparent', 'unit': 'VA',  'lower_limit': 0,      'upper_limit': 10000},
    {'physical_quantity': 'power',   'type': 'reactive', 'unit': 'VAr', 'lower_limit': -10000, 'upper_limit': 10000},
    {'physical_quantity': 'voltage', 'type': '',         'unit': 'V',   'lower_limit': 0,      'upper_limit': 300},
    {'physical_quantity': 'current', 'type': '',         'unit': 'A',   'lower_limit': 0,      'upper_limit': 100},
    {'physical_quantity': 'power',   'type': 'factor',   'unit': '',    'lower_limit': -1,     'upper_limit': 1},
    {'physical_quantity': 'energy',  'type': 'active',   'unit': 'Wh',  'lower_limit': 0,      'upper_limit': 1e9},
]


def csv_to_hdf5(
    csv_path: str,
    hdf5_path: str,
    label: str = 'AccuEnergy',
    building: int = 1,
    timezone: str = 'America/Cuiaba',
    resample_s: int | None = None,
) -> int:
    df = pd.read_csv(csv_path, parse_dates=['ts'])
    if df.empty:
        raise ValueError(f'CSV vazio: {csv_path}')

    df.set_index('ts', inplace=True)
    if df.index.tzinfo is None:
        df.index = df.index.tz_localize('UTC')
    df.index = df.index.tz_convert(timezone)
    df.index.name = 'datetime'

    col_idx = pd.MultiIndex.from_tuples(COLUMNS, names=['physical_quantity', 'type'])

    # Converte kWh para Wh (convenção NILMTK para energy/active)
    data = pd.DataFrame(
        {
            ('power',   'active'):   df['preal'].to_numpy(dtype=np.float32),
            ('power',   'apparent'): df['s'].to_numpy(dtype=np.float32),
            ('power',   'reactive'): df['q'].to_numpy(dtype=np.float32),
            ('voltage', ''):         df['vrms'].to_numpy(dtype=np.float32),
            ('current', ''):         df['irms'].to_numpy(dtype=np.float32),
            ('power',   'factor'):   df['fp'].to_numpy(dtype=np.float32),
            ('energy',  'active'):   (df['kwh'] * 1000.0).to_numpy(dtype=np.float64),
        },
        index=df.index,
    )
    data.columns = col_idx

    data = data[~data.index.duplicated(keep='last')]
    data.sort_index(inplace=True)

    if resample_s:
        data = data.resample(f'{resample_s}s').mean().dropna(how='all')

    hdf5_path = pathlib.Path(hdf5_path)
    key = f'/building{building}/elec/meter1'

    with pd.HDFStore(str(hdf5_path), mode='w', complevel=5, complib='blosc') as store:
        store.put(key, data, format='table')

    meta = {
        'name': 'AccuEnergy STM32',
        'long_name': 'AccuEnergy STM32H743 Power Measurement (MQTT IoT)',
        'date_issued': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'timezone': timezone,
        'geo_location': {'country': 'BR', 'locality': 'Cuiabá, MT'},
        'buildings': {
            building: {
                'description': label,
                'elec_meters': {
                    1: {
                        'device':      'stm32_accuenergy',
                        'submeter_of': None,
                        'site_meter':  True,
                        'label':       label,
                        'n_rows':      len(data),
                    }
                },
            }
        },
        'meter_devices': {
            'stm32_accuenergy': {
                'model':           'STM32H743 + SCT013-000 (burden 50Ω) + ZMPT101B standalone',
                'manufacturer':    'custom',
                'nominal_voltage': 127,
                'frequency':       60,
                'sample_period':   67,
                'measurements':    _METER_MSMTS,
            }
        },
    }

    meta_path = hdf5_path.with_suffix('.metadata.json')
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding='utf-8')

    print(f'{len(data)} linhas → {hdf5_path}')
    print(f'Metadados     → {meta_path}')
    return len(data)


def main():
    ap = argparse.ArgumentParser(
        description='AccuEnergy CSV → HDF5 NILMTK',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument('input',              help='CSV de entrada (saída do mqtt_ingest.py)')
    ap.add_argument('output',             help='Arquivo HDF5 de saída')
    ap.add_argument('--label',   default='AccuEnergy',      help='Label do dataset')
    ap.add_argument('--tz',      default='America/Cuiaba',  help='Fuso horário IANA')
    ap.add_argument('--resample', type=int, default=None,   help='Reamostrar para N segundos (None=manter original)')
    args = ap.parse_args()

    try:
        csv_to_hdf5(args.input, args.output, label=args.label,
                    timezone=args.tz, resample_s=args.resample)
    except Exception as exc:
        print(f'Erro: {exc}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
