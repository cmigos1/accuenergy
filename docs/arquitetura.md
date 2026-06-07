# Arquitetura — AccuEnergy Power Monitor

```mermaid
flowchart LR

  subgraph SEN["Sensores - bifasico"]
    ZMPT["ZMPT101B F1\nR1=136k R2=1k\nfator 136x\n0-300 V"]
    SCT["SCT013-100 F1\n2000:1 Burden 50\nZener 3.3V\n0-100 A"]
    ZMPT2["ZMPT101B F2\nfator 136x\n0-300 V"]
    SCT2["SCT013-100 F2\n2000:1 Burden 50\n0-100 A"]
  end

  subgraph STM["STM32H743"]
    subgraph ACQ["Aquisicao - 2 ADC paralelos"]
      TIM["TIM2\n240MHz / 15625\nTRGO 15360 Hz"]
      ADC["ADC1 16-bit F1\nCH3=PA6 tensao\nCH4=PC4 corrente\n2 conv por trigger"]
      ADC2["ADC2 16-bit F2\nCH7=PA7 tensao\nCH8=PC5 corrente\n2 conv por trigger"]
      DMA["DMA2 Stream0\nbuf 2048 words\nV0 I0 V1 I1 ..."]
      DMA2["DMA2 Stream1\nbuf 2048 words\nV0 I0 V1 I1 ..."]
      TIM -->|TRGO| ADC
      TIM -->|TRGO| ADC2
      ADC  -->|DMA req| DMA
      ADC2 -->|DMA req| DMA2
    end

    subgraph PROC["Processamento - 1 frame = 1024 amostras"]
      DINT["Deinterleave\nvRaw 1024\niRaw 1024"]
      RMS["Vrms Irms P Q S FP\navg movel Irms N=5\nnoise floor threshold"]
      GOER["Goertzel\nh1..h50\nTHD V e I"]
      DINT --> RMS
      DINT --> GOER
    end

    DMA  -->|ConvCplt IRQ| DINT
    DMA2 -->|ConvCplt IRQ| DINT

    subgraph FOUT["Frames UART"]
      F1["Frame 0x01 Power F1\nABCD 01\n6xf32 + 128x2xi16\nEFFE - 543 B a 15 Hz"]
      F2["Frame 0x02 Harm F1\nABCD 02\n2xf32 + 100xf32\nEFFE - 413 B a 1.5 Hz"]
      F3["Frame 0x03 Power F2\nABCD 03\nmesmo layout 0x01\n543 B a 15 Hz"]
      F4["Frame 0x04 Harm F2\nABCD 04\nmesmo layout 0x02\n413 B a 1.5 Hz"]
    end

    RMS  --> F1
    GOER --> F2
    RMS  --> F3
    GOER --> F4
    UART_TX["LPUART1\n460800 8N1\nTX=PA12"]
    F1 --> UART_TX
    F2 --> UART_TX
    LCD["ST7735 SPI\ndisplay local\nVrms Irms P FP"]
    RMS --> LCD
  end

  subgraph ESP["ESP32 Bridge"]
    RX2["UART2 RX\nGPIO16\nbuf RX 2048 B\n460800 baud"]
    USBS["USB Serial\nUART0\n460800 baud"]
    LEDB["LED GPIO2\npulsa com dados"]
    RX2 -->|pass-through| USBS
    RX2 --> LEDB
  end

  subgraph PC["PC - monitor.py"]
    DRAIN["SerialReader\nthread daemon\nsync ABCD\ndrain buffer"]

    subgraph PAR["Parser"]
      P1["parse frame1\nvrms irms fp\npreal q s\nv wave i wave"]
      P2["parse frame2\nthd v thd i\nharm v 50\nharm i 50"]
    end

    subgraph GUI["GUI PyQt5 + pyqtgraph - 30 ms poll"]
      CARDS["MetricCards\nVrms Irms P Q\nS FP THD V THD I"]
      WAVE["Waveforms\nV(t) e I(t)\nlinhas pico"]
      HARM["Harmonic bars\nV e I h1..h50"]
    end

    subgraph DR["DataRecorder"]
      SEG["Segmentos nomeados\n1 por carga\nstart label / stop"]
      CSVO["CSV incremental\nlabel ts P S Q\nVrms Irms FP"]
      HDF["HDF5 NILMTK\n/building1/elec/meterN\npandas MultiIndex\n+ metadata.json"]
      SEG --> CSVO
      SEG --> HDF
    end

    DRAIN -->|power frame| P1
    DRAIN -->|harm frame| P2
    P1 --> CARDS
    P1 --> WAVE
    P2 --> CARDS
    P2 --> HARM
    P1 -->|append ts grandezas| SEG
  end

  CONS["consolidate hdf5 files\nsessao A sessao B ...\n1 meter por sessao"]
  NILMTK["NILMTK\nDataSet\ndisaggregation"]

  ZMPT  -->|PA6 CH3| ADC
  SCT   -->|PC4 CH4| ADC
  ZMPT2 -->|PA7 CH7| ADC2
  SCT2  -->|PC5 CH8| ADC2
  UART_TX -->|TX GPIO16| RX2
  USBS    -->|USB CDC| DRAIN
  HDF     -->|DataSet h5| NILMTK
  HDF     -.->|multiplas sessoes| CONS
  CONS    --> NILMTK

  classDef hw     fill:#1e3a5f,stroke:#89b4fa,color:#cdd6f4
  classDef fw     fill:#1f3520,stroke:#a6e3a1,color:#cdd6f4
  classDef bridge fill:#3d2b1f,stroke:#fab387,color:#cdd6f4
  classDef sw     fill:#2a1f3d,stroke:#cba6f7,color:#cdd6f4
  classDef data   fill:#1f2d3d,stroke:#89dceb,color:#cdd6f4
  classDef ext    fill:#2d1f1f,stroke:#f38ba8,color:#cdd6f4

  class ZMPT,SCT,ZMPT2,SCT2 hw
  class TIM,ADC,ADC2,DMA,DMA2,DINT,RMS,GOER,F1,F2,F3,F4,UART_TX,LCD fw
  class RX2,USBS,LEDB bridge
  class DRAIN,P1,P2,CARDS,WAVE,HARM sw
  class SEG,CSVO,HDF data
  class NILMTK,CONS ext
```

