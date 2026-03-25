/**
 * @file    nilm_adc.c
 * @brief   ADC1 dual-channel (voltage + current) with DMA and TIM2 trigger
 *
 * Channels:
 *   - IN0 (PA0) = ZMPT101B voltage sensor
 *   - IN1 (PA1) = SCT-013 current sensor (via burden + bias circuit)
 *
 * The ADC runs in scan mode, converting both channels sequentially on each
 * TIM2 TRGO event (10 kHz). DMA stores results in a circular buffer of
 * interleaved [V, I, V, I, ...] pairs.
 *
 * A double-buffering scheme is used: when the first half is filled, a flag
 * is set for processing while DMA continues filling the second half.
 */

#include "nilm_adc.h"
#include <string.h>

/* ---------- Peripheral handles ---------- */
static ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
static TIM_HandleTypeDef htim2;

/* ---------- DMA buffer in D2 SRAM for DMA access ---------- */
/* STM32H7 note: DMA buffers must be in a memory region accessible by DMA.
 * AXI SRAM (default) is accessible. If using DTCM, DMA cannot reach it. */
static volatile uint16_t adc_dma_buffer[NILM_DMA_BUF_SIZE]
    __attribute__((aligned(32)));

/* ---------- Flags ---------- */
static volatile int window_ready_flag = 0;

/* ================================================================== */
/*                      TIM2 Configuration                            */
/* ================================================================== */
static void MX_TIM2_Init(void) {
  __HAL_RCC_TIM2_CLK_ENABLE();

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  /* For 10 kHz trigger at a TIM2 clock of e.g. 240 MHz (APB1 timer clock):
   * Period = (TIM_CLK / desired_rate) - 1
   * We compute based on the actual APB1 timer clock at runtime. */
  {
    uint32_t tim_clk = HAL_RCC_GetPCLK1Freq();
    /* APB1 timer clock is 2x PCLK1 if APB1 prescaler != 1 */
    RCC_ClkInitTypeDef clk_cfg;
    uint32_t flash_lat;
    HAL_RCC_GetClockConfig(&clk_cfg, &flash_lat);
    if (clk_cfg.APB1CLKDivider != RCC_APB1_DIV1) {
      tim_clk *= 2;
    }
    htim2.Init.Period = (tim_clk / NILM_ADC_SAMPLE_RATE_HZ) - 1;
  }
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  HAL_TIM_Base_Init(&htim2);

  /* Configure TRGO = Update event → triggers ADC */
  TIM_MasterConfigTypeDef master_cfg = {0};
  master_cfg.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master_cfg.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  HAL_TIMEx_MasterConfigSynchronization(&htim2, &master_cfg);
}

/* ================================================================== */
/*                      DMA Configuration                             */
/* ================================================================== */
static void MX_DMA_Init(void) {
  __HAL_RCC_DMA1_CLK_ENABLE();

  hdma_adc1.Instance = DMA1_Stream0;
  hdma_adc1.Init.Request = DMA_REQUEST_ADC1;
  hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
  hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
  hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  hdma_adc1.Init.Mode = DMA_CIRCULAR;
  hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;
  hdma_adc1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  HAL_DMA_Init(&hdma_adc1);

  __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

  /* DMA interrupt for half and complete transfer */
  HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
}

/* ================================================================== */
/*                      ADC1 Configuration                            */
/* ================================================================== */
static void MX_ADC1_Init(void) {
  __HAL_RCC_ADC12_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* Configure PA0 and PA1 as analog */
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* ADC common clock: Use PLL2P as ADC clock source for better control */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SEQ_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE; /* Timer-triggered */
  hadc1.Init.NbrOfConversion = 2;          /* 2 channels      */
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T2_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.OversamplingMode = DISABLE;

  HAL_ADC_Init(&hadc1);

  /* Calibrate ADC */
  HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED);

  /* Channel 0 (PA0) = Voltage (ZMPT101B) — Rank 1 */
  ADC_ChannelConfTypeDef ch_cfg = {0};
  ch_cfg.Channel = ADC_CHANNEL_16;
  ch_cfg.Rank = ADC_REGULAR_RANK_1;
  ch_cfg.SamplingTime = ADC_SAMPLETIME_32CYCLES_5; /* Adequate for 10kHz */
  ch_cfg.SingleDiff = ADC_SINGLE_ENDED;
  ch_cfg.OffsetNumber = ADC_OFFSET_NONE;
  HAL_ADC_ConfigChannel(&hadc1, &ch_cfg);

  /* Channel 1 (PA1) = Current (SCT-013) — Rank 2 */
  ch_cfg.Channel = ADC_CHANNEL_17;
  ch_cfg.Rank = ADC_REGULAR_RANK_2;
  HAL_ADC_ConfigChannel(&hadc1, &ch_cfg);
}

/* ================================================================== */
/*                      Public API                                   */
/* ================================================================== */

void NILM_ADC_Init(void) {
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_TIM2_Init();
}

void NILM_ADC_Start(void) {
  /* Start ADC with DMA in circular mode */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_buffer, NILM_DMA_BUF_SIZE);
  /* Start timer to trigger conversions */
  HAL_TIM_Base_Start(&htim2);
}

volatile uint16_t *NILM_ADC_GetBuffer(void) { return adc_dma_buffer; }

int NILM_ADC_IsWindowReady(void) {
  if (window_ready_flag) {
    window_ready_flag = 0;
    return 1;
  }
  return 0;
}

/* ================================================================== */
/*                   DMA / ADC Callbacks                              */
/* ================================================================== */

/**
 * DMA Transfer Complete callback — full buffer filled.
 * In circular mode this means we have a complete window of samples.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
  if (hadc->Instance == ADC1) {
    /* Invalidate D-Cache for DMA buffer region (H7 cache coherency) */
    SCB_InvalidateDCache_by_Addr(
        (uint32_t *)&adc_dma_buffer[NILM_DMA_BUF_SIZE / 2], NILM_DMA_BUF_SIZE);
    window_ready_flag = 1;
  }
}

/* ================================================================== */
/*                      IRQ Handlers                                 */
/* ================================================================== */

void ADC_IRQHandler(void) { HAL_ADC_IRQHandler(&hadc1); }
