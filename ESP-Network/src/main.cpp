#include <Arduino.h>
#include <string.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

// ── Constantes ────────────────────────────────────────────────────────────────
#define KWH_FILE              "/kwh.bin"
#define KWH_SAVE_INTERVAL_MS  60000u   // salva no LittleFS a cada 60 s
#define SAMPLE_PERIOD_S       0.080f   // STM32 envia a cada ~80 ms (66.7ms aquisição + processamento)
#define MQTT_QUEUE_LEN        8        // frames em espera para publicação
#define MQTT_TOPIC_HARM       "energia/harmonicas"
#define HARM_MAX_ESP          50       // harmônicos no frame 0x02

// ── Protocolo IDF ─────────────────────────────────────────────────────────────
// Frame 0x01/0x03 (potência F1/F2):
//   [AB CD tt] [Vrms f4][Irms f4][FP f4][Preal f4][Q f4][S f4]
//   [N u8][step u8] [N × (V_i32 I_i32)] [EF FE]
//   Header pós-magic: 6×f32 (24 B) + N (1 B) + step (1 B) = 26 B
//   Amostras: N × 2 × 4 B = N*8 B  (int32 desde Jun/2026 — era int16 N*4 B)
//   Total típico (N=128): 3 + 26 + 1024 + 2 = 1055 bytes
//
// Frame 0x02/0x04 (harmônicos F1/F2):
//   [AB CD tt] THD_V(f32) THD_I(f32) HarmV[50](f32) HarmI[50](f32) [EF FE]
//   Total: 413 bytes

enum State : uint8_t {
    WAIT_AB, WAIT_CD, WAIT_TYPE,
    READ_HDR,                       // frame 0x01/0x03: lê 26 bytes de header
    SKIP_SAMPLES,                   // frame 0x01/0x03: descarta N*8 bytes de amostras (int32)
    WAIT_EF, WAIT_FE,
    READ_HARM,                      // frame 0x02/0x04: lê 408 bytes (THD_V+THD_I+50*V+50*I floats)
    WAIT_HARM_EF, WAIT_HARM_FE
};

static HardwareSerial &stmSerial = Serial2;
static const int BTN_PIN = 0;

static State    state    = WAIT_AB;
static uint8_t  hdr[26];
static uint8_t  hdrPos   = 0;
static uint32_t skipLeft = 0;

// frame 0x02 — harmônicos
static uint8_t  harmBuf[408];   // THD_V(4) + THD_I(4) + HarmV[50](200) + HarmI[50](200)
static uint16_t harmPos  = 0;

static char    lineBuf[200];
static uint8_t linePos  = 0;
static bool    inLine   = false;

static uint32_t pktOk   = 0;
static uint32_t pktErr  = 0;
static uint8_t  curPhase = 1u;   // fase do frame em processamento (1 ou 2)

// ── Dados compartilhados Core 1 → Core 0 (via fila FreeRTOS) ─────────────────
struct MeterData {
    float   vrms, irms, fp, preal, q, s;
    double  kwh;
    time_t  ts;
    uint8_t phase;
};

static QueueHandle_t mqttQueue;

// ── Acumulador kWh (apenas escrito no Core 1) ─────────────────────────────────
static double g_kwh = 0.0;
static volatile bool debugMode = false;

static void kwh_load(void)
{
    if (!LittleFS.exists(KWH_FILE)) return;
    File f = LittleFS.open(KWH_FILE, "r");
    if (!f) return;
    f.read((uint8_t *)&g_kwh, sizeof(g_kwh));
    f.close();
}

static void kwh_save(double value)
{
    File f = LittleFS.open(KWH_FILE, "w");
    if (!f) return;
    f.write((uint8_t *)&value, sizeof(value));
    f.close();
}

// ── Forward declarations ───────────────────────────────────────────────────────
static WiFiClientSecure wifiClient;
static PubSubClient     mqttClient(wifiClient);

