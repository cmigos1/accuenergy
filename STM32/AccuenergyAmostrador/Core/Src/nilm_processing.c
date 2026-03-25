/**
 * @file    nilm_processing.c
 * @brief   Signal processing: compute Vrms, Irms, P, S, PF, frequency
 *
 * Algorithm overview:
 *   1. Iterate over N pairs of interleaved [V_raw, I_raw] samples
 *   2. Subtract DC offset (ADC midpoint = 2048 for 12-bit @ 1.65V bias)
 *   3. Convert to real-world units using sensor calibration factors
 *   4. Accumulate sum-of-squares for RMS, and sum-of-products for P
 *   5. Detect zero-crossings on voltage channel to estimate frequency
 */

#include "nilm_processing.h"
#include "nilm_adc.h"
#include <math.h>

/* Running cycle counter */
static uint32_t g_cycle_count = 0;

void NILM_Processing_Compute(const volatile uint16_t *adc_buf,
                              uint32_t n_samples,
                              nilm_data_t *out)
{
    double sum_v2  = 0.0;   /* Σ(v²) */
    double sum_i2  = 0.0;   /* Σ(i²) */
    double sum_vi  = 0.0;   /* Σ(v×i) */

    /* Zero-crossing detection for frequency */
    int    zero_crossings = 0;
    float  prev_v         = 0.0f;
    int    first_sample   = 1;

    /* Conversion factors */
    const float v_scale = (NILM_ADC_VREF / (float)NILM_ADC_RESOLUTION) * ZMPT101B_FACTOR;
    const float i_scale = (NILM_ADC_VREF / (float)NILM_ADC_RESOLUTION) * SCT013_FACTOR;

    for (uint32_t k = 0; k < n_samples; k++) {
        /* Interleaved buffer: index 2k = voltage, 2k+1 = current */
        int32_t v_raw = (int32_t)adc_buf[2 * k]     - ADC_DC_OFFSET;
        int32_t i_raw = (int32_t)adc_buf[2 * k + 1] - ADC_DC_OFFSET;

        /* Convert to real-world units */
        float v = (float)v_raw * v_scale;
        float i = (float)i_raw * i_scale;

        /* Accumulate */
        sum_v2 += (double)v * (double)v;
        sum_i2 += (double)i * (double)i;
        sum_vi += (double)v * (double)i;

        /* Zero-crossing detection on voltage (rising edge) */
        if (!first_sample) {
            if (prev_v < 0.0f && v >= 0.0f) {
                zero_crossings++;
            }
        }
        prev_v       = v;
        first_sample = 0;
    }

    /* Compute RMS values */
    float vrms = (float)sqrt(sum_v2 / (double)n_samples);
    float irms = (float)sqrt(sum_i2 / (double)n_samples);

    /* Power calculations */
    float p_active   = (float)(sum_vi / (double)n_samples);   /* Active power (W)   */
    float p_apparent = vrms * irms;                            /* Apparent power (VA) */

    /* Power factor — avoid division by zero */
    float pf = 0.0f;
    if (p_apparent > 0.001f) {
        pf = p_active / p_apparent;
        /* Clamp PF to [-1, 1] */
        if (pf > 1.0f) pf = 1.0f;
        if (pf < -1.0f) pf = -1.0f;
    }

    /* Frequency estimation from zero crossings
     * Each complete cycle has 1 rising zero-crossing
     * Time window = n_samples / SAMPLE_RATE */
    float freq = 0.0f;
    if (zero_crossings > 0) {
        float window_time = (float)n_samples / (float)NILM_ADC_SAMPLE_RATE_HZ;
        freq = (float)zero_crossings / window_time;
    }

    /* Fill output packet */
    out->vrms           = vrms;
    out->irms           = irms;
    out->power_active   = p_active;
    out->power_apparent = p_apparent;
    out->power_factor   = pf;
    out->frequency      = freq;
    out->sample_count   = ++g_cycle_count;

    /* Finalize packet (header + CRC) */
    nilm_packet_finalize(out);
}
