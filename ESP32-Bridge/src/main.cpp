/**
 * AccuEnergy — ESP32 transparent UART bridge
 *
 * STM32H743 LPUART1 (460800 8N1)
 *   TX → GPIO16 (ESP32 UART2 RX)
 *   RX ← GPIO17 (ESP32 UART2 TX)  [opcional — para futuros comandos]
 *
 * USB Serial (UART0) → PC @ 460800 baud
 *
 * Frames do STM32 (protocolo binário) — análise bifásica:
 *   0x01 Power F1 : AB CD 01 + 6×f32 + 128×2×i16 + EF FE = 543 bytes @ ~15 Hz
 *   0x02 Harm  F1 : AB CD 02 + 2×f32 + 100×f32  + EF FE = 413 bytes @ ~1.5 Hz
 *   0x03 Power F2 : AB CD 03 + (mesmo layout 0x01)       = 543 bytes @ ~15 Hz
 *   0x04 Harm  F2 : AB CD 04 + (mesmo layout 0x02)       = 413 bytes @ ~1.5 Hz
 *   '#' lines     : debug ASCII, passadas integralmente ao PC
 *
 * A ponte é transparente (byte-a-byte), então os 4 tipos passam sem alteração
 * de lógica — só foram ampliados os buffers para o rajada bifásica (até ~1.9 KB
 * por janela: 0x01+0x03+0x02+0x04).
 *
 * LED onboard (GPIO2) pisca ao receber dados do STM32.
 */

#include <Arduino.h>

static constexpr uint32_t STM32_BAUD = 460800;
static constexpr uint32_t PC_BAUD    = 460800;
static constexpr int      UART2_RX   = 16;
static constexpr int      UART2_TX   = 17;
static constexpr int      RX_BUF_SZ  = 4096;  // bifásico: rajada até ~1.9 KB/janela
static constexpr int      TX_BUF_SZ  = 4096;
static constexpr uint8_t  LED_PIN    = 2;
static constexpr uint32_t LED_PERIOD = 60;     // ms — pisca ~16 Hz no pico

static uint8_t  xBuf[2048];
static uint32_t lastLedToggle = 0;
static bool     ledState      = false;
static uint32_t bytesTotal    = 0;

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // USB Serial → PC
    Serial.begin(PC_BAUD);

    // UART2 ← STM32
    Serial2.setRxBufferSize(RX_BUF_SZ);
    Serial2.setTxBufferSize(TX_BUF_SZ);
    Serial2.begin(STM32_BAUD, SERIAL_8N1, UART2_RX, UART2_TX);

    // Aguarda USB Serial estabilizar
    delay(100);
    Serial.println("# [ESP32] AccuEnergy bridge OK (bifasico 0x01-0x04) — aguardando STM32...");
}

void loop() {
    // Drena UART2 → USB Serial em blocos
    int avail = Serial2.available();
    if (avail > 0) {
        int chunk = (avail > (int)sizeof(xBuf)) ? (int)sizeof(xBuf) : avail;
        size_t n  = Serial2.readBytes(xBuf, chunk);
        if (n > 0) {
            Serial.write(xBuf, n);
            bytesTotal += n;

            // Toggle LED a cada LED_PERIOD ms enquanto há fluxo
            uint32_t now = millis();
            if (now - lastLedToggle >= LED_PERIOD) {
                lastLedToggle = now;
                ledState      = !ledState;
                digitalWrite(LED_PIN, ledState ? HIGH : LOW);
            }
        }
    }

    // Passa comandos do PC → STM32 (futuro uso: calibração remota, etc.)
    if (Serial.available()) {
        int n = Serial.available();
        if (n > (int)sizeof(xBuf)) n = (int)sizeof(xBuf);
        Serial.readBytes(xBuf, n);
        Serial2.write(xBuf, n);
    }
}
