/**
 * @file    web_server.cpp
 * @brief   WiFi AP + Async WebServer + WebSocket for NILM dashboard
 *
 * WiFi runs in Access Point mode so there's no need for an external router.
 * The dashboard is served as an embedded HTML string (no LittleFS needed).
 *
 * Endpoints:
 *   GET /           → Dashboard HTML page
 *   WS  /ws         → WebSocket for real-time data push
 *   GET /api/cal    → Read calibration as JSON
 *   POST /api/cal   → Update calibration from JSON body
 *   POST /api/cal/reset → Reset calibration to defaults
 */

#include "web_server.h"
#include "calibration.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

/* ---- WiFi AP Configuration ---- */
#define WIFI_AP_SSID     "NILM-Monitor"
#define WIFI_AP_PASSWORD "nilm2026"   /* min 8 chars for WPA2 */
#define WIFI_AP_CHANNEL  1

/* ---- Server instances ---- */
static AsyncWebServer  server(80);
static AsyncWebSocket  ws("/ws");

/* ---- Connected client tracking ---- */
static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[WS] Client #%u connected from %s\n",
                          client->id(), client->remoteIP().toString().c_str());
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("[WS] Client #%u disconnected\n", client->id());
            break;
        case WS_EVT_ERROR:
            Serial.printf("[WS] Client #%u error\n", client->id());
            break;
        case WS_EVT_DATA:
            /* Client → Server messages (not used currently) */
            break;
        default:
            break;
    }
}

