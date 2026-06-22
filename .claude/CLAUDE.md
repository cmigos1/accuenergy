# context-mode — MANDATORY routing rules

context-mode MCP tools available. Rules protect context window from flooding. One unrouted command dumps 56 KB into context.

## Think in Code — MANDATORY

Analyze/count/filter/compare/search/parse/transform data: **write code** via `ctx_execute(language, code)`, `console.log()` only the answer. Do NOT read raw data into context. PROGRAM the analysis, not COMPUTE it. Pure JavaScript — Node.js built-ins only (`fs`, `path`, `child_process`). `try/catch`, handle `null`/`undefined`. One script replaces ten tool calls.

## BLOCKED — do NOT attempt

### curl / wget — BLOCKED
Intercepted and replaced with error. Do NOT retry.
Use: `ctx_fetch_and_index(url, source)` or `ctx_execute(language: "javascript", code: "const r = await fetch(...)")`

### Inline HTTP — BLOCKED
`fetch('http`, `requests.get(`, `requests.post(`, `http.get(`, `http.request(` — intercepted. Do NOT retry.
Use: `ctx_execute(language, code)` — only stdout enters context

### WebFetch — BLOCKED
Use: `ctx_fetch_and_index(url, source)` then `ctx_search(queries)`

## REDIRECTED — use sandbox

### Bash (>20 lines output)
Bash ONLY for: `git`, `mkdir`, `rm`, `mv`, `cd`, `ls`, `npm install`, `pip install`.
Otherwise: `ctx_batch_execute(commands, queries)` or `ctx_execute(language: "shell", code: "...")`

### Read (for analysis)
Reading to **Edit** → Read correct. Reading to **analyze/explore/summarize** → `ctx_execute_file(path, language, code)`.

### Grep — may flood context
Use `ctx_execute(language: "shell", code: "grep ...")` in sandbox.

## Tool selection

0. **MEMORY**: `ctx_search(sort: "timeline")` — after resume, check prior context before asking user.
1. **GATHER**: `ctx_batch_execute(commands, queries)` — runs all commands, auto-indexes, returns search. ONE call replaces 30+. Each command: `{label: "header", command: "..."}`.
2. **FOLLOW-UP**: `ctx_search(queries: ["q1", "q2", ...])` — all questions as array, ONE call (default relevance mode).
3. **PROCESSING**: `ctx_execute(language, code)` | `ctx_execute_file(path, language, code)` — sandbox, only stdout enters context.
4. **WEB**: `ctx_fetch_and_index(url, source)` then `ctx_search(queries)` — raw HTML never enters context.
5. **INDEX**: `ctx_index(content, source)` — store in FTS5 for later search.

## Parallel I/O batches

For multi-URL fetches or multi-API calls, **always** include `concurrency: N` (1-8):

- `ctx_batch_execute(commands: [3+ network commands], concurrency: 5)` — gh, curl, dig, docker inspect, multi-region cloud queries
- `ctx_fetch_and_index(requests: [{url, source}, ...], concurrency: 5)` — multi-URL batch fetch

**Use concurrency 4-8** for I/O-bound work (network calls, API queries). **Keep concurrency 1** for CPU-bound (npm test, build, lint) or commands sharing state (ports, lock files, same-repo writes).

GitHub API rate-limit: cap at 4 for `gh` calls.

## Subagent routing

Routing block auto-injected into subagent prompts. Bash-type subagents upgraded to general-purpose. No manual instruction needed.

## Output

Write artifacts to FILES — never inline. Return: file path + 1-line description.
Descriptive source labels for `ctx_search(source: "label")`.

## Session Continuity

Skills, roles, and decisions persist for the entire session. Do not abandon them as the conversation grows.

## Memory

Session history is persistent and searchable. On resume, search BEFORE asking the user:

| Need | Command |
|------|---------|
| What were we working on? | `ctx_search(queries: ["summary"], source: "compaction", sort: "timeline")` |
| What was the first request? | `ctx_search(queries: ["prompt"], source: "user-prompt", sort: "timeline")` |
| What did we decide? | `ctx_search(queries: ["decision"], source: "decision", sort: "timeline")` |
| What NOT to repeat? | `ctx_search(queries: ["rejected"], source: "rejected-approach")` |
| What constraints exist? | `ctx_search(queries: ["constraint"], source: "constraint")` |

DO NOT ask "what were we working on?" — SEARCH FIRST.
If search returns 0 results, proceed as a fresh session.

## ctx commands

| Command | Action |
|---------|--------|
| `ctx stats` | Call `ctx_stats` MCP tool, display full output verbatim |
| `ctx doctor` | Call `ctx_doctor` MCP tool, run returned shell command, display as checklist |
| `ctx upgrade` | Call `ctx_upgrade` MCP tool, run returned shell command, display as checklist |
| `ctx purge` | Call `ctx_purge` MCP tool with confirm: true. Warns before wiping knowledge base. |

After /clear or /compact: knowledge base and session stats preserved. Use `ctx purge` to start fresh.

---

# Projeto AccuEnergy — referência rápida

## Componentes

