/**
 * @file    nilm_spi.h
 * @brief   SPI Master driver for transmitting nilm_data_t to ESP32
 */

#ifndef NILM_SPI_H
#define NILM_SPI_H

#include "stm32h7xx_hal.h"
#include "nilm_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialize SPI1 as Master
 *         PA5=SCK, PA6=MISO, PA7=MOSI, PA4=NSS (hardware managed)
 */
void NILM_SPI_Init(void);

/**
 * @brief  Transmit a nilm_data_t packet to the ESP32 slave
 * @param  pkt  Pointer to the finalized packet (header & CRC must be set)
 * @return HAL_OK on success
 */
HAL_StatusTypeDef NILM_SPI_Transmit(const nilm_data_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* NILM_SPI_H */
