#include "calculo.h"


void calculoRMS(float *V_Raw, float *I_Raw, int numSamples, float *V_RMS, float *I_RMS) {
    /* Detecção de cruzamento por zero para frequência */
    int    zero_crossings = 0;
    float  prev_v         = 0.0f;
    int    first_sample   = 1;

    double sum_v2 = 0.0;
    double sum_i2 = 0.0;
    double sum_vi = 0.0;

    /* Fatores de conversão ADS1115 */
    const float vRef = ADS_VREF;
    const float adcRes= ADS_RES;
    const float v_scale = (vRef / adcRes) * ZMPT101B_FACTOR;
    const float i_scale = (vRef / adcRes) * SCT013_FACTOR;

    for (int k = 0; k < numSamples; k++) {
        float v_raw = V_Raw[k] - ADC_DC_OFFSET;
        float i_raw = I_Raw[k] - ADC_DC_OFFSET;

        /* Converter para unidades do mundo real */
        float v = v_raw * v_scale;
        float i = i_raw * i_scale;

        /* Acumular */
        sum_v2 += (double)v * (double)v;
        sum_i2 += (double)i * (double)i;
        sum_vi += (double)v * (double)i;

        /* Detecção de cruzamento por zero na tensão (borda de subida) */
        if (!first_sample) {
            if (prev_v < 0.0f && v >= 0.0f) {
                zero_crossings++;
            }
        }
        prev_v       = v;
        first_sample = 0;
    }

    /* Calcular valores RMS */
    float vrms = (float)sqrt(sum_v2 / (double)numSamples);
    float irms = (float)sqrt(sum_i2 / (double)numSamples);

    /* Cálculos de potência */
    float p_active   = (float)(sum_vi / (double)numSamples);   /* Potência ativa (W)   */
    float p_apparent = vrms * irms;                            /* Potência aparente (VA) */

    /* Fator de potência — evitar divisão por zero */
    float pf = 0.0f;
    if (p_apparent > 0.001f) {
        pf = p_active / p_apparent;
        /* Limitar FP a [-1, 1] */
        if (pf > 1.0f) pf = 1.0f;
        if (pf < -1.0f) pf = -1.0f;
    }

    /* Saída do RMS calculado */
    if (V_RMS) *V_RMS = vrms;
    if (I_RMS) *I_RMS = irms;

    /* Ignorar a estimativa de frequência por enquanto */
}