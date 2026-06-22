# AccuEnergy — Medidor bifásico de energia elétrica

Sistema NILM (*Non-Intrusive Load Monitoring*) Edge-to-Server de baixo custo: aquisição bifásica em alta frequência no STM32H743, transmissão via MQTT e desagregação por aprendizado de máquina no servidor.

---

## Arquitetura

```
Rede elétrica
   │
   ├── ZMPT101B (F1) ──► PA6 ─┐
   ├── SCT013-100 (F1) ─► PC4 ─┤  ADC1 (DMA2 Stream0)
   │                            │
   ├── ZMPT101B (F2) ──► PA7 ─┐│
   └── SCT013-100 (F2) ─► PC5 ─┤│ ADC2 (DMA2 Stream1)
                                ││
                         STM32H743 (WeAct)
                         TIM2 @ 15360 Hz · 1024 amostras/frame
                         Goertzel h1..h50 · P/Q/S/FP/THD
                         ↓ LPUART1 460800 baud
                         ↓
                     GPIO16 ESP32 (ESP-Network)
                     WiFi · MQTT · kWh · NTP
                         │ MQTT JSON
                         ▼
              Mosquitto broker (192.168.1.80:1883)
              energia/medidor · energia/harmonicas
                         │
                  mqtt_ingest.py
                  (assinante · CSV logger)
                         │
                  Análise NILM (Python)
                  detecção Hart · K-means · teste nulidade
```

---

## Estrutura do repositório

```
accuenergy/
├── STM-Amostrador/          # Firmware STM32H743 (STM32CubeIDE)
│   └── Core/
│       ├── Inc/
│       │   ├── calculo.h    # Constantes de calibração e amostragem
│       │   └── st7735.h     # Interface display TFT 0.96"
│       └── Src/
│           ├── main.c       # Loop principal, ADC/DMA, cálculos, transmissão
│           └── st7735.c     # Driver SPI bit-bang ST7735
│
├── ESP32-Bridge/            # Firmware ESP32 — ponte UART↔USB (PlatformIO)
│   └── src/main.cpp
│
├── ESP-Network/             # Firmware ESP32 — nó IoT WiFi/MQTT (PlatformIO)
│   └── src/
│       ├── main.cpp
│       └── config.h         # SSID, senha, broker MQTT (192.168.1.80)
│
├── python-monitor/          # Scripts de ingestão e análise (Python 3.10+)
│   ├── monitor.py           # GUI PyQt5 — visualização via ESP32-Bridge
│   ├── mqtt_ingest.py       # Assinante MQTT → CSV logger (roda no servidor)
│   └── data/                # CSVs coletados (excluídos do git via .gitignore)
│       ├── medicoes*.csv
│       ├── harmonicas*.csv
│       └── res_*/           # Artefatos de clustering (clusters.png, etc.)
│
├── nilmtk/                  # Fork local do NILMTK (instalado com pip install --no-deps -e .)
│
└── docs/artigo/             # Artigo SBC (LaTeX)
    ├── main.tex
    └── figuras/
```

---

## Hardware

### Sensores

| Sensor | Função | Fator de escala | Observações |
|---|---|---|---|
| ZMPT101B (standalone) | Tensão | R1/R2 = 136kΩ/1kΩ = **136** | Sem módulo op-amp; saída centrada em 1,65 V (VDD/2) |
| SCT013-100 | Corrente | CT 2000:1, burden 50 Ω | Zener 1N4728A protege excursão positiva no pino ADC |

### Verificação de range (127 Vrms nominal ANEEL)

| Grandeza | Cálculo | Resultado |
|---|---|---|
| I_pico ZMPT primário | 127√2 / 136kΩ | 1,32 mA < 2 mA (linear) |
| V_ADC pico | 1,65 V ± 1,32 V | [0,33 V; 2,97 V] — ~80% do range 0–3,3 V |
| Sobretensão 140 Vrms (limite ANEEL) | 1,65 V ± 1,46 V | [0,19 V; 3,11 V] — sem clipping |
| Corrente máxima SCT (clipping Zener) | (3,3−1,65)/50Ω = 33 mA sec | 66 A pico → ~47 Arms no primário |

