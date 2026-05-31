# Arquitetura — AccuEnergy Power Monitor

```mermaid
flowchart TB

  %% ── Sensores ──────────────────────────────────────────────────────────────
  subgraph SEN["⚡ Sensores"]
    direction TB
    ZMPT["ZMPT101B\nTransformador de tensão\nR1 = 136 kΩ · R2 = 1 kΩ\nFator = 136× · faixa 0–300 V"]
    SCT["SCT013-100\nTransformador de corrente\nRazão 2000:1 · Burden 50 Ω\nZener 1N4728A (clamp 3.3 V)"]
  end

  %% ── STM32H743 ─────────────────────────────────────────────────────────────
  subgraph STM["🔲 STM32H743 WeAct — STM-Amostrador"]
    direction TB

    subgraph ACQ["Aquisição"]
      direction LR
      TIM["TIM2\nAPB1×2 = 240 MHz\nPeriod = 15624\n→ TRGO @ 15 360 Hz"]
      ADC["ADC1  16-bit\nCH3 = PA6 tensão\nCH4 = PC4 corrente\n2 conv / trigger\nScan + DMA circular"]
      DMA["DMA2 Stream0\nbuffer 2048 words\ninterleaved [V₀ I₀ V₁ I₁…]\nDMA_NORMAL"]
      TIM -->|TRGO rising| ADC
      ADC -->|DMA req| DMA
    end

    subgraph PROC["Processamento  (1 frame = 1024 amostras ≈ 66.7 ms)"]
      direction TB
      DINT["Deinterleave\nvRaw[1024]  iRaw[1024]"]
      RMS["Vrms · Irms · P · Q · S · FP\nmádia deslizante Irms N=5\nthreshold noise floor"]
      GOER["Goertzel  h1..h50\nbin F0 = 4  (60 Hz)\nTHD_V · THD_I"]
      DINT --> RMS
      DINT --> GOER
    end

    DMA -->|"ConvCplt IRQ\n__DSB()"| DINT

    subgraph FRAMES["Frames binários"]
      direction TB
      F1["Frame 0x01  Power\nABCD·01 · Vrms·Irms·FP·P·Q·S (6×f32)\n128 pares i16 downsampled (step=8)\nEFFE  →  543 bytes  @ ~15 Hz"]
      F2["Frame 0x02  Harmonics\nABCD·02 · THD_V·THD_I (2×f32)\nharmV[1..50]·harmI[1..50] (100×f32)\nEFFE  →  413 bytes  @ ~1.5 Hz"]
    end

    RMS  --> F1
    GOER --> F2

    UART["LPUART1\n460 800 baud · 8N1\nTX = PA12"]
    F1 --> UART
    F2 --> UART

    LCD["ST7735  SPI\ndisplay local\nVrms · Irms · P · FP\nuptime · ADC raw"]
    RMS --> LCD
  end

  %% ── ESP32 ─────────────────────────────────────────────────────────────────
  subgraph ESP["📡 ESP32  —  ESP32-Bridge"]
    direction LR
    U2["UART2\nRX = GPIO16\nbuffer RX 2 048 bytes\n460 800 baud"]
    USB_SER["USB Serial  UART0\n460 800 baud\n(CDC → PC)"]
    LED_B["LED GPIO2\npulsa ao receber dados"]
    U2 -->|"readBytes 1 024\npass-through"| USB_SER
    U2 --> LED_B
  end

  %% ── PC ────────────────────────────────────────────────────────────────────
  subgraph PC["💻 PC  —  python-monitor/monitor.py"]
    direction TB

    subgraph THREAD["SerialReader  (thread daemon)"]
      DRAIN["drain bytearray\nsync magic ABCD\ndescarta bytes órfãos"]
    end

    subgraph PARSE["Parser"]
      direction LR
      P1["parse_frame1\n→ dict vrms irms fp\n   preal q s\n   v_wave[128] i_wave[128]"]
      P2["parse_frame2\n→ dict thd_v thd_i\n   harm_v[50] harm_i[50]"]
    end

    subgraph GUI["GUI  —  PyQt5 + pyqtgraph  (QTimer 30 ms)"]
      direction LR
      CARDS["8 MetricCards\nVrms · Irms · P · Q\nS · FP · THD_V · THD_I"]
      WAVE["Waveforms\nV(t) e I(t) em Volts/Ampères\nlinhas ±Vpico tracejadas"]
      HARM["Harmonic bars\nV e I  h1..h50\nrótulo h1 com magnitude"]
    end

    subgraph REC["DataRecorder"]
      direction TB
      SEG["Segmentos nomeados\n(1 por carga testada)\nstart label · stop"]
      CSV_O["CSV incremental\nlabel · ts · P · S · Q\nVrms · Irms · FP"]
      HDF_O["HDF5  pandas HDFStore\n/building1/elec/meterN\nMultiIndex power/voltage/current\n+ metadata.json  NILMTK-ready"]
      SEG --> CSV_O
      SEG --> HDF_O
    end

    DRAIN -->|queue power| P1
    DRAIN -->|queue harm|  P2
    P1 --> CARDS & WAVE
    P2 --> CARDS & HARM
    P1 -->|"append ts vrms irms\nP S Q FP"| SEG
  end

  %% ── NILMTK ────────────────────────────────────────────────────────────────
  NILMTK["📊 NILMTK\nDataSet · elec.mains()\ndisaggregation · CO · FHMM"]

  %% ── Ligações externas ──────────────────────────────────────────────────────
  ZMPT -->|"PA6  CH3\nAC centrado em 1.65 V"| ADC
  SCT  -->|"PC4  CH4\nAC centrado em 1.65 V"| ADC
  UART -->|"TX → GPIO16\nGND comum"| U2
  USB_SER -->|"USB CDC\nVCP driver"| DRAIN
  HDF_O   -->|"DataSet(h5)"| NILMTK

  %% ── Consolidação ───────────────────────────────────────────────────────────
  CONS["consolidate_hdf5_files\n[ sessão A, sessão B … ]\n→ dataset único\nmeter1 · meter2 · …"]
  HDF_O -.->|"múltiplas sessões"| CONS
  CONS  -->|"HDF5 consolidado"| NILMTK

  %% ── Estilos ────────────────────────────────────────────────────────────────
  classDef hw      fill:#1e3a5f,stroke:#89b4fa,color:#cdd6f4
  classDef fw      fill:#1f3520,stroke:#a6e3a1,color:#cdd6f4
  classDef bridge  fill:#3d2b1f,stroke:#fab387,color:#cdd6f4
  classDef sw      fill:#2a1f3d,stroke:#cba6f7,color:#cdd6f4
  classDef data    fill:#1f2d3d,stroke:#89dceb,color:#cdd6f4
  classDef tool    fill:#2d1f1f,stroke:#f38ba8,color:#cdd6f4

  class ZMPT,SCT hw
  class TIM,ADC,DMA,DINT,RMS,GOER,F1,F2,UART,LCD fw
  class U2,USB_SER,LED_B bridge
  class DRAIN,P1,P2,CARDS,WAVE,HARM sw
  class SEG,CSV_O,HDF_O data
  class NILMTK,CONS tool
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
STM32 LPUART1 TX
  └─→ ESP32 UART2 RX (GPIO16)  460 800 baud  RX buf 2 048 B
        └─→ ESP32 USB Serial (UART0)  →  USB CDC VCP
              └─→ SerialReader thread  →  queue  →  GUI (30 ms poll)
```

### Fluxo de gravação (GUI → NILMTK)
```
Sessão única (múltiplas cargas)          Múltiplas sessões
─────────────────────────────            ─────────────────
start("Ventilador")                      sessao1.h5
  append × N frames                      sessao2.h5   ──→ consolidate_hdf5_files
stop()                                   sessao3.h5
start("Ferro")                                │
  append × M frames                           ↓
stop()                                   dataset_final.h5
  ↓
export_nilmtk_hdf5("dataset.h5")
  ├─ /building1/elec/meter1  ← Ventilador
  └─ /building1/elec/meter2  ← Ferro
```

### Protocolo binário (frames do STM32)

| Campo         | Frame 0x01 Power  | Frame 0x02 Harmonics |
|---------------|-------------------|----------------------|
| Magic + tipo  | `AB CD 01`        | `AB CD 02`           |
| Payload       | Vrms Irms FP P Q S (6×f32) + 128 pares i16 | THD_V THD_I (2×f32) + harm_V[50] harm_I[50] (100×f32) |
| Footer        | `EF FE`           | `EF FE`              |
| Tamanho       | **543 bytes**     | **413 bytes**        |
| Taxa          | **~15 Hz**        | **~1.5 Hz**          |