---

## Fluxo de amostragem — STM32H743

```mermaid
flowchart TD

  STM32([STM32H743])

  STM32      --> INIT
  INIT["Inicializa ADC1 DMA2\nTIM2 LPUART1 ST7735"]
  INIT       --> ADCCAL
  ADCCAL["Calibracao offset ADC\nHAL_ADCEx_Calibration_Start"]
  ADCCAL     --> CALIB

  CALIB["Calibra piso de ruido\n5 aquisicoes sem carga\ng_v_floor g_i_floor"]
  CALIB      --> STARTACQ

  STARTACQ["Inicia DMA2 + TIM2\nbuffer 2048 words\nV0 I0 V1 I1 ..."]
  STARTACQ   --> TRGO

  TRGO["TIM2 TRGO\n@ 15360 Hz\ndispara ADC1"]
  TRGO       --> ADCCONV
  ADCCONV["ADC1 converte\nCH3 tensao PA6\nCH4 corrente PC4\n16-bit por canal"]
  ADCCONV    --> DMAACC
  DMAACC["DMA2 Stream0\nacumula no buffer\ninterleaved"]
  DMAACC     --> Q1024{1024 pares\ncompletos?}

  Q1024 -->|nao| TRGO
  Q1024 -->|sim| ISR

  ISR["IRQ ConvCplt\nPara TIM2\nsamplesReady = 1"]
  ISR        --> DINT
  DINT["Deinterleave\nvRaw 1024\niRaw 1024"]
  DINT       --> CALC
  CALC["Calcula\nVrms Irms P Q S FP\nmed movel Irms N=5"]
  CALC       --> QFLOOR{Abaixo do\npiso de ruido?}

  QFLOOR -->|sim| ZERO["Zera grandezas\nVrms=0 Irms=0 P=0"]
  QFLOOR -->|nao| SEND1
  ZERO       --> SEND1

  SEND1["Envia Frame 0x01\nABCD 01 6xf32\n128x2xi16 EFFE\n543 bytes LPUART1"]
  SEND1      --> DISP
  DISP["Atualiza ST7735\nVrms Irms P FP\nuptime ADC raw"]
  DISP       --> QHARM{frame count\npercent 10 = 0?}

  QHARM -->|nao| RESTART
  QHARM -->|sim| GOERT
  GOERT["Goertzel h1..h50\npara V e I\nTHD_V THD_I"]
  GOERT      --> SEND2
  SEND2["Envia Frame 0x02\nABCD 02 2xf32\n100xf32 EFFE\n413 bytes LPUART1"]
  SEND2      --> RESTART

  RESTART["Reinicia DMA2 + TIM2\nproxima janela\n1024 amostras"]
  RESTART    --> QBTN{Botao PC13\npressionado?}

  QBTN -->|nao| TRGO
  QBTN -->|sim| STOPDMA
  STOPDMA["Para TIM2 + DMA\ndebounce 300 ms"]
  STOPDMA    --> CALIB

  LPUART([LPUART1\n460800 baud\nTX para ESP32])
  SEND1 -.-> LPUART
  SEND2 -.-> LPUART

  classDef actor   fill:#f4b8b8,stroke:#c98a9a,color:#222
  classDef process fill:#b8bef4,stroke:#8090c9,color:#111
  classDef decide  fill:#d0d2f0,stroke:#8090c9,color:#111

  class STM32,LPUART actor
  class INIT,ADCCAL,CALIB,STARTACQ,TRGO,ADCCONV,DMAACC,ISR,DINT,CALC,ZERO,SEND1,SEND2,DISP,GOERT,RESTART,STOPDMA process
  class Q1024,QFLOOR,QHARM,QBTN decide
```