| Módulo | Path | Papel |
|---|---|---|
| Firmware STM32 | `STM-Amostrador/Core/Src/main.c` | Aquisição ADC/DMA, cálculos, transmissão binária |
| Constantes calibração | `STM-Amostrador/Core/Inc/calculo.h` | CAL_V, CAL_I, Fs, F0_BIN, HARM_MAX, pisos de ruído |
| Driver display | `STM-Amostrador/Core/Src/st7735.c` | SPI bit-bang para ST7735 80×160 |
| ESP32 bridge | `ESP32-Bridge/src/main.cpp` | Ponte UART↔USB transparente |
| ESP32 IoT | `ESP-Network/src/main.cpp` | WiFi + MQTT + kWh + LittleFS |
| Monitor Python | `python-monitor/monitor.py` | GUI PyQt5 — visualização via ESP32-Bridge |
| Ingestor MQTT | `python-monitor/mqtt_ingest.py` | Assinante MQTT → CSV (roda no servidor .80) |
| Artigo | `docs/artigo/main.tex` | Artigo SBC — 7 seções, 3 figuras reais |
| NILMTK local | `nilmtk/` | Fork editável — Hart85 e CO funcionam; FHMM bloqueado (hmmlearn sem wheel Python 3.14) |

## Constantes de protocolo — devem estar sincronizadas entre STM32 e Python

| Constante | STM32 | Python | Valor atual |
|---|---|---|---|
| `FRAME1_TOTAL` | tamanho calculado implicitamente | `monitor.py:47` | **1055** bytes |
| `IDF_N` | `main.c:#define IDF_N` | `FRAME1_SAMPLES` | **128** |
| `IDF_STEP` | `main.c:#define IDF_STEP` | `IDF_STEP` | **8** |
| `FS_HZ` | `calculo.h` | `FS_HZ` | **15360** |
| `HARM_MAX` | `calculo.h` | `HARM_MAX` | **50** |
| Tipo amostras | `int32_t` | `struct '<Ni'` | **int32 LE** |

## Calibração

```
CAL_V = (3.3 / 65535) * 136     = 6.848e-3  V/count
CAL_I = (3.3 / 65535) * 40000   = 2.014e-3  A/count
        (40000 = CT_ratio 2000 / burden 50Ω)
```

Limite de representação int32: ±2.147.483.647 counts → sem overflow até o limite do ADC (±32767 counts para 16 bits, que o int32 cobre com folga).

## Piso de ruído (global, compartilhado entre fases)

```c
static float g_v_floor;   // calibrado no boot, fixo em V_NOISE_FLOOR_MIN=1.0V
static float g_i_floor;   // calibrado no boot: max_irms_ruído × 2.2
```

## Pipeline de frames por ciclo (~66 ms)

```
ADC1+ADC2 DMA completo
  → Deinterleave (ambas as fases)
  → ComputeAndSend(&ph1, 0x01)    ← 1055 bytes LPUART1
  → ComputeAndSend(&ph2, 0x03)    ← 1055 bytes LPUART1
  → a cada 10 frames:
      Harmonics_Compute(&ph1) + Send_HarmonicsFrame(&ph1, 0x02)   ← 413 bytes
      Harmonics_Compute(&ph2) + Send_HarmonicsFrame(&ph2, 0x04)   ← 413 bytes
  → ST7735_Main_Update(...)
  → reinicia DMA + TIM2
```

Taxa de transmissão estimada (steady state):
- Frames de potência: 2 × 1055 × 15 Hz = 31.650 bytes/s
- Frames harmônicos: 2 × 413 × 1.5 Hz = 1.239 bytes/s
- Total: ~32.889 bytes/s × 10 bits/byte = ~329 kbps < 460800 baud ✓

## Pipeline MQTT / Servidor

- Broker: Mosquitto em `192.168.1.80:1883`, allow_anonymous, bind 0.0.0.0
- Tópicos: `energia/medidor` (potência por fase) e `energia/harmonicas`
- `mqtt_ingest.py` roda como Task Scheduler (`/ru server /rp <pass>`) — reinício automático com `goto loop`
- Q gravado como `abs(Q)` — STM32 calcula magnitude √(S²−P²), sem sinal
- Credenciais servidor: usuário `server` — senha em variável de ambiente (não commitar)

## Análise NILM — resultados Jun/2026

- Detector Hart próprio (`_events.py`): limiar 50 W, baseline deslizante por fase
- K-means (ΔP, ΔQ): K=10/sil=0,44 em 1 dia → K=3/sil=0,74 em 2 dias (diluição por inverter)
- Subconjunto limpo |ΔP|>200 W: K=4, silhueta=0,50 (motor ~2,3 kW vs resistivos ~800 W)
- Teste de nulidade harmônicos: real ≈ embaralhado — harmônicos não discriminam eventos
- Hart85 NILMTK vs detector próprio: F2 <0,5% de diferença; F1 ~16% (parâmetros internos)
- FHMM bloqueado: hmmlearn não tem wheel para Python 3.14

## Artigo (docs/artigo/main.tex)

- 7 seções completas (SBC template, pt-BR)
- 3 figuras reais: `clusters_1d2d.png`, `degradacao.png`, `ablacao_harmonicos.png`
- 2 placeholders restantes: `fig:pipeline` (diagrama arquitetura) e `fig:resultados` (métricas — intencionalmente aguardando ground truth)
- Figura extra (não no artigo): `nilmtk_hart85_series.png` — comparativo visual Hart85

## Pendências conhecidas

- **ESP-Network** parser de amostras ainda usa `int16` — precisa migrar para `int32` (tamanho frame 0x01/0x03 muda de 543 → 1055).
- Schottky GND recomendado nos pinos PC4/PC5 (excursão negativa do SCT013 não é protegida pelo Zener).
- `SCT013_SIGN = -1.0f` porque o clamp está invertido no fio; reverter para `+1.0f` se reinstalar o CT na orientação correta.
- `fig:pipeline` e `fig:resultados` no artigo têm `\label` sem `\ref` — geram warning no LaTeX (não fatal).