/* ================================================================== */
/*                    Embedded Dashboard HTML                         */
/* ================================================================== */
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>NILM Monitor — IFMT</title>
<style>
  :root {
    --bg: #0f0f1a;
    --card-bg: #1a1a2e;
    --card-border: #2d2d4a;
    --accent: #00d4ff;
    --accent2: #7c3aed;
    --text: #e2e8f0;
    --text-dim: #94a3b8;
    --success: #22c55e;
    --warning: #f59e0b;
    --danger: #ef4444;
  }
  * { margin:0; padding:0; box-sizing:border-box; }
  body {
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
  }
  .header {
    background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
    border-bottom: 1px solid var(--card-border);
    padding: 1rem 1.5rem;
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .header h1 {
    font-size: 1.3rem;
    background: linear-gradient(90deg, var(--accent), var(--accent2));
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
  }
  .status-badge {
    display: flex; align-items: center; gap: 6px;
    font-size: 0.8rem; color: var(--text-dim);
  }
  .status-dot {
    width: 8px; height: 8px; border-radius: 50%;
    background: var(--danger);
    animation: pulse 2s infinite;
  }
  .status-dot.connected { background: var(--success); }
  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:.4} }

  .container { max-width: 900px; margin: 0 auto; padding: 1.5rem; }

  /* ---- Metric Cards ---- */
  .metrics-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
    gap: 1rem;
    margin-bottom: 1.5rem;
  }
  .card {
    background: var(--card-bg);
    border: 1px solid var(--card-border);
    border-radius: 12px;
    padding: 1.2rem;
    transition: transform 0.2s, box-shadow 0.2s;
  }
  .card:hover {
    transform: translateY(-2px);
    box-shadow: 0 8px 25px rgba(0,212,255,0.08);
  }
  .card-label {
    font-size: 0.75rem;
    color: var(--text-dim);
    text-transform: uppercase;
    letter-spacing: 1px;
    margin-bottom: 0.5rem;
  }
  .card-value {
    font-size: 2rem;
    font-weight: 700;
    line-height: 1;
  }
  .card-unit {
    font-size: 0.9rem;
    color: var(--text-dim);
    margin-left: 4px;
  }
  .card-value.voltage { color: var(--accent); }
  .card-value.current { color: #f59e0b; }
  .card-value.power-active { color: var(--success); }
  .card-value.power-apparent { color: var(--accent2); }
  .card-value.pf { color: #f472b6; }
  .card-value.freq { color: #38bdf8; }

  /* ---- Bar Chart ---- */
  .bar-section {
    background: var(--card-bg);
    border: 1px solid var(--card-border);
    border-radius: 12px;
    padding: 1.2rem;
    margin-bottom: 1.5rem;
  }
  .bar-section h3 {
    font-size: 0.85rem; color: var(--text-dim);
    text-transform: uppercase; letter-spacing: 1px;
    margin-bottom: 1rem;
  }
  .bar-row {
    display: flex; align-items: center; gap: 10px;
    margin-bottom: 0.7rem;
  }
  .bar-label { width: 30px; font-size: 0.75rem; color: var(--text-dim); text-align: right; }
  .bar-track {
    flex: 1; height: 20px; background: rgba(255,255,255,0.05);
    border-radius: 10px; overflow: hidden;
  }
  .bar-fill {
    height: 100%; border-radius: 10px;
    transition: width 0.5s ease;
  }
  .bar-val { width: 70px; font-size: 0.8rem; text-align: right; }

  /* ---- Calibration Panel ---- */
  .cal-section {
    background: var(--card-bg);
    border: 1px solid var(--card-border);
    border-radius: 12px;
    padding: 1.2rem;
  }
  .cal-section h3 {
    font-size: 0.85rem; color: var(--text-dim);
    text-transform: uppercase; letter-spacing: 1px;
    margin-bottom: 1rem;
  }
  .cal-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 0.8rem;
    margin-bottom: 1rem;
  }
  .cal-field label {
    display: block; font-size: 0.75rem; color: var(--text-dim);
    margin-bottom: 4px;
  }
  .cal-field input {
    width: 100%; padding: 8px 10px;
    background: rgba(255,255,255,0.05);
    border: 1px solid var(--card-border);
    border-radius: 8px;
    color: var(--text); font-size: 0.9rem;
    outline: none;
    transition: border-color 0.2s;
  }
  .cal-field input:focus { border-color: var(--accent); }
  .btn-row { display: flex; gap: 0.5rem; flex-wrap: wrap; }
  .btn {
    padding: 8px 20px; border: none; border-radius: 8px;
    font-size: 0.85rem; cursor: pointer;
    transition: transform 0.1s, opacity 0.2s;
  }
  .btn:active { transform: scale(0.97); }
  .btn-primary {
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    color: #fff;
  }
  .btn-secondary {
    background: rgba(255,255,255,0.08); color: var(--text);
    border: 1px solid var(--card-border);
  }
  .btn-danger {
    background: rgba(239,68,68,0.15); color: var(--danger);
    border: 1px solid rgba(239,68,68,0.3);
  }
  .toast {
    position: fixed; bottom: 20px; right: 20px;
    padding: 10px 20px; border-radius: 8px;
    font-size: 0.85rem; opacity: 0;
    transition: opacity 0.3s;
    z-index: 100;
  }
  .toast.show { opacity: 1; }
  .toast.success { background: rgba(34,197,94,0.2); color: var(--success); border: 1px solid rgba(34,197,94,0.3); }
  .toast.error { background: rgba(239,68,68,0.2); color: var(--danger); border: 1px solid rgba(239,68,68,0.3); }

  .info-footer {
    text-align: center; padding: 1.5rem; font-size: 0.7rem; color: var(--text-dim);
  }
</style>
</head>
<body>

<div class="header">
  <h1>⚡ NILM Monitor</h1>
  <div class="status-badge">
    <div class="status-dot" id="statusDot"></div>
    <span id="statusText">Desconectado</span>
  </div>
</div>

<div class="container">

  <!-- Metric Cards -->
  <div class="metrics-grid">
    <div class="card">
      <div class="card-label">Tensão RMS</div>
      <div class="card-value voltage" id="vrms">---<span class="card-unit">V</span></div>
    </div>
    <div class="card">
      <div class="card-label">Corrente RMS</div>
      <div class="card-value current" id="irms">---<span class="card-unit">A</span></div>
    </div>
    <div class="card">
      <div class="card-label">Potência Ativa</div>
      <div class="card-value power-active" id="pactive">---<span class="card-unit">W</span></div>
    </div>
    <div class="card">
      <div class="card-label">Potência Aparente</div>
      <div class="card-value power-apparent" id="papparent">---<span class="card-unit">VA</span></div>
    </div>
    <div class="card">
      <div class="card-label">Fator de Potência</div>
      <div class="card-value pf" id="powerfactor">---</div>
    </div>
    <div class="card">
      <div class="card-label">Frequência</div>
      <div class="card-value freq" id="frequency">---<span class="card-unit">Hz</span></div>
    </div>
  </div>

  <!-- Visual Bars -->
  <div class="bar-section">
    <h3>Distribuição de Potência</h3>
    <div class="bar-row">
      <div class="bar-label">P</div>
      <div class="bar-track">
        <div class="bar-fill" id="barP" style="width:0%; background:linear-gradient(90deg,#22c55e,#16a34a)"></div>
      </div>
      <div class="bar-val" id="barPval">0 W</div>
    </div>
    <div class="bar-row">
      <div class="bar-label">S</div>
      <div class="bar-track">
        <div class="bar-fill" id="barS" style="width:0%; background:linear-gradient(90deg,#7c3aed,#a855f7)"></div>
      </div>
      <div class="bar-val" id="barSval">0 VA</div>
    </div>
    <div class="bar-row">
      <div class="bar-label">PF</div>
      <div class="bar-track">
        <div class="bar-fill" id="barPF" style="width:0%; background:linear-gradient(90deg,#f472b6,#ec4899)"></div>
      </div>
      <div class="bar-val" id="barPFval">0.00</div>
    </div>
  </div>

  <!-- Calibration Panel -->
  <div class="cal-section">
    <h3>🔧 Calibração (Fluke 179)</h3>
    <div class="cal-grid">
      <div class="cal-field">
        <label>Ganho Tensão</label>
        <input type="number" step="0.001" id="calVgain" value="1.000">
      </div>
      <div class="cal-field">
        <label>Offset Tensão (V)</label>
        <input type="number" step="0.1" id="calVoffset" value="0.0">
      </div>
      <div class="cal-field">
        <label>Ganho Corrente</label>
        <input type="number" step="0.001" id="calIgain" value="1.000">
      </div>
      <div class="cal-field">
        <label>Offset Corrente (A)</label>
        <input type="number" step="0.01" id="calIoffset" value="0.0">
      </div>
      <div class="cal-field">
        <label>Ganho Potência</label>
        <input type="number" step="0.001" id="calPgain" value="1.000">
      </div>
      <div class="cal-field">
        <label>Offset Potência (W)</label>
        <input type="number" step="0.1" id="calPoffset" value="0.0">
      </div>
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" onclick="saveCal()">Salvar Calibração</button>
      <button class="btn btn-secondary" onclick="loadCal()">Recarregar</button>
      <button class="btn btn-danger" onclick="resetCal()">Reset Padrão</button>
    </div>
  </div>

</div>

<div class="info-footer">
  NILM Energy Monitor — Projeto de Extensão IFMT &bull; Pacote #<span id="pktCount">0</span>
</div>

<div class="toast" id="toast"></div>

<script>
  /* ---- WebSocket ---- */
  let ws;
  const dot = document.getElementById('statusDot');
  const stxt = document.getElementById('statusText');

  function connectWS() {
    ws = new WebSocket('ws://' + location.host + '/ws');
    ws.onopen = () => {
      dot.classList.add('connected');
      stxt.textContent = 'Conectado';
      loadCal();
    };
    ws.onclose = () => {
      dot.classList.remove('connected');
      stxt.textContent = 'Desconectado';
      setTimeout(connectWS, 2000);
    };
    ws.onerror = () => ws.close();
    ws.onmessage = (evt) => {
      try {
        const d = JSON.parse(evt.data);
        updateUI(d);
      } catch(e) {}
    };
  }
  connectWS();

  /* ---- Update UI ---- */
  function updateUI(d) {
    setText('vrms', d.vrms.toFixed(1));
    setText('irms', d.irms.toFixed(3));
    setText('pactive', d.P.toFixed(1));
    setText('papparent', d.S.toFixed(1));
    setText('powerfactor', d.PF.toFixed(3));
    setText('frequency', d.freq.toFixed(1));
    document.getElementById('pktCount').textContent = d.n || 0;

    /* Bars — assume max 5000W for scaling */
    const maxW = Math.max(d.S, 1000);
    setBar('barP', 'barPval', d.P, maxW, 'W');
    setBar('barS', 'barSval', d.S, maxW, 'VA');
    document.getElementById('barPF').style.width = (Math.abs(d.PF)*100)+'%';
    document.getElementById('barPFval').textContent = d.PF.toFixed(3);
  }

  function setText(id, val) {
    const el = document.getElementById(id);
    const unit = el.querySelector('.card-unit');
    const unitText = unit ? unit.outerHTML : '';
    el.innerHTML = val + unitText;
  }

  function setBar(barId, valId, val, max, unit) {
    const pct = Math.min((Math.abs(val)/max)*100, 100);
    document.getElementById(barId).style.width = pct+'%';
    document.getElementById(valId).textContent = val.toFixed(1)+' '+unit;
  }

  /* ---- Calibration API ---- */
  async function loadCal() {
    try {
      const r = await fetch('/api/cal');
      const c = await r.json();
      document.getElementById('calVgain').value = c.vg;
      document.getElementById('calVoffset').value = c.vo;
      document.getElementById('calIgain').value = c.ig;
      document.getElementById('calIoffset').value = c.io;
      document.getElementById('calPgain').value = c.pg;
      document.getElementById('calPoffset').value = c.po;
    } catch(e) { showToast('Erro ao carregar calibração', 'error'); }
  }

  async function saveCal() {
    const body = {
      vg: parseFloat(document.getElementById('calVgain').value),
      vo: parseFloat(document.getElementById('calVoffset').value),
      ig: parseFloat(document.getElementById('calIgain').value),
      io: parseFloat(document.getElementById('calIoffset').value),
      pg: parseFloat(document.getElementById('calPgain').value),
      po: parseFloat(document.getElementById('calPoffset').value)
    };
    try {
      const r = await fetch('/api/cal', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify(body)
      });
      if (r.ok) showToast('Calibração salva!', 'success');
      else showToast('Erro ao salvar', 'error');
    } catch(e) { showToast('Erro de conexão', 'error'); }
  }

  async function resetCal() {
    if (!confirm('Resetar calibração para valores padrão?')) return;
    try {
      const r = await fetch('/api/cal/reset', { method: 'POST' });
      if (r.ok) { loadCal(); showToast('Calibração resetada!', 'success'); }
    } catch(e) { showToast('Erro', 'error'); }
  }

  function showToast(msg, type) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.className = 'toast show ' + type;
    setTimeout(() => t.className = 'toast', 3000);
  }
