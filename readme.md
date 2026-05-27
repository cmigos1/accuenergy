# Projeto Accuenergy - Monitoramento de Energia (NILM)

Este repositório concentra o desenvolvimento de um sistema descentralizado para monitoramento elétrico e evolução para estratégias de NILM (Non-Intrusive Load Monitoring).

O estado atual divide o sistema em três blocos principais: aquisição de alta velocidade com STM32, ponte IoT via ESP32, e ingestão de dados / interface via Python no backend.

## Estado Atual da Arquitetura

### 1) STM-Amostrador (Aquisição de Hardware e Processamento de Sinal)
Local: `STM-Amostrador/`

O coração da aquisição de dados rodando em um microcontrolador STM32H7. Ele atua como um medidor inteligente embarcado que captura sinais de tensão e corrente em alta velocidade.
- Realiza matemática complexa para calcular métricas de energia ($V_{rms}$, $I_{rms}$, Potência Ativa $P$, Reativa $Q$, Fator de Potência).
- Calcula harmônicas baseadas num buffer de 1024 amostras.
- Contém drivers (`st7735`) para atualização de um pequeno display LCD local para monitoramento em tempo real.
- **Saída:** Transmite as métricas compiladas continuamente como frames em protocolo binário pela porta Serial (UART).

### 2) ESP-Network (Ponte de Conectividade IoT e MQTT)
Local: `ESP-Network/`

Firmware escrito em C++ para ESP32 utilizando o PlatformIO. O firmware serve como uma ponte de comunicação.
- Conecta fisicamente ao STM32 via Serial, processando e bufferizando os dados de telemetria baseados no protocolo customizado do STM32.
- Gerencia conectividade Wi-Fi e se encarrega de publicar os relatórios para um broker MQTT via rede.
- Lida com métricas unificadas e persistência, como salvamento cumulativo de kWh no sistema de arquivos local (LittleFS).

### 3) Backend (Ingestão de Dados, Interface Gráfica e Análise NILM)
Local: `backend/`

Um conjunto de scripts Python que atuam como servidor e central de análise de dados.
- **Ingestão e Interface (`mqtt_ingest.py`, `mqtt_gui.py`, `gui_bridge.py`):** Lida com o parsing em tempo real das filas e mensagens MQTT que vêm do ESP32/Broker, operando uma interface de dashboard para visualização.
- **Motor NILM (`nilm_engine.py`):** Analisa a variação em formato de degraus de potência ativa ($\Delta P$) e reativa ($\Delta Q$) para deduzir em tempo real (ou off-line) a mudança de estado e tipo de dispositivos eletrodomésticos que são ligados ou desligados baseados em assinaturas de carga.
- **Tratamento de Dados (`csv_to_hdf5.py`):** Converte registros de arquivos para o formato rápido e otimizado HDF5.

## Estrutura do Repositório

```text
.
|-- backend/
|   |-- mqtt_ingest.py
|   |-- mqtt_gui.py
|   |-- nilm_engine.py
|   `-- requirements.txt
|-- docs/
|   |-- Artigos relevantes/
|   `-- Sensores/
|-- ESP-Network/
|   |-- platformio.ini
|   |-- src/
|   |   |-- main.cpp
|   |   `-- config.h
|-- STM-Amostrador/
|   |-- CMakeLists.txt
|   |-- Core/
|   `-- Drivers/
`-- readme.md
```

## Como Executar

### 1) Firmware STM32
Projeto gerado para utilização através de um fluxo do CMake ou CubeIDE. Pode compilar através da extensão CMake Tools no VS Code.

### 2) Firmware ESP32
Pré-requisitos: VS Code com extensão do PlatformIO.
Na pasta `ESP-Network/`, ajuste o seu Wi-Fi e Broker em `src/config.h` e execute:
- Upload para a placa ESP32.

### 3) Backend e GUI Python
Na pasta `backend/`:
```bash
python -m venv .venv
# ative a venv correspondente ao seu SO
pip install -r requirements.txt
python mqtt_gui.py
```

## Próximos passos
- Evolução de detecção de eventos multicanais / bifásico / trifásico.
- Aperfeiçoamento dos métodos de classificação no engine NILM.