// ── Parser IDF ────────────────────────────────────────────────────────────────
static void emitHarmonics(void)
{
    float thd_v, thd_i;
    float harmV[HARM_MAX_ESP], harmI[HARM_MAX_ESP];
    memcpy(&thd_v, &harmBuf[0],                   4);
    memcpy(&thd_i, &harmBuf[4],                   4);
    memcpy(harmV,  &harmBuf[8],                   HARM_MAX_ESP * 4);
    memcpy(harmI,  &harmBuf[8 + HARM_MAX_ESP * 4], HARM_MAX_ESP * 4);

    if (isnan(thd_v) || isnan(thd_i)) { pktErr++; return; }

    // Tamanho: 2 arrays×50 floats × ~8B + overhead + ts/thd ≈ 1400 B
    StaticJsonDocument<2048> doc;

    char ts_buf[32];
    if (time(nullptr) > 1577836800LL) {
        struct tm tm_info;
        time_t now = time(nullptr);
        localtime_r(&now, &tm_info);
        strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    } else {
        snprintf(ts_buf, sizeof(ts_buf), "unsynced");
    }

    doc["ts"]    = ts_buf;
    doc["phase"] = curPhase;
    doc["thd_v"] = serialized(String(thd_v, 4));
    doc["thd_i"] = serialized(String(thd_i, 4));

    // Arrays completos harm_v[50] e harm_i[50] — formato esperado pelo mqtt_ingest.py
    JsonArray arrV = doc.createNestedArray("harm_v");
    JsonArray arrI = doc.createNestedArray("harm_i");
    for (int h = 0; h < HARM_MAX_ESP; h++) {
        arrV.add(serialized(String(harmV[h], 4)));
        arrI.add(serialized(String(harmI[h], 4)));
    }

    char payload[1500];
    if (mqttClient.connected()) {
        size_t len = serializeJson(doc, payload, sizeof(payload));
        mqttClient.publish(MQTT_TOPIC_HARM, payload, len);
    }
    pktOk++;
}

static void emitData(void)
{
    float Vrms, Irms, FP, Preal, Q, S;
    memcpy(&Vrms,  &hdr[0],  4);
    memcpy(&Irms,  &hdr[4],  4);
    memcpy(&FP,    &hdr[8],  4);
    memcpy(&Preal, &hdr[12], 4);
    memcpy(&Q,     &hdr[16], 4);
    memcpy(&S,     &hdr[20], 4);

    if (isnan(Vrms) || isnan(Irms) || isnan(Preal) || isnan(S)) {
        pktErr++;
        return;
    }
    pktOk++;

    // Acumula kWh: P[W] × t[s] / 3 600 000 = kWh
    if (Preal > 0.0f)
        g_kwh += (double)Preal * SAMPLE_PERIOD_S / 3600000.0;

    MeterData data = {};
    data.vrms  = Vrms;
    data.irms  = Irms;
    data.fp    = FP;
    data.preal = Preal;
    data.q     = Q;
    data.s     = S;
    data.kwh   = g_kwh;
    data.ts    = time(nullptr);
    data.phase = curPhase;

    // Não bloqueia o parser se a fila estiver cheia
    xQueueSend(mqttQueue, &data, 0);

    // CSV mantido para compatibilidade com gui_bridge.py
    Serial.printf("METER,%.2f,%.4f,%.2f,%.2f,%.2f,%.4f\r\n",
                  Vrms, Irms, Preal, S, Q, FP);
}

