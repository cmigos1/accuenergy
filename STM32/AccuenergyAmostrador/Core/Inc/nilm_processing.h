/**
 * @file    nilm_processing.h
 * @brief   Signal processing: Vrms, Irms, P, S, PF, frequency
 */

#ifndef NILM_PROCESSING_H
#define NILM_PROCESSING_H

#include "nilm_data.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Sensor constants ---------- */

/*
 * SCT-013 100A:50mA with 22 Ohm burden resistor
 *
 * Conversion chain:
 *   ADC raw → Voltage at ADC pin → Current through burden → Primary current
 *
 *   V_adc = (raw - DC_OFFSET) * (VREF / ADC_RESOLUTION)
 *   I_secondary = V_adc / R_BURDEN          [A at secondary]
 *   I_primary   = I_secondary * CT_RATIO    [A at primary]
 *
 * Combined factor:
 *   I_primary = (raw - offset) * (3.3 / 4096) / 22.0 * (100.0 / 0.050)
 *   I_primary = (raw - offset) * (3.3 / 4096) * (100.0 / (0.050 * 22.0))
 *   I_primary = (raw - offset) * (3.3 / 4096) * 90.909...
 *   I_primary = (raw - offset) * 0.07326...
 */
#define SCT013_CT_RATIO         (100.0f / 0.050f)   /* 2000:1 turns ratio     */
#define SCT013_BURDEN_OHM       22.0f               /* Burden resistor        */
#define SCT013_FACTOR           (SCT013_CT_RATIO / SCT013_BURDEN_OHM)  /* 90.909 */

/*
 * ZMPT101B voltage sensor
 * The output gain depends on board trim-pot. Typical: ~input/output ratio.
 * Default calibration factor — MUST be adjusted in software/field calibration.
 * This factor converts ADC voltage to mains voltage.
 */
#define ZMPT101B_FACTOR         311.13f  /* Adjust based on calibration with Fluke 179 */

/* DC offset for 3.3V system biased at 1.65V center */
#define ADC_DC_OFFSET           2048     /* 12-bit midpoint (3.3V/2 = 1.65V)   */

/* ---------- API ---------- */

/**
 * @brief  Process a window of raw ADC samples and compute all power metrics
 * @param  adc_buf   Pointer to interleaved [V,I,V,I,...] raw ADC data
 * @param  n_samples Number of V,I PAIRS (not total array elements)
 * @param  out       Pointer to nilm_data_t packet to fill with results
 */
void NILM_Processing_Compute(const volatile uint16_t *adc_buf,
                              uint32_t n_samples,
                              nilm_data_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NILM_PROCESSING_H */
