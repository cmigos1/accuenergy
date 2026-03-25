/**
 * @file    nilm_adc.h
 * @brief   ADC + DMA configuration for dual-channel acquisition (voltage & current)
 */

#ifndef NILM_ADC_H
#define NILM_ADC_H

#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Configuration ---------- */
#define NILM_ADC_SAMPLE_RATE_HZ     10000   /* 10 kHz per channel                  */
#define NILM_ADC_RESOLUTION         4096    /* 12-bit ADC                          */
#define NILM_ADC_VREF               3.3f    /* Reference voltage                   */

/* Samples per mains cycle: 10000 / 60 ≈ 166 samples/cycle
 * We accumulate for ~10 cycles (100ms) ≈ 1666 samples per measurement window */
#define NILM_CYCLES_PER_WINDOW      10
#define NILM_SAMPLES_PER_CYCLE      166     /* approximate, for 60Hz               */
#define NILM_SAMPLES_PER_WINDOW     (NILM_CYCLES_PER_WINDOW * NILM_SAMPLES_PER_CYCLE)

/* DMA buffer: 2 channels interleaved [V0,I0, V1,I1, ...] */
#define NILM_DMA_BUF_SIZE           (NILM_SAMPLES_PER_WINDOW * 2)

/* ---------- API ---------- */

/**
 * @brief  Initialize ADC1 (2 channels), DMA, and TIM2 trigger
 */
void NILM_ADC_Init(void);

/**
 * @brief  Start continuous ADC conversion with DMA
 */
void NILM_ADC_Start(void);

/**
 * @brief  Get pointer to the raw DMA buffer (interleaved V,I pairs)
 */
volatile uint16_t* NILM_ADC_GetBuffer(void);

/**
 * @brief  Check if a full window of data is ready
 * @return 1 if ready, 0 otherwise. Automatically clears when read.
 */
int NILM_ADC_IsWindowReady(void);

#ifdef __cplusplus
}
#endif

#endif /* NILM_ADC_H */
