#include "envio.h"

static inline void wf(float f)   { Serial.write((const uint8_t *)&f, 4); }
static inline void wb(uint8_t v) { Serial.write(&v, 1); }
static inline void wi(int16_t v) { Serial.write((const uint8_t *)&v, 2); }

static void writeHeader(uint8_t type) {
    wb(FRAME_MAGIC_0); wb(FRAME_MAGIC_1); wb(type);
}
static void writeFooter() {
    wb(FRAME_FOOTER_0); wb(FRAME_FOOTER_1);
}

/* ==========================================================================
 *  enviarDados — Frame DATA binário
 *
 *  Layout: magic(2) + type(1) + 6×float(24) + n(1) + step(1)
 *          + n × (int16 V, int16 I)(n×4) + footer(2)
 *  Tamanho: 31 + n×4 bytes.  Para n=120: 511 bytes.
 *
 *  vs protocolo texto anterior: ~1800 bytes → redução de ~3,5×.
 *  A 230400 baud e intervalo de 100 ms: 511/0,1 = 5110 bytes/s ≈ 22 % da banda.
 * ========================================================================== */
void enviarDados(float V_RMS, float I_RMS, float pf,
                 float p_active, float p_reactive, float p_apparent,
                 float *V_Raw, float *I_Raw, int numSamples) {

    int step = (numSamples > MAX_SEND_SAMPLES) ? numSamples / MAX_SEND_SAMPLES : 1;
    uint8_t sendCount = (uint8_t)(numSamples / step);

    writeHeader(FRAME_TYPE_DATA);
    wf(V_RMS); wf(I_RMS); wf(pf); wf(p_active); wf(p_reactive); wf(p_apparent);
    wb(sendCount);
    wb((uint8_t)step);
    for (int i = 0; i < numSamples; i += step) {
        wi((int16_t)V_Raw[i]);
        wi((int16_t)I_Raw[i]);
    }
    writeFooter();
}

/* ==========================================================================
 *  enviarCalPot — Frame CALPOT (29 bytes)
 *
 *  Enviado no modo CAL_POT a cada 200 ms.
 *  Todos os valores são em mV brutos (saída de esp_adc_cal).
 *  v_dc / i_dc : offset DC (esperado ~1650 mV = VCC/2)
 *  v_pp / i_pp : pico-a-pico (= amplitude do sinal AC × 2)
 *  v_ac_rms / i_ac_rms : RMS da componente AC
 * ========================================================================== */
void enviarCalPot(float v_dc, float v_pp, float v_ac_rms,
                  float i_dc, float i_pp, float i_ac_rms) {
    writeHeader(FRAME_TYPE_CALPOT);
    wf(v_dc); wf(v_pp); wf(v_ac_rms);
    wf(i_dc); wf(i_pp); wf(i_ac_rms);
    writeFooter();
}

/* ==========================================================================
 *  enviarCalPin — Frame CALPIN (21 bytes)
 *
 *  Enviado no modo CAL_PIN a cada 200 ms.
 *  Com ambos canais no mesmo sinal, ratio = v_ac_rms / i_ac_rms deve ser 1,0.
 *  Desvio indica diferença de ganho entre os canais ADC.
 * ========================================================================== */
void enviarCalPin(float v_dc, float i_dc, float v_ac_rms, float i_ac_rms) {
    writeHeader(FRAME_TYPE_CALPIN);
    wf(v_dc); wf(i_dc); wf(v_ac_rms); wf(i_ac_rms);
    writeFooter();
}