</script>
</body>
</html>
)rawliteral";

/* ================================================================== */
/*                    Route Handlers                                 */
/* ================================================================== */

static void handle_get_calibration(AsyncWebServerRequest *request)
{
    const calibration_t *cal = calibration_get();
    JsonDocument doc;
    doc["vg"] = cal->voltage_gain;
    doc["vo"] = cal->voltage_offset;
    doc["ig"] = cal->current_gain;
    doc["io"] = cal->current_offset;
    doc["pg"] = cal->power_gain;
    doc["po"] = cal->power_offset;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handle_post_calibration(AsyncWebServerRequest *request,
                                     uint8_t *data, size_t len,
                                     size_t index, size_t total)
{
    /* Accumulate body — for small JSON this comes in one chunk */
    if (index == 0 && len == total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, (const char *)data, len);
        if (err) {
            request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        calibration_t cal;
        cal.voltage_gain   = doc["vg"] | 1.0f;
        cal.voltage_offset = doc["vo"] | 0.0f;
        cal.current_gain   = doc["ig"] | 1.0f;
        cal.current_offset = doc["io"] | 0.0f;
        cal.power_gain     = doc["pg"] | 1.0f;
        cal.power_offset   = doc["po"] | 0.0f;
        calibration_set(&cal);
        request->send(200, "application/json", "{\"ok\":true}");
    }
}

static void handle_reset_calibration(AsyncWebServerRequest *request)
{
    calibration_reset();
    request->send(200, "application/json", "{\"ok\":true}");
}

/* ================================================================== */
/*                    Public API                                     */
/* ================================================================== */

void web_server_init()
{
    /* Start WiFi Access Point */
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL);

    Serial.println("[WIFI] Access Point started");
    Serial.printf("[WIFI] SSID: %s  Password: %s\n", WIFI_AP_SSID, WIFI_AP_PASSWORD);
    Serial.printf("[WIFI] IP: %s\n", WiFi.softAPIP().toString().c_str());

    /* WebSocket */
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    /* Routes */
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", INDEX_HTML);
    });

    server.on("/api/cal", HTTP_GET, handle_get_calibration);

    server.on("/api/cal", HTTP_POST,
        [](AsyncWebServerRequest *request) { /* handler is in body callback */ },
        NULL,
        handle_post_calibration
    );

    server.on("/api/cal/reset", HTTP_POST, handle_reset_calibration);

    /* Start server */
    server.begin();
    Serial.println("[WEB] Server started on port 80");
}

void web_server_broadcast(const nilm_data_t *data)
{
    if (ws.count() == 0) return;  /* No clients connected */

    JsonDocument doc;
    doc["vrms"] = serialized(String(data->vrms, 2));
    doc["irms"] = serialized(String(data->irms, 4));
    doc["P"]    = serialized(String(data->power_active, 2));
    doc["S"]    = serialized(String(data->power_apparent, 2));
    doc["PF"]   = serialized(String(data->power_factor, 4));
    doc["freq"] = serialized(String(data->frequency, 2));
    doc["n"]    = data->sample_count;

    String json;
    serializeJson(doc, json);
    ws.textAll(json);

    /* Periodic cleanup of disconnected clients */
    ws.cleanupClients();
}
