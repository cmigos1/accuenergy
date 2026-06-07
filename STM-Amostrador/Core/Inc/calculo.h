#ifndef CALCULO_H
#define CALCULO_H

#include <math.h>
#include <stdint.h>

/* SCT013-100 (100A:50mA) — burden 50 Ω (era 11 Ω) para maior sensibilidade em baixas correntes.
 *   Zener 1N4728A (3.3V) protege excursão positiva: sem ele V_ADC_max = 1.65 + 50mA×50Ω = 4.15V > VDDA.
 *   Range antes do clipping: (3.3V − 1.65V)/50Ω = 33mA sec → 66A pico → ~46.7A RMS no primário.
 *   ATENÇÃO: excursão negativa chega a 1.65 − 2.5 = −0.85V (Zener não protege esse lado);
 *   clampeado pelos diodos internos do STM32, mas considere Schottky GND no pino PC4. */
#define SCT013_CT_RATIO    (100.0f / 0.050f)           /* 2000:1 */
#define SCT013_BURDEN_OHM  50.0f
#define SCT013_FACTOR      (SCT013_CT_RATIO / SCT013_BURDEN_OHM)
#define SCT013_SIGN        (-1.0f)  /* -1.0f = clamp invertido no fio; +1.0f = correto */

/* ZMPT101B standalone (transformador puro, sem módulo op-amp)
 *   Circuito: R1=136kΩ (dois 68kΩ série) no primário, R2=1kΩ burden no secundário
 *   Trafo 1:1 (2mA:2mA) → I_s = V_mains/R1 → V_R2 = V_mains × R2/R1
 *   Fator = R1/R2 = 136 (era 337.876 para o módulo comercial com op-amp)
 *
 *   Verificação @ 127Vrms nominal:
 *     I_pico  = (127√2) / 136000 = 1.321 mA  < 2 mA (linear)
 *     V_R2_pk = 1.321 mA × 1000  = 1.321 V
 *     V_ADC   = 1.65V ± 1.321V   → [0.33V, 2.97V] (~80% do range 0–3.3V)
 *   @ 140Vrms (sobretensão máxima ANEEL):
 *     I_pico  = (140√2) / 136000 = 1.454 mA  < 2 mA (sem saturação)
 *     V_ADC   = 1.65V ± 1.455V   → [0.19V, 3.11V] (sem clipping) */
#define ZMPT101B_R1        136000.0f              /* primário: 2 × 68kΩ série [Ω] */
#define ZMPT101B_R2        1000.0f                /* burden/amostragem secundário [Ω] */
#define ZMPT101B_FACTOR    (ZMPT101B_R1 / ZMPT101B_R2)   /* = 136.0 */

/* ADC STM32H743: 16-bit, VDDA = 3.3 V (ajustar ADC_VREF_V se usar referência externa) */
#define ADC_VREF_V         3.3f
#define ADC_MAX_CNT        65535.0f
#define ADC_LSB_V          (ADC_VREF_V / ADC_MAX_CNT)

/* Fatores de calibração: contagens_rms_ADC → unidade física
 *   CAL_V = (VDDA / ADC_MAX) × (R1/R2)       →  contagens → Vrms [V rede]
 *           = 50.35 µV/count × 136 = 6.848 mV/count
 *   CAL_I = (VDDA / ADC_MAX) × (CT_ratio/Rb) →  contagens → Irms [A]
 *
 *   Verificação CAL_V @ 127Vrms:
 *     V_R2_rms = 127/136 = 0.934 V → 0.934/3.3 × 65535 = 18558 counts rms
 *     Vrms = 18558 × 6.848e-3 = 127.1 V ✓ */
#define CAL_V              (ADC_LSB_V * ZMPT101B_FACTOR)
#define CAL_I              (ADC_LSB_V * SCT013_FACTOR)

/* Calibração dinâmica do piso de ruído (substitui #defines fixos) */
#define NOISE_CAL_FRAMES    5u     /* frames adquiridos na calibração (×300ms = 1.5s) */
#define NOISE_CAL_MARGIN    2.2f   /* multiplicador sobre o RMS medido (segurança) */
#define V_NOISE_FLOOR_MIN   1.0f   /* piso mínimo absoluto de tensão [V] — novo circuito
                                    /* tem ganho menor (fator 136 vs 338); ruído elétrico
                                    * equivalente sobe proporcionalmente em Vrms */
#define I_NOISE_FLOOR_MIN   0.03f  /* piso mínimo absoluto de corrente [A] */

/* Amostragem — Timer2 + DMA @ 15360 Hz */
#define FS_HZ             15360u           /* taxa de amostragem [Hz] */
#define F0_HZ             60u              /* fundamental da rede [Hz] */
#define F0_BIN            4u               /* bin Goertzel: N×F0/Fs = 1024×60/15360 = 4 */
#define HARM_MAX          50u              /* harmônicos calculados (até 50ª) */
#define HARM_FRAME_DIV    10u              /* envia frame 0x02 a cada N frames 0x01 */

#endif /* CALCULO_H */