static void processByte(uint8_t b)
{
    if (state == WAIT_AB) {
        if (inLine) {
            if (b == '\n' || linePos >= (uint8_t)(sizeof(lineBuf) - 1)) {
                lineBuf[linePos] = '\0';
                Serial.println(lineBuf);
                inLine  = false;
                linePos = 0;
            } else if (b != '\r') {
                lineBuf[linePos++] = (char)b;
            }
            return;
        }
        if (b == (uint8_t)'#') {
            lineBuf[0] = '#';
            linePos    = 1;
            inLine     = true;
            return;
        }
    }

    switch (state) {
    case WAIT_AB:
        if (b == 0xABu) state = WAIT_CD;
        break;
    case WAIT_CD:
        state = (b == 0xCDu) ? WAIT_TYPE : WAIT_AB;
        break;
    case WAIT_TYPE:
        if      (b == 0x01u) { curPhase = 1u; hdrPos = 0;  state = READ_HDR;  }
        else if (b == 0x03u) { curPhase = 2u; hdrPos = 0;  state = READ_HDR;  }
        else if (b == 0x02u) { curPhase = 1u; harmPos = 0; state = READ_HARM; }
        else if (b == 0x04u) { curPhase = 2u; harmPos = 0; state = READ_HARM; }
        else                   state = WAIT_AB;
        break;
    case READ_HDR:
        hdr[hdrPos++] = b;
        if (hdrPos == 26u) {
            uint8_t N = hdr[24];
            skipLeft  = (uint32_t)N * 8u;   // int32: 2×int32 por amostra = 8 bytes
            state     = (skipLeft > 0u) ? SKIP_SAMPLES : WAIT_EF;
        }
        break;
    case SKIP_SAMPLES:
        if (--skipLeft == 0u) state = WAIT_EF;
        break;
    case WAIT_EF:
        state = (b == 0xEFu) ? WAIT_FE : WAIT_AB;
        if (b != 0xEFu) pktErr++;
        break;
    case WAIT_FE:
        if (b == 0xFEu) emitData();
        else             pktErr++;
        state = WAIT_AB;
        break;
    case READ_HARM:
        harmBuf[harmPos++] = b;
        if (harmPos == 408u) state = WAIT_HARM_EF;
        break;
    case WAIT_HARM_EF:
        state = (b == 0xEFu) ? WAIT_HARM_FE : WAIT_AB;
        if (b != 0xEFu) pktErr++;
        break;
    case WAIT_HARM_FE:
        if (b == 0xFEu) emitHarmonics();
        else             pktErr++;
        state = WAIT_AB;
        break;
    }
}

// ── Task MQTT — pinada no Core 0 (stack TCP/IP do ESP-IDF) ───────────────────

static void wifi_connect(void)
{
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.printf("# [ESP32 WiFi] conectando a %s\r\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 15000u) {
        if (debugMode) return;
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        /* Força DNS do Google — evita falha de resolução quando o DHCP demora
         * a propagar o DNS do roteador (erro: DNS Failed for hivemq.cloud) */
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                    IPAddress(8, 8, 8, 8), IPAddress(8, 8, 4, 4));
        delay(200);
        Serial.printf("# [ESP32 WiFi] IP: %s  DNS: 8.8.8.8\r\n",
                      WiFi.localIP().toString().c_str());
        configTzTime(NTP_TZ, NTP_SERVER);
        wifiClient.setInsecure();
    } else {
        Serial.println("# [ESP32 WiFi] falha — tentará novamente");
    }
}

static void mqtt_connect(void)
{
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setKeepAlive(30);
    uint8_t tries = 0;
    while (!mqttClient.connected() && tries < 3) {
        if (debugMode) return;
        if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
            Serial.println("# [ESP32 MQTT] conectado");
        } else {
            Serial.printf("# [ESP32 MQTT] falha rc=%d, retry\r\n", mqttClient.state());
            delay(2000);
        }
        tries++;
    }
}

