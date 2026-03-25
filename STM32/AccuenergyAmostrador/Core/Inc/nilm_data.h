/**
 * @file    nilm_data.h
 * @brief   Shared data structure for SPI communication between STM32 and ESP32
 * @author  NILM Project - IFMT Extension
 *
 * This header defines the data packet format used for SPI transfer.
 * MUST be identical on both STM32 (master) and ESP32 (slave) sides.
 *
 * Packet structure (32 bytes, packed):
 *   [header:2][vrms:4][irms:4][P:4][S:4][PF:4][freq:4][count:4][crc:2]
 *
 * The header (0xAA55) allows the slave to detect frame start.
 * CRC-16 (CCITT) ensures data integrity over the SPI bus.
 */

#ifndef NILM_DATA_H
#define NILM_DATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Magic header for frame synchronization -- */
#define NILM_HEADER_MAGIC   0xAA55

/* -- SPI data packet -- */
#pragma pack(push, 1)
typedef struct {
    uint16_t header;          /* 0xAA55 magic number                       */
    float    vrms;            /* RMS voltage (V)                           */
    float    irms;            /* RMS current (A)                           */
    float    power_active;    /* Active power  P = 1/N * sum(v*i)  (W)    */
    float    power_apparent;  /* Apparent power S = Vrms * Irms    (VA)   */
    float    power_factor;    /* Power factor  PF = P / S          (0-1) */
    float    frequency;       /* Mains frequency (Hz)                     */
    uint32_t sample_count;    /* Processed cycle counter                  */
    uint16_t crc16;           /* CRC-16/CCITT for integrity check         */
} nilm_data_t;
#pragma pack(pop)

/* Compile-time check */
_Static_assert(sizeof(nilm_data_t) == 32, "nilm_data_t must be exactly 32 bytes");

/**
 * @brief  Compute CRC-16/CCITT over a data buffer
 * @param  data  Pointer to data
 * @param  len   Number of bytes
 * @return CRC-16 value
 */
static inline uint16_t nilm_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/**
 * @brief  Fill header and CRC fields of a nilm_data_t packet
 * @param  pkt  Pointer to the packet (all measurement fields must be filled)
 */
static inline void nilm_packet_finalize(nilm_data_t *pkt)
{
    pkt->header = NILM_HEADER_MAGIC;
    /* CRC covers everything except the last 2 bytes (crc16 field itself) */
    pkt->crc16 = nilm_crc16((const uint8_t *)pkt, sizeof(nilm_data_t) - sizeof(uint16_t));
}

/**
 * @brief  Validate a received nilm_data_t packet
 * @param  pkt  Pointer to the received packet
 * @return 1 if valid, 0 if header or CRC mismatch
 */
static inline int nilm_packet_validate(const nilm_data_t *pkt)
{
    if (pkt->header != NILM_HEADER_MAGIC)
        return 0;
    uint16_t crc = nilm_crc16((const uint8_t *)pkt, sizeof(nilm_data_t) - sizeof(uint16_t));
    return (crc == pkt->crc16) ? 1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* NILM_DATA_H */
