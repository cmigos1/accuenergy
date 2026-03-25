/**
 * @file    main.cpp
 * @brief   NILM ESP32 Gateway — Main application
 *
 * Architecture (FreeRTOS dual-core):
 *   Core 0: SPI Slave task (receives data from STM32)
 *   Core 1: Arduino loop + WiFi + WebServer + WebSocket
 *
 * Data flow:
 *   STM32 → SPI → spi_slave (Core 0) → calibration_apply → WebSocket → Browser
 */

#include <Arduino.h>
#include "nilm_data.h"
#include "spi_slave.h"
#include "web_server.h"
#include "calibration.h"

/* ---- Configuration ---- */
#define BROADCAST_INTERVAL_MS   500   /* WebSocket update rate */
#define SERIAL_LOG_INTERVAL_MS  2000  /* Serial debug log rate */

/* ---- Timing ---- */
static unsigned long last_broadcast = 0;
static unsigned long last_serial_log = 0;

/* ================================================================== */
/*                         Setup (Core 1)                            */
/* ================================================================== */
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("=================================");
    Serial.println("  NILM Energy Monitor — ESP32");
    Serial.println("  Projeto de Extensão IFMT");
    Serial.println("=================================");
    Serial.println();

    /* Initialize calibration (loads from NVS) */
    calibration_init();

    /* Initialize SPI Slave (creates task on Core 0) */
    spi_slave_init();

    /* Initialize WiFi AP + Web Server (runs on Core 1) */
    web_server_init();

    Serial.println();
    Serial.println("[MAIN] System ready. Waiting for STM32 data...");
    Serial.println("[MAIN] Connect to WiFi 'NILM-Monitor' (password: nilm2026)");
    Serial.println("[MAIN] Then open http://192.168.4.1 in your browser");
    Serial.println();
}

/* ================================================================== */
/*                         Loop (Core 1)                             */
/* ================================================================== */
void loop()
{
    unsigned long now = millis();
    nilm_data_t data;

    /* ---- Broadcast to WebSocket clients ---- */
    if ((now - last_broadcast) >= BROADCAST_INTERVAL_MS)
    {
        last_broadcast = now;

        if (spi_slave_get_data(&data))
        {
            /* Apply software calibration (gain + offset) */
            calibration_apply(&data);

            /* Push to all connected WebSocket clients */
            web_server_broadcast(&data);
        }
    }

    /* ---- Serial debug logging ---- */
    if ((now - last_serial_log) >= SERIAL_LOG_INTERVAL_MS)
    {
        last_serial_log = now;

        if (spi_slave_get_data(&data))
        {
            calibration_apply(&data);
            Serial.printf("[DATA] Vrms=%.1fV  Irms=%.3fA  P=%.1fW  S=%.1fVA  "
                          "PF=%.3f  f=%.1fHz  pkt#%u\n",
                          data.vrms, data.irms,
                          data.power_active, data.power_apparent,
                          data.power_factor, data.frequency,
                          data.sample_count);
        }
        else
        {
            Serial.println("[DATA] Waiting for STM32 data...");
        }
    }

    /* Small delay to avoid hogging Core 1 */
    delay(10);
}