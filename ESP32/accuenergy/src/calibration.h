/**
 * @file    calibration.h
 * @brief   Software calibration system — gain & offset for voltage and current
 *          Values stored in NVS (ESP32 Preferences) for persistence across reboots
 */

#ifndef CALIBRATION_H
#define CALIBRATION_H

#include "nilm_data.h"

/**
 * @brief  Calibration parameters
 *
 * Applied as: calibrated_value = (raw_value * gain) + offset
 *
 * Workflow with Fluke 179:
 *   1. Read the "raw" values from the system
 *   2. Measure the real values with the Fluke 179
 *   3. Compute: gain = real_value / raw_value
 *   4. Compute: offset = real_value - (raw_value * gain)
 *   5. Enter via web interface and save
 */
typedef struct {
    float voltage_gain;     /* Default: 1.0  */
    float voltage_offset;   /* Default: 0.0  */
    float current_gain;     /* Default: 1.0  */
    float current_offset;   /* Default: 0.0  */
    float power_gain;       /* Default: 1.0  (applied to both P and S) */
    float power_offset;     /* Default: 0.0  */
} calibration_t;

/**
 * @brief  Initialize calibration — loads from NVS or uses defaults
 */
void calibration_init();

/**
 * @brief  Get current calibration parameters
 */
const calibration_t* calibration_get();

/**
 * @brief  Update calibration parameters and save to NVS
 */
void calibration_set(const calibration_t *cal);

/**
 * @brief  Reset calibration to factory defaults (gain=1, offset=0)
 */
void calibration_reset();

/**
 * @brief  Apply calibration to a nilm_data_t packet (in-place)
 */
void calibration_apply(nilm_data_t *data);

#endif /* CALIBRATION_H */