static void mqtt_task(void *pvParams)
{
    // Core 0 — mesma afinidade do stack TCP/IP
    wifi_connect();
    mqtt_connect();

    MeterData data;
    StaticJsonDocument<384> doc;
    char payload[384];
    char ts_buf[32];
    double kwhToSave   = 0.0;
    uint32_t lastSaveMs = 0;

    for (;;) {
        // Reconexão automática Wi-Fi / MQTT
        if (!debugMode) {
            if (WiFi.status() != WL_CONNECTED) {
                wifi_connect();
            }
            if (!mqttClient.connected()) {
                mqtt_connect();
            }
        }
        mqttClient.loop();

        // Aguarda frame da fila (até 100 ms) — mantém loop() leve
        if (xQueueReceive(mqttQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE) {
            kwhToSave = data.kwh;

            // Timestamp ISO 8601 — exige NTP sincronizado (ts > 2020-01-01)
            if (data.ts > 1577836800LL) {
                struct tm tm_info;
                localtime_r(&data.ts, &tm_info);
                strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
            } else {
                snprintf(ts_buf, sizeof(ts_buf), "unsynced");
            }

            doc.clear();
            doc["ts"]    = ts_buf;
            doc["phase"] = data.phase;
            doc["vrms"]  = serialized(String(data.vrms,  2));
            doc["irms"]  = serialized(String(data.irms,  4));
            doc["preal"] = serialized(String(data.preal, 2));
            doc["s"]     = serialized(String(data.s,     2));
            doc["q"]     = serialized(String(data.q,     2));
            doc["fp"]    = serialized(String(data.fp,    4));
            doc["kwh"]   = serialized(String(data.kwh,   6));

            if (mqttClient.connected()) {
                size_t len = serializeJson(doc, payload, sizeof(payload));
                mqttClient.publish(MQTT_TOPIC, payload, len);
            }
        }

        // Persiste kWh no LittleFS a cada KWH_SAVE_INTERVAL_MS
        if ((millis() - lastSaveMs) >= KWH_SAVE_INTERVAL_MS) {
            lastSaveMs = millis();
            kwh_save(kwhToSave);
        }
    }
}

// ── Setup / Loop — Core 1 ─────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("# [ESP32 BOOT] ESP-Network v2 | MQTT + LittleFS");

    // US12: monta LittleFS e recupera kWh acumulado
    if (!LittleFS.begin(true))
        Serial.println("# [ESP32 LittleFS] falha no mount");
    else
        kwh_load();

    Serial.printf("# [ESP32] kWh recuperado: %.6f\r\n", g_kwh);

    stmSerial.begin(460800, SERIAL_8N1, 16, 17);
    pinMode(BTN_PIN, INPUT_PULLUP);

    mqttQueue = xQueueCreate(MQTT_QUEUE_LEN, sizeof(MeterData));
    mqttClient.setBufferSize(1200);   // necessário para payload de harmônicos (~1400 B serializado)

    // US9: task MQTT pinada no Core 0 (stack TCP/IP do ESP-IDF roda lá)
    xTaskCreatePinnedToCore(
        mqtt_task, "mqtt_task",
        8192,    /* stack bytes */
        nullptr,
        1,       /* priority */
        nullptr,
        0        /* Core 0 */
    );
}

void loop()
{
    // Core 1: parser IDF em tempo real
    while (stmSerial.available())
        processByte((uint8_t)stmSerial.read());

    // Botão BOOT (GPIO0, ativo baixo) — status via serial
    static uint32_t btnLastMs = 0u;
    if (digitalRead(BTN_PIN) == LOW && (millis() - btnLastMs) >= 300u) {
        btnLastMs = millis();
        debugMode = !debugMode;
        if (debugMode) {
            Serial.println("# [ESP32] MODO DEBUG ATIVADO: Reconexoes desabilitadas. Lendo apenas serial.");
        } else {
            Serial.println("# [ESP32] MODO NORMAL ATIVADO: Reconexoes habilitadas.");
        }
        Serial.printf("# [ESP32] up:%lus pktOk:%lu pktErr:%lu kWh:%.6f WiFi:%d MQTT:%d Debug:%d\r\n",
                      (unsigned long)(millis() / 1000u),
                      (unsigned long)pktOk, (unsigned long)pktErr,
                      g_kwh,
                      (int)(WiFi.status() == WL_CONNECTED),
                      (int)mqttClient.connected(),
                      (int)debugMode);
    }
}
