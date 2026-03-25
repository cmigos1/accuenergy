/**
 * @file    spi_slave.cpp
 * @brief   SPI Slave implementation using ESP-IDF spi_slave driver (Arduino compatible)
 *
 * Runs a FreeRTOS task pinned to Core 0 that continuously listens for
 * incoming SPI transactions from the STM32 master.
 *
 * Pins (VSPI):
 *   GPIO 18 = SCK
 *   GPIO 23 = MOSI (data from master)
 *   GPIO 19 = MISO (data to master — unused for now)
 *   GPIO  5 = SS   (chip select from master)
 */

#include "spi_slave.h"
#include <Arduino.h>
#include "driver/spi_slave.h"
#include <string.h>

/* ---- Configuration ---- */
#define SPI_SLAVE_HOST    VSPI_HOST
#define PIN_SCK           18
#define PIN_MOSI          23
#define PIN_MISO          19
#define PIN_SS             5

/* ---- Shared state (protected by mutex) ---- */
static nilm_data_t       s_latest_data;
static SemaphoreHandle_t s_data_mutex  = NULL;
static volatile bool     s_data_valid  = false;
static volatile bool     s_new_data    = false;

/* ---- DMA-capable receive buffer ---- */
/* Must be in DMA-capable memory and word-aligned */
WORD_ALIGNED_ATTR static uint8_t s_rx_buffer[64];  /* >= sizeof(nilm_data_t) = 32 */

/**
 * SPI Slave task — runs exclusively on Core 0
 * Continuously queues SPI slave transactions and waits for master.
 */
static void spi_slave_task(void *pvParameters)
{
    spi_slave_transaction_t trans;
    memset(&trans, 0, sizeof(trans));

    while (true)
    {
        /* Prepare the receive buffer */
        memset(s_rx_buffer, 0, sizeof(s_rx_buffer));
        trans.length    = sizeof(nilm_data_t) * 8;  /* Length in bits */
        trans.tx_buffer = NULL;                       /* Nothing to send back (yet) */
        trans.rx_buffer = s_rx_buffer;

        /* Block until a transaction completes (master sends data) */
        esp_err_t ret = spi_slave_transmit(SPI_SLAVE_HOST, &trans, portMAX_DELAY);

        if (ret == ESP_OK && trans.trans_len >= (sizeof(nilm_data_t) * 8))
        {
            /* Validate received packet */
            const nilm_data_t *pkt = (const nilm_data_t *)s_rx_buffer;

            if (nilm_packet_validate(pkt))
            {
                /* Copy to shared state under mutex */
                if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
                {
                    memcpy(&s_latest_data, pkt, sizeof(nilm_data_t));
                    s_data_valid = true;
                    s_new_data   = true;
                    xSemaphoreGive(s_data_mutex);
                }
            }
            else
            {
                Serial.println("[SPI] CRC or header mismatch — packet dropped");
            }
        }
    }
}

void spi_slave_init()
{
    /* Create mutex */
    s_data_mutex = xSemaphoreCreateMutex();
    configASSERT(s_data_mutex);

    /* SPI bus configuration */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num   = PIN_MOSI;
    bus_cfg.miso_io_num   = PIN_MISO;
    bus_cfg.sclk_io_num   = PIN_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 64;

    /* SPI slave interface configuration — must match master settings */
    spi_slave_interface_config_t slave_cfg = {};
    slave_cfg.mode          = 0;            /* CPOL=0, CPHA=0 (same as STM32) */
    slave_cfg.spics_io_num  = PIN_SS;
    slave_cfg.queue_size    = 3;
    slave_cfg.flags         = 0;

    /* Initialize the SPI slave driver */
    esp_err_t ret = spi_slave_initialize(SPI_SLAVE_HOST, &bus_cfg, &slave_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        Serial.printf("[SPI] Init failed: %s\n", esp_err_to_name(ret));
        return;
    }

    Serial.println("[SPI] Slave initialized on VSPI (GPIO 5,18,19,23)");

    /* Create task pinned to Core 0 */
    xTaskCreatePinnedToCore(
        spi_slave_task,
        "spi_slave",
        4096,           /* Stack size (bytes) */
        NULL,
        5,              /* Priority (higher = more urgent) */
        NULL,
        0               /* Core 0 — dedicated to SPI */
    );
}

bool spi_slave_get_data(nilm_data_t *out)
{
    if (!s_data_valid) return false;

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        memcpy(out, &s_latest_data, sizeof(nilm_data_t));
        s_new_data = false;
        xSemaphoreGive(s_data_mutex);
        return true;
    }
    return false;
}

bool spi_slave_has_new_data()
{
    return s_new_data;
}
