#include "calculo.h"

void calculoRMS(const float *V_Raw, const float *I_Raw, int numSamples,
                float *V_RMS, float *I_RMS, float *pf_out,
                float *p_active_out, float *p_reactive_out, float *p_apparent_out) {

    if (numSamples <= 0) return;

    /* ── Passo 1: Calcular média (= offset DC real do hardware) ────────── */
    double sum_v = 0.0, sum_i = 0.0;
    for (int k = 0; k < numSamples; k++) {
        sum_v += V_Raw[k];
        sum_i += I_Raw[k];
    }
    const float dc_v = (float)(sum_v / numSamples);
    const float dc_i = (float)(sum_i / numSamples);

    /* ── Passo 2: Acumular potências usando offset real ─────────────────── */
    const float lsb     = ADS_VREF / ADS_RES;       
    const float v_scale = lsb * ZMPT101B_FACTOR;    
    const float i_scale = lsb * SCT013_FACTOR;      

    double sum_v2 = 0.0;
    double sum_i2 = 0.0;
    double sum_vi = 0.0;

    for (int k = 0; k < numSamples; k++) {
        const float v = (V_Raw[k] - dc_v) * v_scale;
        const float i = (I_Raw[k] - dc_i) * i_scale;

        sum_v2 += (double)v * (double)v;
        sum_i2 += (double)i * (double)i;
        sum_vi += (double)v * (double)i;
    }

    /* ── Passo 3: Grandezas RMS e de potência ───────────────────────────── */
    float vrms = (float)sqrt(sum_v2 / (double)numSamples);
    float irms = (float)sqrt(sum_i2 / (double)numSamples);

    /* ── Passo 3.5: Noise Gate ──────────────────────────────────────────
     * O ADC do ESP32 tem ~5 mV RMS de ruído intrínseco. Multiplicado
     * pelos fatores dos sensores, gera leituras fantasma.
     * Se o RMS está abaixo do piso de ruído, zeramos tudo. */
    if (vrms < V_NOISE_FLOOR) vrms = 0.0f;
    if (irms < I_NOISE_FLOOR) irms = 0.0f;

    float p_active   = 0.0f;
    float p_apparent = 0.0f;
    float p_reactive = 0.0f;
    float pf         = 0.0f;

    /* Só calcula potências se ambos V e I estiverem acima do ruído */
    if (vrms > 0.0f && irms > 0.0f) {
        p_active  = (float)(sum_vi / (double)numSamples);
        p_apparent = vrms * irms;

        const float s2 = p_apparent * p_apparent;
        const float p2 = p_active   * p_active;
        if (s2 >= p2) {
            p_reactive = (float)sqrt((double)(s2 - p2));
        }
        if (p_active < 0.0f) p_reactive = -p_reactive;

        if (p_apparent > 0.001f) {
            pf = p_active / p_apparent;
            if (pf >  1.0f) pf =  1.0f;
            if (pf < -1.0f) pf = -1.0f;
        }
    }

    if (V_RMS)           *V_RMS           = vrms;
    if (I_RMS)           *I_RMS           = irms;
    if (pf_out)          *pf_out          = pf;
    if (p_active_out)    *p_active_out    = p_active;
    if (p_reactive_out)  *p_reactive_out  = p_reactive;
    if (p_apparent_out)  *p_apparent_out  = p_apparent;
}
