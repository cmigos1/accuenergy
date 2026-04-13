# Projeto Accuenergy - Monitoramento de Energia (NILM)

Este repositório, no estado atual, concentra o desenvolvimento em dois blocos:

1. Firmware de aquisição e processamento no ESP32 usando ADC interno.
2. Aplicação desktop em Python para visualização em tempo real, FFT e calibração.

O objetivo geral continua sendo monitoramento elétrico e evolução para estratégias de NILM, com base em sinais de tensão e corrente.

## Estado Atual

- O código ativo de firmware está em `esp32_interno/` (PlatformIO + Arduino).
- A interface de visualização e calibração está em `visualizacao/` (Tkinter + Matplotlib).
- A pasta `docs/` reúne artigos e datasheets de sensores.

## Arquitetura Atual

### 1) ESP32 interno (amostragem + cálculo + envio)

Local: `esp32_interno/`

Resumo técnico:
- Amostragem de dois canais ADC1:
	- Tensão: GPIO34 (`ADC1_CHANNEL_6`)
	- Corrente: GPIO35 (`ADC1_CHANNEL_7`)
- Taxa alvo de amostragem: 7200 Hz.
- Janela de processamento: 600 amostras (5 ciclos de 60 Hz).
- Execução em tarefas FreeRTOS:
	- Core 1: amostragem contínua
	- Core 0: cálculo e envio
- Cálculos principais:
	- `V_RMS`, `I_RMS`
	- potência ativa, reativa e aparente
	- fator de potência
- Envio serial em protocolo binário a 230400 baud.

Arquivos principais:
- `esp32_interno/src/main.cpp`
- `esp32_interno/src/calculo.cpp`
- `esp32_interno/src/envio.cpp`
- `esp32_interno/include/calculo.h`
- `esp32_interno/include/envio.h`

### 2) GUI Python (visualização + calibração)

Local: `visualizacao/`

Funcionalidades principais:
- Conexão serial com o ESP32.
- Exibição em tempo real de grandezas elétricas.
- Plot de formas de onda de tensão e corrente.
- Espectro harmônico (FFT).
- Calibração por fator/offset e apoio por multímetro.
- Modos de calibração do ADC via comandos seriais.

Arquivo principal:
- `visualizacao/gui_amostrador_interno.py`

Dependências:
- listadas em `visualizacao/requirements.txt`

## Estrutura do Repositório

```text
.
|-- docs/
|   |-- Artigos relevantes/
|   `-- Sensores/
|-- esp32_interno/
|   |-- platformio.ini
|   |-- include/
|   |   |-- calculo.h
|   |   `-- envio.h
|   |-- src/
|   |   |-- calculo.cpp
|   |   |-- envio.cpp
|   |   `-- main.cpp
|   `-- test/
|-- visualizacao/
|   |-- gui_amostrador_interno.py
|   `-- requirements.txt
`-- readme.md
```

## Protocolo Serial Binário

Todos os frames usam:
- header mágico: `0xAB 0xCD`
- tipo de frame: 1 byte
- payload variável
- footer: `0xEF 0xFE`

Tipos implementados:
- `0x01` DATA
	- métricas elétricas + amostras decimadas de V/I
- `0x02` CAL_POT
	- estatísticas em mV brutos para calibração por potenciômetro
- `0x03` CAL_PIN
	- comparação de ganho entre canais usando mesmo sinal em ambos

Referência de implementação:
- `esp32_interno/include/envio.h`
- parser na GUI em `visualizacao/gui_amostrador_interno.py`

## Como Executar

### 1) Firmware ESP32

Pré-requisitos:
- VS Code com PlatformIO, ou PlatformIO CLI.
- Placa ESP32 DevKit (ambiente `esp32dev`).

Comandos (na pasta `esp32_interno/`):

```bash
pio run
pio run -t upload
pio device monitor -b 230400
```

### 2) GUI de visualização

Pré-requisitos:
- Python 3.10+ recomendado.

Comandos (na pasta `visualizacao/`):

```bash
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python gui_amostrador_interno.py
```

## Comandos de Modo (serial)

Comandos aceitos pelo firmware:
- `NORMAL`
- `CAL_POT`
- `CAL_PIN`

A GUI já envia esses comandos pelos botões da aba de calibração.

## Documentação Técnica

Em `docs/`:
- artigos de NILM para referência de métodos e estado da arte.
- datasheets e materiais de sensores.

## Observações

- O projeto está em fase de evolução de plataforma e calibração.

## Proximos passos
- Adição de outro par de sensores (tensão e corrente), para análise bifásica.
- Alterar a maneira de transferência de dados de serial para Wi-Fi.