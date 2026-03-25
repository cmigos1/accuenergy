/**
 * @file    spi_slave.h
 * @brief   SPI Slave task — receives nilm_data_t packets from STM32
 *          Runs on Core 0 (dedicated)
 */

#ifndef SPI_SLAVE_H
#define SPI_SLAVE_H

#include "nilm_data.h"

/**
 * @brief  Initialize SPI slave hardware and start the receive task on Core 0
 */
void spi_slave_init();

/**
 * @brief  Get the latest valid data packet (thread-safe copy)
 * @param  out  Pointer to nilm_data_t to fill with latest data
 * @return true if valid data available, false if no data received yet
 */
bool spi_slave_get_data(nilm_data_t *out);

/**
 * @brief  Check if new data has arrived since last call to spi_slave_get_data
 */
bool spi_slave_has_new_data();

#endif /* SPI_SLAVE_H */
