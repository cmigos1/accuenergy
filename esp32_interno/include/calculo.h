#if !defined(CALCULO_H)
#define CALCULO_H
#include <Arduino.h>
#include <math.h>
#include <stdint.h>

/* ==========================================================================
 *  Constantes dos Sensores — ESP32 ADC Interno
 *
 *  IMPORTANTE: O firmware usa esp_adc_cal_raw_to_voltage() que retorna
 *  MILIVOLTS (mV) já linearizados, não counts brutos.
 *
 *  Portanto os fatores de escala partem de mV, não de counts:
 *    V_sensor = (mV - dc_offset_mV) / 1000.0   → Volts no pino ADC
 *    V_rede   = V_sensor × ZMPT101B_FACTOR      → Volts na rede
 *    I_pri    = V_sensor / R_BURDEN × CT_RATIO   → Amperes primário
 *
 *  Circuito:
 *    Bias = VCC/2 ≈ 1650 mV (offset DC, removido pela média do buffer)
 *
 *  ESP32 ADC (com ADC_ATTEN_DB_11):
 *    Faixa linear efetiva: ~150 mV a ~2450 mV
 *    Os valores retornados por esp_adc_cal são mV calibrados.
 * ========================================================================== */

/* --------------------------------------------------------------------------
 *  SCT-013 100A:50mA com burden de 22 Ω
 * -------------------------------------------------------------------------- */
#define SCT013_CT_RATIO    (100.0f / 0.050f)         /* 2000:1             */
#define SCT013_BURDEN_OHM  22.0f                      /* burden [Ω]        */
#define SCT013_FACTOR      (SCT013_CT_RATIO / SCT013_BURDEN_OHM) /* 90.909 */

/* --------------------------------------------------------------------------
 *  ZMPT101B — fator empírico (calibrar com multímetro)
 * -------------------------------------------------------------------------- */
#define ZMPT101B_FACTOR  204.1f

/* --------------------------------------------------------------------------
 *  Escala para mV → unidades físicas
 *
 *  Como esp_adc_cal_raw_to_voltage() retorna mV:
 *    ADS_VREF = 1000.0 (para converter mV → V: dividir por 1000)
 *    ADS_RES  = 1.0    (os valores já estão em mV, sem escala de counts)
 *
 *  Assim:  lsb = ADS_VREF / ADS_RES = 1000.0 / 1.0 = 1000
 *  E:      (mV - dc) * (1/1000) * FACTOR  → Volts ou Amperes na rede
 *
 *  Mas para simplificar, usamos a mesma fórmula existente:
 *    lsb = 1.0 / 1000.0 = 0.001  (converte mV → V diretamente)
 * -------------------------------------------------------------------------- */
#define ADC_DC_OFFSET  1650.0f   /* Nominal para VCC/2 em mV — não usado    */
#define ADS_VREF       1.0f      /* Para que lsb = 1/1000 = 0.001 V/mV    */
#define ADS_RES        1000.0f   /* Divisor: mV → V                        */

/* --------------------------------------------------------------------------
 *  Noise Gate — Piso de ruído do ADC interno do ESP32
 *
 *  O ADC do ESP32 tem ~5 mV RMS de ruído intrínseco. Multiplicado pelos
 *  fatores dos sensores, isso aparece como:
 *    V_ruido = 5 mV / 1000 × 204.1 ≈ 1.0 V  (medido: 0.85 V)
 *    I_ruido = 5 mV / 1000 × 90.9  ≈ 0.45 A (medido: 0.16 A)
 *
 *  Valores abaixo destes limiares são zerados para evitar leituras
 *  fantasma quando nenhum sensor está conectado ou sem carga.
 * -------------------------------------------------------------------------- */
#define V_NOISE_FLOOR  2.0f     /* Zerar V_RMS abaixo deste valor [V]     */
#define I_NOISE_FLOOR  0.3f     /* Zerar I_RMS abaixo deste valor [A]     */

/* --------------------------------------------------------------------------
 *  Protótipo
 * -------------------------------------------------------------------------- */
void calculoRMS(const float *V_Raw, const float *I_Raw, int numSamples,
                float *V_RMS, float *I_RMS, float *pf_out,
                float *p_active_out, float *p_reactive_out, float *p_apparent_out);

#endif /* CALCULO_H */
