#if !defined(CALCULO_H)
#define CALCULO_H
#include <Arduino.h>
#include <math.h>
#include <stdint.h>


/* ---------- Constantes dos Sensores ---------- */

/*
 * SCT-013 100A:50mA com resistor burden de 22 Ohm
 *
 * Cadeia de conversão:
 *   ADC bruto -> Tensão no pino ADC -> Corrente no burden -> Corrente primária
 *
 *   V_adc = (bruto - OFFSET_DC) * (VREF / RESOLUCAO_ADC)
 *   I_secundario = V_adc / R_BURDEN          [A no secundário]
 *   I_primario   = I_secundario * CT_RATIO    [A no primário]
 *
 * Fator combinado:
 *   I_primario = (bruto - offset) * (3.3 / 32768) / 22.0 * (100.0 / 0.050)
 *   I_primario = (bruto - offset) * (3.3 / 32768) * (100.0 / (0.050 * 22.0))
 *   I_primario = (bruto - offset) * (3.3 / 32768) * 90.909...
 *   I_primario = (bruto - offset) * 0.0091...
 */
#define SCT013_CT_RATIO (100.0f / 0.050f) /* Relação de espiras 2000:1 */
#define SCT013_BURDEN_OHM 22.0f           /* Resistor burden        */
#define SCT013_FACTOR (SCT013_CT_RATIO / SCT013_BURDEN_OHM) /* 90.909 */

/*
 * Sensor de tensão ZMPT101B
 * O ganho de saída depende do trimpot na placa. Típico: ~razão entrada/saída.
 * Fator de calibração padrão — DEVE ser ajustado por software/calibração
 * prática. Este fator converte a tensão do ADC para a tensão da rede.
 */
#define ZMPT101B_FACTOR 165.5f

/* Offset DC para ADS1115 com GAIN_ONE (1.65V / 4.096V) * 32768 */
#define ADC_DC_OFFSET 13200.0f
#define ADS_VREF 4.096f
#define ADS_RES 32768.0f

void calculoRMS(float *V_Raw, float *I_Raw, int numSamples, float *V_RMS,
                float *I_RMS);
#endif