> **Atenção:** A excursão negativa do SCT013 chega a −0,85 V (Zener não protege esse lado). Os diodos internos do STM32 fazem clamping, mas um Schottky para GND no pino PC4/PC5 é recomendado.

### Pinagem STM32H743 (WeAct)

**ADC — sensores**

| Sinal | Pino | ADC | Canal | Fase |
|---|---|---|---|---|
| Tensão V1 | **PA6** | ADC1 | CH3 (Rank 1) | F1 |
| Corrente I1 | **PC4** | ADC1 | CH4 (Rank 2) | F1 |
| Tensão V2 | **PA7** | ADC2 | CH7 (Rank 1) | F2 |
| Corrente I2 | **PC5** | ADC2 | CH8 (Rank 2) | F2 |

**Comunicação serial**

| Sinal | Pino | Periférico | Baud |
|---|---|---|---|
| TX → ESP32 | **PA9** | LPUART1 | 460800 |
| RX ← ESP32 | **PA10** | LPUART1 | 460800 |

**Display ST7735 (0.96" 80×160, SPI bit-bang)**

| Sinal | Pino | Função |
|---|---|---|
| LED | **PE10** | Backlight |
| CS | **PE11** | Chip select |
| SCL | **PE12** | Clock |
| DC | **PE13** | Dado (HIGH) / Comando (LOW) |
| SDA | **PE14** | MOSI |

**Controle**

| Função | Pino | Lógica |
|---|---|---|
| Botão recalibrar ruído | **PC13** | Ativo ALTO — pull-down interno |

### Pinagem ESP32

| Sinal | Pino | Direção |
|---|---|---|
| RX ← STM32 TX | **GPIO16** | Entrada |
| TX → STM32 RX | **GPIO17** | Saída (para futuros comandos) |
| LED onboard | **GPIO2** | Saída — pisca com fluxo de dados |

---

## Pipeline de aquisição (STM32)

```
TIM2 TRGO @ 15360 Hz
      │
      ├─► ADC1 (PA6=V1, PC4=I1) ─► DMA2 Stream0 ─► adcDmaBuf[2048]
      └─► ADC2 (PA7=V2, PC5=I2) ─► DMA2 Stream1 ─► adcDmaBuf2[2048]
          (interleaved: [V₀ I₀ V₁ I₁ ... V₁₀₂₃ I₁₀₂₃])

Callbacks DMA (ISR):
  ADC1 completo → para TIM2, DSB, samplesReady=1
  ADC2 completo → DSB, samplesReady2=1

Loop principal (ambos prontos):
  1. Deinterleave  → vRaw[1024], iRaw[1024]  (para cada fase)
  2. Compute       → Vrms, Irms, Preal, Q, S, FP  (double-precision)
  3. Média deslizante Irms (N=5 frames)
  4. Piso de ruído → zera se Vrms < g_v_floor ou Irms < g_i_floor
  5. Transmite frame 0x01 (F1) + 0x03 (F2)
  6. A cada 10 frames: Goertzel h1..h50 → frame 0x02 (F1) + 0x04 (F2)
  7. Atualiza display ST7735
  8. Reinicia DMA + TIM2
```

### Parâmetros de amostragem

| Parâmetro | Valor | Derivação |
|---|---|---|
| Taxa de amostragem (Fs) | 15360 Hz | TIM2: 240 MHz / 15625 |
| Amostras por frame | 1024 | ~66,7 ms = 4 ciclos a 60 Hz |
| Amostras enviadas (IDF) | 128 | decimação 1:8 |
| Duração do eixo de tempo | ~66,1 ms | 128 × 8 / 15360 × 1000 |
| Frequência dos frames | ~15 Hz | 1 frame / 66,7 ms |
| Frequência dos harmônicos | ~1,5 Hz | 1 a cada 10 frames |

### Constantes de calibração

```
ADC: 16 bits, VDDA = 3,3 V  →  1 LSB = 3,3 / 65535 = 50,35 µV

CAL_V = (3,3 / 65535) × 136            = 6,848 mV/count  → counts → Vrms [V]
CAL_I = (3,3 / 65535) × (2000 / 50)    = 2,014 mA/count  → counts → Irms [A]

Verificação @ 127 Vrms:
  V_R2_rms = 127 / 136 = 0,934 V  →  18558 counts rms
  Vrms = 18558 × 6,848e-3 = 127,1 V  ✓
```

> **Por que int32 nas amostras?** Para Vrms = 177 V o desvio de pico é
> 250 V / 6,848 mV/count = 36.504 counts > INT16_MAX (32767), causando
> wrapping e distorção trapezoidal na forma de onda. Migrado para `int32_t`
> em Jun/2026.

### Algoritmo Goertzel (harmônicos)

- N = 1024, Fs = 15360 Hz, F0 = 60 Hz → **bin = N × F0 / Fs = 4**
- Magnitude de pico normalizada: `2 × sqrt(re² + im²) / N`
- THD_V = `sqrt(Σₙ₌₂⁵⁰ Vₙ²) / V₁`
- Harmônicos calculados: h1 (60 Hz) até h50 (3000 Hz)

---

## Protocolo binário

Todos os campos numéricos são little-endian. Magic = `AB CD`, Footer = `EF FE`.

### Frame 0x01 / 0x03 — Potência (fase 1 / fase 2)

| Offset | Campo | Tipo | Bytes |
|---|---|---|---|
| 0 | Magic | uint8[2] | 2 |
| 2 | Tipo (`01` ou `03`) | uint8 | 1 |
| 3 | Vrms | float32 LE | 4 |
| 7 | Irms | float32 LE | 4 |
| 11 | FP | float32 LE | 4 |
| 15 | Preal | float32 LE | 4 |
| 19 | Q | float32 LE | 4 |
| 23 | S | float32 LE | 4 |
| 27 | N (amostras enviadas) | uint8 | 1 |
| 28 | Step (decimação) | uint8 | 1 |
| 29 | Amostras `[V₀ I₀ V₁ I₁ ...]` | int32 LE × (N×2) | 1024 |
| 1053 | Footer | uint8[2] | 2 |
| **Total** | | | **1055** |

Converter para unidade física:
```python
v_volts = count * (3.3 / 65535 * 136)    # CAL_V
i_amps  = count * (3.3 / 65535 * 40000)  # CAL_I = LSB * 2000/50
```

### Frame 0x02 / 0x04 — Harmônicos (fase 1 / fase 2)

| Offset | Campo | Tipo | Bytes |
|---|---|---|---|
| 0 | Magic | uint8[2] | 2 |
| 2 | Tipo (`02` ou `04`) | uint8 | 1 |
| 3 | THD_V | float32 LE | 4 |
| 7 | THD_I | float32 LE | 4 |
| 11 | HarmMagV[1..50] | float32[50] LE | 200 |
| 211 | HarmMagI[1..50] | float32[50] LE | 200 |
| 411 | Footer | uint8[2] | 2 |
| **Total** | | | **413** |

Magnitudes em V pico (tensão) e A pico (corrente).

---

## ESP32-Bridge

Ponte byte-a-byte transparente. Repassa todos os frames `0x01..0x04` e linhas ASCII de debug (`#`) sem processamento.

- RX buffer: 4096 bytes (suporta rajada bifásica de ~2,1 KB/janela)
- LED GPIO2 pisca a cada 60 ms enquanto há fluxo

## ESP-Network

Nó IoT autônomo. Faz parsing próprio dos frames e publica via MQTT com timestamp NTP.

| Tópico MQTT | Conteúdo |
|---|---|
| `energia/medidor` | `{phase, vrms, irms, fp, preal, q, s, kwh, ts}` |
| `energia/harmonicas` | `{phase, thd_v, thd_i, hv1..hv50, hi1..hi50, ts}` |

Persistência: kWh acumulado salvo em LittleFS (`/kwh.bin`) a cada 60 s.

Throttle de publicação: 500 ms por fase. Retain desativado (overload explícito `publish(topic, payload, len, false)`).

> **Atenção (pendente):** o ESP-Network ainda usa `int16` no parser de amostras. Após a migração do STM32 para `int32` (Jun/2026), o campo `SKIP_SAMPLES` e o tamanho do frame precisam ser atualizados em `ESP-Network/src/main.cpp`.

---

## Python Monitor (via ESP32-Bridge)

```bash
cd python-monitor
pip install -r requirements.txt
python monitor.py [--port COM5] [--baud 460800]
```

GUI PyQt5 para visualização em tempo real via porta serial (ESP32-Bridge). Exibe Vrms, Irms, P, Q, S, FP, THD e espectro harmônico h1..h50 para as duas fases simultaneamente.

---

## Pipeline de ingestão MQTT

`mqtt_ingest.py` roda no servidor dedicado (192.168.1.80) como serviço persistente (Windows Task Scheduler). Assina os dois tópicos e grava CSVs com rotação automática de schema.

```bash
python mqtt_ingest.py [--broker 192.168.1.80] [--port 1883]
```

**Arquivos gerados:**

| Arquivo | Colunas |
|---|---|
| `medicoes_AAAA-MM-DD.csv` | ts, phase, vrms, irms, preal, s, q, fp, kwh |
| `harmonicas_AAAA-MM-DD.csv` | ts, phase, thd_v, thd_i, hv1..hv50, hi1..hi50 |

> `q` é gravado como `abs(Q)` — o STM32 calcula Q = √(S²−P²), magnitude não sinalizada.

---

## Análise NILM

Scripts em `python-monitor/` (prefixo `_`, não versionados — contêm caminhos locais):

| Script | Função |
|---|---|
| `_events.py` | Detector Hart sliding-baseline → `eventos_*.csv` |
| `_export_events.py` | Exporta ΔP, ΔQ por evento |
| `_merge_harm.py` | Join eventos ↔ harmônicos (`merge_asof` por fase) |
| `_dharm_test.py` | Teste de nulidade: Δharmônico real vs embaralhado |
| `_compara.py` | K-means 1 dia vs 2 dias · degradação temporal |
| `_harm_fig.py` | Figura ablação harmônicos para o artigo |
| `_nilmtk_analysis.py` | Hart85 do NILMTK sobre os dados reais |

### Resultados principais (Jun/2026)

- K-means (ΔP, ΔQ): K=10 silhueta 0,44 em 1 dia → K=3 silhueta 0,74 em 2 dias (diluição por inverter)
- Subconjunto limpo (|ΔP| > 200 W): K=4, silhueta 0,50 — motor ~2,3 kW vs resistivos ~800 W
- Harmônicos (teste de nulidade): real ≈ embaralhado — sem contribuição discriminativa
- Hart85 NILMTK vs detector próprio: F2 diverge <0,5%; F1 ~16% por diferença de parâmetros internos

### NILMTK — instalação local

```bash
# Fork em ./nilmtk/ — Python 3.14 (hmmlearn sem wheel → FHMM indisponível)
pip install --no-deps -e ./nilmtk
pip install h5py networkx pyyaml tqdm pytz ipython nilm_metadata
```

---

## Calibração automática (boot)

```
STM32 liga → "CALIBRANDO..." no display → desconecte todas as cargas
  Adquire 5 frames → calcula Irms de ruído máximo
  g_i_floor = max_irms × 2,2   (margem de segurança)
  g_v_floor = 1,0 V            (fixo — sensor sempre energizado pela rede)
  Exibe "CAL OK" com os valores

Durante operação:
  Vrms < g_v_floor  →  Vrms = 0, Preal = 0
  Irms < g_i_floor  →  Irms = 0, Preal = 0

Recalibrar a qualquer momento: pressionar KEY (PC13) sem cargas
```

---

## Build e gravação

### STM32H743 — STM32CubeIDE

```
File → Import → Existing Projects → STM-Amostrador/
Project → Build All (Release)
Run → Flash via ST-Link V2
```

### ESP32-Bridge — PlatformIO

```bash
cd ESP32-Bridge
pio run --target upload --upload-port COM<N>
```

### ESP-Network — PlatformIO

```bash
cd ESP-Network
# Editar src/config.h: SSID, senha Wi-Fi, IP/host broker MQTT, porta
pio run --target upload --upload-port COM<N>
```