---

## Resumo dos fluxos

### Fluxo de aquisição (hardware → firmware)
```
Rede elétrica
  ├─ ZMPT101B (Vtens) ──→ PA6 / ADC CH3 ─┐
  └─ SCT013-100 (Icorr) ─→ PC4 / ADC CH4 ─┤
                                            │
        TIM2 @ 15 360 Hz ──TRGO──→ ADC1 ──→ DMA2
                                            │
                                     2048 words
                                  [V₀ I₀ V₁ I₁ …]
```

### Fluxo de processamento (firmware → frames)
```
DMA ConvCplt IRQ
  └─→ Deinterleave  →  vRaw[1024]  iRaw[1024]
        ├─→ RMS / P / Q / S / FP  →  Frame 0x01  (543 B @ 15 Hz)
        └─→ Goertzel h1..h50      →  Frame 0x02  (413 B @  1.5 Hz)
                                          ↓
                                     LPUART1  460 800 baud
```

### Fluxo de comunicação (firmware → PC)
```
STM32 LPUART1 TX (frames 0x01-0x04, bifásico)
  └─→ ESP32-Bridge UART2 RX (GPIO16)  460 800 baud  RX buf 4 096 B (transparente)
        └─→ ESP32 USB Serial (UART0)  →  USB CDC VCP
              └─→ SerialReader thread  →  queue  →  GUI (30 ms poll)
```

A ESP32-Bridge é **byte-a-byte** (não interpreta frames), então os tipos de
fase 2 (`0x03`/`0x04`) passam sem mudança de lógica — só os buffers foram
ampliados (2 KB → 4 KB) para a rajada bifásica.

### Fluxo de gravação (GUI → NILMTK)
```
Sessão única (múltiplas cargas)          Múltiplas sessões
─────────────────────────────            ─────────────────
start("Ventilador")                      sessao1.h5
  append × N frames (F1 + F2)            sessao2.h5   ──→ consolidate_hdf5_files
stop()                                   sessao3.h5
start("Ferro")                                │
  append × M frames                           ↓
stop()                                   dataset_final.h5
  ↓
export_nilmtk_hdf5("dataset.h5")   ← 1 meter por fase, por segmento
  ├─ /building1/elec/meter1  ← Ventilador F1
  ├─ /building1/elec/meter2  ← Ventilador F2
  ├─ /building1/elec/meter3  ← Ferro F1
  └─ /building1/elec/meter4  ← Ferro F2
```

### Protocolo binário (frames do STM32)

Análise bifásica: cada fase tem um par de frames. A **fase 2** (ADC2, PA7/PC5)
usa os tipos `0x03`/`0x04` com **layout idêntico** aos da fase 1 (`0x01`/`0x02`) —
só muda o byte de tipo. O parser (`monitor.py`) reusa `parse_frame1`/`parse_frame2`.

| Campo         | Frame 0x01 Power F1 | Frame 0x02 Harm F1   | Frame 0x03 Power F2 | Frame 0x04 Harm F2 |
|---------------|---------------------|----------------------|---------------------|--------------------|
| Magic + tipo  | `AB CD 01`          | `AB CD 02`           | `AB CD 03`          | `AB CD 04`         |
| Payload       | Vrms Irms FP P Q S (6×f32) + 128 pares i16 | THD_V THD_I (2×f32) + harm_V[50] harm_I[50] (100×f32) | = 0x01 (fase 2) | = 0x02 (fase 2) |
| Footer        | `EF FE`             | `EF FE`              | `EF FE`             | `EF FE`            |
| Tamanho       | **543 bytes**       | **413 bytes**        | **543 bytes**       | **413 bytes**      |
| Taxa          | **~15 Hz**          | **~1.5 Hz**          | **~15 Hz**          | **~1.5 Hz**        |

> As duas fases amostram **em paralelo** (ADC1 + ADC2 no mesmo trigger TIM2),
> portanto adicionar a fase 2 **não aumenta o tempo de aquisição** por frame —
> apenas duplica os bytes transmitidos por janela (~2 KB/frame na LPUART).
