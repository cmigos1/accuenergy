/**
 * @file    nilm_spi.c
 * @brief   SPI1 Master driver — transmits nilm_data_t packets to ESP32
 *
 * Pins: PA5=SCK, PA6=MISO, PA7=MOSI, PA4=NSS
 * SPI clock ~1 MHz (safe for short wires between boards)
 */

#include "nilm_spi.h"

static SPI_HandleTypeDef hspi1;

void NILM_SPI_Init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* GPIO: PA5(SCK), PA6(MISO), PA7(MOSI) — Alternate Function */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_MEDIUM;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PA4 = NSS (hardware managed) */
    gpio.Pin       = GPIO_PIN_4;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_MEDIUM;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* SPI1 configuration */
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_HARD_OUTPUT;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.NSSPMode          = SPI_NSS_PULSE_ENABLE;
    hspi1.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;

    /* Baudrate prescaler: SPI1 is on APB2.
     * APB2 = 120 MHz (with 480MHz sysclk, /2 AHB, /2 APB2)
     * Prescaler 128 → 120/128 ≈ 937.5 kHz ≈ 1 MHz */
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_128;

    HAL_SPI_Init(&hspi1);
}

HAL_StatusTypeDef NILM_SPI_Transmit(const nilm_data_t *pkt)
{
    return HAL_SPI_Transmit(&hspi1,
                            (uint8_t *)pkt,
                            sizeof(nilm_data_t),
                            100);  /* 100ms timeout */
}
