/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "st7735.h"
#include "calculo.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define NUM_SAMPLES       1024u

/* Protocolo IDF FRAME_DATA: downsampling das 1024 amostras para 128 pares */
#define IDF_N             128u    /* pares V+I enviados */
#define IDF_STEP          8u      /* 1 a cada 8 amostras — 1024/8 = 128 */

/* Média deslizante de Irms — atenua ruído de frame a frame */
#define IRMS_AVG_N        5u

/* CAL_V, CAL_I, FS_HZ, F0_BIN, HARM_MAX, HARM_FRAME_DIV definidos em calculo.h */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* Estado completo de uma fase de medição (sensor de tensão + sensor de corrente).
 * Duas instâncias independentes: fase 1 (ADC1, PA6/PC4) e fase 2 (ADC2, PA7/PC5).
 * Amostragem bifásica em paralelo — os dois ADCs convertem no mesmo trigger TIM2,
 * portanto o tempo de aquisição por frame não aumenta com a 2ª fase. */
typedef struct {
    uint32_t vRaw[NUM_SAMPLES];          /* contagens ADC pós-deinterleave */
    uint32_t iRaw[NUM_SAMPLES];
    float    vFloat[NUM_SAMPLES];        /* trabalho Goertzel (unidades físicas) */
    float    iFloat[NUM_SAMPLES];

    float    vrms, irms, preal, papar, preati, fp;   /* métricas calculadas */
    float    thd_v, thd_i;
    float    harmMagV[HARM_MAX + 1u];
    float    harmMagI[HARM_MAX + 1u];

    uint32_t adc_mean_v, adc_mean_i;     /* raw para calibração / debug */
    uint32_t adc_pp_v,   adc_pp_i;

    float    irms_buf[IRMS_AVG_N];       /* média deslizante de Irms (por fase) */
    uint32_t irms_idx;
} PhaseData;
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;
DMA_HandleTypeDef hdma_adc1;
DMA_HandleTypeDef hdma_adc2;
TIM_HandleTypeDef htim2;

UART_HandleTypeDef hlpuart1;

/* USER CODE BEGIN PV */
/* Buffers DMA em AXI SRAM (DMA2-acessível) — seção .dma_buffer no linker script.
 * Interleaved [V0 I0 V1 I1 ...]: adcDmaBuf=ADC1 (fase 1), adcDmaBuf2=ADC2 (fase 2). */
static volatile uint32_t adcDmaBuf [NUM_SAMPLES * 2u] __attribute__((section(".dma_buffer"), aligned(32)));
static volatile uint32_t adcDmaBuf2[NUM_SAMPLES * 2u] __attribute__((section(".dma_buffer"), aligned(32)));
static volatile uint8_t  samplesReady  = 0u;   /* ADC1 (fase 1) concluiu o frame */
static volatile uint8_t  samplesReady2 = 0u;   /* ADC2 (fase 2) concluiu o frame */

/* Estado das duas fases (deinterleave, métricas, harmônicos) */
static PhaseData ph1;   /* fase 1 — ADC1, PA6 (V) / PC4 (I) */
static PhaseData ph2;   /* fase 2 — ADC2, PA7 (V) / PC5 (I) */

static uint32_t frame_count = 0u;

/* Pisos de ruído dinâmicos — um par por fase (circuitos independentes) */
static float g_v_floor  = 2.0f;   /* fase 1 — tensão */
static float g_i_floor  = 1.2f;   /* fase 1 — corrente */
static float g_v_floor2 = 2.0f;   /* fase 2 — tensão */
static float g_i_floor2 = 1.2f;   /* fase 2 — corrente */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_TIM2_Init(void);
static void MX_LPUART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static void Sampler_Deinterleave(const volatile uint32_t *src, uint32_t *v, uint32_t *i);
static void Calibrate_Acquire(uint8_t phase);
static uint8_t Calibrate_Phase(uint8_t phase, float *out_vrms, float *out_irms);
static void Calibrate_NoiseFloor(void);
static void Sampler_ComputeAndSend(PhaseData *ph, uint8_t frameType);
static float Goertzel_Magnitude(float *x, uint32_t N, uint32_t bin);
static void Harmonics_Compute(PhaseData *ph);
static void Send_HarmonicsFrame(PhaseData *ph, uint8_t frameType);
static void Debug_SendStatus(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Timer2 — dispara ADC @ FS_HZ via TRGO (TIM2 clock = APB1×2 = 240 MHz) */
static void MX_TIM2_Init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = 0u;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = (240000000u / FS_HZ) - 1u;   /* = 15624 */
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) Error_Handler();
    TIM_MasterConfigTypeDef mc = {0};
    mc.MasterOutputTrigger = TIM_TRGO_UPDATE;
    mc.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &mc) != HAL_OK) Error_Handler();
}

/* DMA2 Stream0 — ADC1 → adcDmaBuf, modo NORMAL (para e reinicia por frame) */
static void MX_DMA_Init(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();
    hdma_adc1.Instance                 = DMA2_Stream0;
    hdma_adc1.Init.Request             = DMA_REQUEST_ADC1;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_adc1.Init.Mode                = DMA_NORMAL;
    hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adc1.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

    /* DMA2 Stream1 — ADC2 → adcDmaBuf2 (fase 2), mesma config do Stream0 */
    hdma_adc2.Instance                 = DMA2_Stream1;
    hdma_adc2.Init.Request             = DMA_REQUEST_ADC2;
    hdma_adc2.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc2.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc2.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc2.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_adc2.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_adc2.Init.Mode                = DMA_NORMAL;
    hdma_adc2.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adc2.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc2) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&hadc2, DMA_Handle, hdma_adc2);
    HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
}

/* Separa buffer DMA interleaved [V0 I0 V1 I1 ...] → v[] e i[] da fase */
static void Sampler_Deinterleave(const volatile uint32_t *src, uint32_t *v, uint32_t *i)
{
    for (uint32_t k = 0u; k < NUM_SAMPLES; k++) {
        v[k] = src[k * 2u];
        i[k] = src[k * 2u + 1u];
    }
}

/* Aquisição bloqueante de uma fase — para uso em Calibrate_NoiseFloor.
 * phase=1 usa ADC1/adcDmaBuf/ph1, phase=2 usa ADC2/adcDmaBuf2/ph2.
 * Para e reinicia somente o ADC da fase alvo; o outro permanece parado. */
static void Calibrate_Acquire(uint8_t phase)
{
    char _d[160]; int _n;

    ADC_HandleTypeDef        *hadc   = (phase == 1u) ? &hadc1  : &hadc2;
    volatile uint32_t        *buf    = (phase == 1u) ? adcDmaBuf : adcDmaBuf2;
    volatile uint8_t         *ready  = (phase == 1u) ? &samplesReady : &samplesReady2;
    PhaseData                *ph     = (phase == 1u) ? &ph1 : &ph2;

    *ready = 0u;

    _n = snprintf(_d, sizeof(_d), "# [CAL] F%u A stop\r\n", (unsigned)phase);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    HAL_TIM_Base_Stop(&htim2);
    HAL_ADC_Stop_DMA(hadc);
    __HAL_ADC_CLEAR_FLAG(hadc, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);

    _n = snprintf(_d, sizeof(_d), "# [CAL] F%u B start\r\n", (unsigned)phase);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    HAL_StatusTypeDef st = HAL_ADC_Start_DMA(hadc, (uint32_t *)buf, NUM_SAMPLES * 2u);

    _n = snprintf(_d, sizeof(_d), "# [CAL] F%u C tim st=%d\r\n", (unsigned)phase, (int)st);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    HAL_TIM_Base_Start(&htim2);

    uint32_t t0 = HAL_GetTick();
    uint32_t t_last = t0;
    while (!(*ready) && (HAL_GetTick() - t0) < 500u) {
        uint32_t now = HAL_GetTick();
        if (now - t_last >= 20u) {
            t_last = now;
            _n = snprintf(_d, sizeof(_d), "# [CAL] F%u E t=%lu ISR=0x%lx\r\n",
                          (unsigned)phase,
                          (unsigned long)(now - t0),
                          (unsigned long)hadc->Instance->ISR);
            HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 10u);
        }
        __NOP();
    }

    _n = snprintf(_d, sizeof(_d), "# [CAL] F%u F rdy=%d t=%lu\r\n",
                  (unsigned)phase, (int)(*ready), (unsigned long)(HAL_GetTick() - t0));
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    if (*ready) {
        Sampler_Deinterleave(buf, ph->vRaw, ph->iRaw);
    }
}

/* Mede piso de ruído de uma fase e retorna (max_vrms, max_irms).
 * Retorna 0 se bem-sucedido, 1 se corrente alta detectada (carga ligada). */
static uint8_t Calibrate_Phase(uint8_t phase, float *out_vrms, float *out_irms)
{
    PhaseData *ph = (phase == 1u) ? &ph1 : &ph2;
    float max_vrms = 0.0f, max_irms = 0.0f;

    for (uint32_t f = 0u; f < NOISE_CAL_FRAMES; f++) {
        char frameMsg[40];
        int  frameLen = snprintf(frameMsg, sizeof(frameMsg),
                                 "# [CAL] F%u Frame %lu/%u\r\n",
                                 (unsigned)phase, (unsigned long)(f + 1u), (unsigned)NOISE_CAL_FRAMES);
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)frameMsg, (uint16_t)frameLen, 50u);

        Calibrate_Acquire(phase);

        uint64_t sumV = 0u, sumI = 0u;
        for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
            sumV += ph->vRaw[i];
            sumI += ph->iRaw[i];
        }
        float meanV = (float)sumV / NUM_SAMPLES;
        float meanI = (float)sumI / NUM_SAMPLES;

        float sumV2 = 0.0f, sumI2 = 0.0f;
        for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
            float v = (float)ph->vRaw[i] - meanV;
            float c = (float)ph->iRaw[i] - meanI;
            sumV2 += v * v;
            sumI2 += c * c;
        }

        float vn  = sqrtf(sumV2 / NUM_SAMPLES) * CAL_V;
        float in_ = sqrtf(sumI2 / NUM_SAMPLES) * CAL_I;
        if (vn  > max_vrms) max_vrms = vn;
        if (in_ > max_irms) max_irms = in_;
    }

    char dbg[80];
    int  dlen = snprintf(dbg, sizeof(dbg),
        "# [CAL] F%u medido: Irms_max=%.3fA\r\n", (unsigned)phase, max_irms);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)dbg, (uint16_t)dlen, 100u);

    *out_vrms = max_vrms;
    *out_irms = max_irms;
    return (max_irms > 5.0f) ? 1u : 0u;
}

/* Mede piso de ruído das duas fases e atualiza g_v_floor/g_i_floor (fase 1)
 * e g_v_floor2/g_i_floor2 (fase 2) com pisos independentes. */
static void Calibrate_NoiseFloor(void)
{
    const char *startMsg = "# [CAL] Iniciando calibracao bifasica — desconecte cargas!\r\n";
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)startMsg, (uint16_t)strlen(startMsg), 200u);

    ST7735_FillScreen(ST7735_BLACK); ST7735_Main_Reset();
    ST7735_DrawString(0, 20, "CALIBRANDO...", ST7735_YELLOW, ST7735_BLACK);
    ST7735_DrawString(0, 38, "Desconecte",   ST7735_WHITE,  ST7735_BLACK);
    ST7735_DrawString(0, 50, "as cargas!",   ST7735_WHITE,  ST7735_BLACK);

    float mv1, mi1, mv2, mi2;

    /* ── Fase 1 ── */
    if (Calibrate_Phase(1u, &mv1, &mi1)) {
        const char *w = "# [CAL] AVISO F1: corrente alta — usando pisos anteriores.\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)w, (uint16_t)strlen(w), 200u);
        ST7735_FillScreen(ST7735_BLACK); ST7735_Main_Reset();
        ST7735_DrawString(0, 20, "CAL ABORTADA", ST7735_RED,   ST7735_BLACK);
        ST7735_DrawString(0, 38, "F1 I > 5A",   ST7735_WHITE, ST7735_BLACK);
        HAL_Delay(2000u);
        return;
    }

    /* ── Fase 2 ── */
    if (Calibrate_Phase(2u, &mv2, &mi2)) {
        const char *w = "# [CAL] AVISO F2: corrente alta — usando pisos anteriores.\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)w, (uint16_t)strlen(w), 200u);
        ST7735_FillScreen(ST7735_BLACK); ST7735_Main_Reset();
        ST7735_DrawString(0, 20, "CAL ABORTADA", ST7735_RED,   ST7735_BLACK);
        ST7735_DrawString(0, 38, "F2 I > 5A",   ST7735_WHITE, ST7735_BLACK);
        HAL_Delay(2000u);
        return;
    }

    /* Tensão: ZMPT101B em rede — piso fixo para ambas as fases */
    g_v_floor  = V_NOISE_FLOOR_MIN;
    g_v_floor2 = V_NOISE_FLOOR_MIN;

    g_i_floor  = mi1 * NOISE_CAL_MARGIN;
    if (g_i_floor  < I_NOISE_FLOOR_MIN) g_i_floor  = I_NOISE_FLOOR_MIN;

    g_i_floor2 = mi2 * NOISE_CAL_MARGIN;
    if (g_i_floor2 < I_NOISE_FLOOR_MIN) g_i_floor2 = I_NOISE_FLOOR_MIN;

    char msg[160];
    int  len = snprintf(msg, sizeof(msg),
        "# [CAL] OK: F1 Ifloor=%ld.%04ldA | F2 Ifloor=%ld.%04ldA\r\n",
        (long)g_i_floor,  labs((long)(g_i_floor  * 10000.0f) % 10000),
        (long)g_i_floor2, labs((long)(g_i_floor2 * 10000.0f) % 10000));
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, (uint16_t)len, 200u);

    char lcd1[24], lcd2[24];
    snprintf(lcd1, sizeof(lcd1), "F1:%ld.%04ldA",
             (long)g_i_floor,  labs((long)(g_i_floor  * 10000.0f) % 10000));
    snprintf(lcd2, sizeof(lcd2), "F2:%ld.%04ldA",
             (long)g_i_floor2, labs((long)(g_i_floor2 * 10000.0f) % 10000));
    ST7735_FillScreen(ST7735_BLACK); ST7735_Main_Reset();
    ST7735_DrawString(0, 20, "CAL OK",  ST7735_GREEN, ST7735_BLACK);
    ST7735_DrawString(0, 38, lcd1,      ST7735_CYAN,  ST7735_BLACK);
    ST7735_DrawString(0, 50, lcd2,      ST7735_CYAN,  ST7735_BLACK);
    HAL_Delay(2000u);
}

/* Cálculo Vrms, Irms, Preal, S, Q, FP e transmissão do frame de potência.
 * frameType = 0x01 (fase 1) ou 0x03 (fase 2). Opera sobre os buffers de 'ph'. */
static void Sampler_ComputeAndSend(PhaseData *ph, uint8_t frameType)
{
    double   sumV = 0.0, sumI = 0.0;
    uint32_t vmax = 0u,  vmin = UINT32_MAX;
    uint32_t imax = 0u,  imin = UINT32_MAX;
    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        sumV += ph->vRaw[i];
        sumI += ph->iRaw[i];
        if (ph->vRaw[i] > vmax) vmax = ph->vRaw[i];
        if (ph->vRaw[i] < vmin) vmin = ph->vRaw[i];
        if (ph->iRaw[i] > imax) imax = ph->iRaw[i];
        if (ph->iRaw[i] < imin) imin = ph->iRaw[i];
    }
    double meanV = sumV / NUM_SAMPLES;
    double meanI = sumI / NUM_SAMPLES;

    double sumV2 = 0.0, sumI2 = 0.0, sumVI = 0.0;
    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        double v  = (double)ph->vRaw[i] - meanV;
        double ci = (double)ph->iRaw[i] - meanI;
        sumV2 += v  * v;
        sumI2 += ci * ci;
        sumVI += v  * ci;
    }

    float Vrms  = (float)(sqrt(sumV2 / NUM_SAMPLES)) * CAL_V;
    float Irms  = (float)(sqrt(sumI2 / NUM_SAMPLES)) * CAL_I;
    float Preal = (float)(sumVI / NUM_SAMPLES) * CAL_V * CAL_I * SCT013_SIGN;

    ph->irms_buf[ph->irms_idx] = Irms;
    ph->irms_idx = (ph->irms_idx + 1u) % IRMS_AVG_N;
    float irms_sum = 0.0f;
    for (uint32_t k = 0u; k < IRMS_AVG_N; k++) irms_sum += ph->irms_buf[k];
    Irms = irms_sum / (float)IRMS_AVG_N;

    /* Pisos independentes por fase — frameType 0x01=F1, 0x03=F2 */
    float vfloor = (frameType == 0x01u) ? g_v_floor  : g_v_floor2;
    float ifloor = (frameType == 0x01u) ? g_i_floor  : g_i_floor2;
    if (Vrms < vfloor) { Vrms = 0.0f; Preal = 0.0f; }
    if (Irms < ifloor) { Irms = 0.0f; Preal = 0.0f; }

    float S  = Vrms * Irms;
    float Q2 = S * S - Preal * Preal;
    float Q  = (Q2 > 0.0f) ? sqrtf(Q2) : 0.0f;
    float FP = (S > 0.001f) ? (Preal / S) : 0.0f;
    if (FP >  1.0f) FP =  1.0f;
    if (FP < -1.0f) FP = -1.0f;

    ph->vrms       = Vrms;
    ph->irms       = Irms;
    ph->preal      = Preal;
    ph->papar      = S;
    ph->preati     = Q;
    ph->fp         = FP;
    ph->adc_mean_v = (uint32_t)meanV;
    ph->adc_mean_i = (uint32_t)meanI;
    ph->adc_pp_v   = vmax - vmin;
    ph->adc_pp_i   = imax - imin;

    uint8_t hdr[29];
    hdr[0] = 0xAB; hdr[1] = 0xCD; hdr[2] = frameType;
    memcpy(&hdr[3],  &Vrms,  4);
    memcpy(&hdr[7],  &Irms,  4);
    memcpy(&hdr[11], &FP,    4);
    memcpy(&hdr[15], &Preal, 4);
    memcpy(&hdr[19], &Q,     4);
    memcpy(&hdr[23], &S,     4);
    hdr[27] = (uint8_t)IDF_N;
    hdr[28] = (uint8_t)IDF_STEP;

    int32_t samples[IDF_N * 2u];
    for (uint32_t i = 0u; i < IDF_N; i++) {
        uint32_t idx     = i * IDF_STEP;
        samples[i*2u]    = (int32_t)((double)ph->vRaw[idx] - meanV);
        samples[i*2u+1u] = (int32_t)((double)ph->iRaw[idx] - meanI);
    }

    uint8_t footer[2] = {0xEF, 0xFE};
    HAL_UART_Transmit(&hlpuart1, hdr,                29u,          50u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)samples, IDF_N * 8u, 200u);
    HAL_UART_Transmit(&hlpuart1, footer,              2u,           10u);
}

/* Goertzel — magnitude de pico normalizada do bin k para sinal x[N] */
static float Goertzel_Magnitude(float *x, uint32_t N, uint32_t bin)
{
    float omega = 2.0f * (float)M_PI * (float)bin / (float)N;
    float coeff = 2.0f * cosf(omega);
    float sp = 0.0f, sp2 = 0.0f, s;
    for (uint32_t n = 0u; n < N; n++) {
        s   = x[n] + coeff * sp - sp2;
        sp2 = sp;
        sp  = s;
    }
    float re = sp - sp2 * cosf(omega);
    float im =      sp2 * sinf(omega);
    return 2.0f * sqrtf(re * re + im * im) / (float)N;
}

/* Calcula magnitudes dos harmônicos 1..HARM_MAX e THD para V e I da fase 'ph' */
static void Harmonics_Compute(PhaseData *ph)
{
    double sumV = 0.0, sumI = 0.0;
    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        sumV += ph->vRaw[i];
        sumI += ph->iRaw[i];
    }
    float meanV = (float)(sumV / NUM_SAMPLES);
    float meanI = (float)(sumI / NUM_SAMPLES);

    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        ph->vFloat[i] = ((float)ph->vRaw[i] - meanV) * CAL_V;
        ph->iFloat[i] = ((float)ph->iRaw[i] - meanI) * CAL_I;
    }

    ph->harmMagV[0u] = 0.0f;
    ph->harmMagI[0u] = 0.0f;
    for (uint32_t h = 1u; h <= HARM_MAX; h++) {
        ph->harmMagV[h] = Goertzel_Magnitude(ph->vFloat, NUM_SAMPLES, h * F0_BIN);
        ph->harmMagI[h] = Goertzel_Magnitude(ph->iFloat, NUM_SAMPLES, h * F0_BIN);
    }

    float sumV2 = 0.0f, sumI2 = 0.0f;
    for (uint32_t h = 2u; h <= HARM_MAX; h++) {
        sumV2 += ph->harmMagV[h] * ph->harmMagV[h];
        sumI2 += ph->harmMagI[h] * ph->harmMagI[h];
    }
    ph->thd_v = (ph->harmMagV[1u] > 0.001f) ? sqrtf(sumV2) / ph->harmMagV[1u] : 0.0f;
    ph->thd_i = (ph->harmMagI[1u] > 0.001f) ? sqrtf(sumI2) / ph->harmMagI[1u] : 0.0f;
}

/* Frame de harmônicos: [AB CD TT] THD_V THD_I HarmV[1..50] HarmI[1..50] [EF FE]
 * frameType = 0x02 (fase 1) ou 0x04 (fase 2).
 * Total: 3 + 4 + 4 + 200 + 200 + 2 = 413 bytes */
static void Send_HarmonicsFrame(PhaseData *ph, uint8_t frameType)
{
    uint8_t hdr[3]    = {0xAB, 0xCD, frameType};
    uint8_t footer[2] = {0xEF, 0xFE};
    HAL_UART_Transmit(&hlpuart1, hdr,                          3u,             10u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&ph->thd_v,        4u,             10u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&ph->thd_i,        4u,             10u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&ph->harmMagV[1u], HARM_MAX * 4u, 200u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&ph->harmMagI[1u], HARM_MAX * 4u, 200u);
    HAL_UART_Transmit(&hlpuart1, footer,                        2u,             10u);
}

/* Botão KEY (PC13, ativo ALTO) — envia status via UART */
static void Debug_SendStatus(void)
{
    char     msg[128];
    int      len;
    uint32_t uptime_s = HAL_GetTick() / 1000u;

    len = snprintf(msg, sizeof(msg),
        "# [STM32 DEBUG] up:%lus frm:%lu | V:%ld.%02ld I:%ld.%04ld P:%ld.%02ld FP:%ld.%03ld\r\n",
        (unsigned long)uptime_s, (unsigned long)frame_count,
        (long)ph1.vrms,  (long)(ph1.vrms  * 100.0f)   % 100,
        (long)ph1.irms,  (long)(ph1.irms  * 10000.0f) % 10000,
        (long)ph1.preal, labs((long)(ph1.preal * 100.0f)  % 100),
        (long)ph1.fp,    labs((long)(ph1.fp    * 1000.0f) % 1000));

    HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, (uint16_t)len, 200u);
    
    const char *doneMsg = "# [CAL] Calibracao concluida com sucesso!\r\n";
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)doneMsg, (uint16_t)strlen(doneMsg), 50u);
}

/* DMA completo — para timer, invalida D-cache, sinaliza main loop */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    /* ADC1 e ADC2 disparam no mesmo trigger TIM2 e terminam o frame juntos.
     * Parar o timer no callback do ADC1 não perde dados do ADC2: a última
     * transferência DMA da fase 2 vem do mesmo trigger já consumido. */
    if (hadc == &hadc1) {
        HAL_TIM_Base_Stop(&htim2);
        /* D-cache desligada neste projeto — SCB_InvalidateDCache_by_Addr é no-op
         * e UNPREDICTABLE quando cache está off (ARM TRM): removida para evitar fault */
        __DSB();
        samplesReady = 1u;
    } else if (hadc == &hadc2) {
        __DSB();
        samplesReady2 = 1u;
    }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    /* Não usar UART em ISR — erro registado em hadc->ErrorCode, visível via debug */
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_TIM2_Init();
  MX_LPUART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* Habilita DWT cycle counter (mantido para compatibilidade com código legado) */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

  /* Calibração de offset dos dois ADCs antes do primeiro uso */
  HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED);
  HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED);

  /* Display ST7735 — inicializa e mostra tela inicial */
  ST7735_Init();
  ST7735_Main_Update(0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,
                     0.0f,0.0f,0.0f,0.0f,0.0f,0.0f, 0u, 0u);

  uint32_t btnLastTick = 0u;

  const char *bootMsg = "# [STM32 BOOT] WeAct H743 OK | ADC1 CH3+CH4 + ADC2 CH7+CH8 (bifasico) DMA | TIM2 15360Hz | LPUART1 460800\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)bootMsg, (uint16_t)strlen(bootMsg), 200);

  /* Zera buffers DMA: seção NOLOAD não é inicializada pelo startup — garbage causaria
   * falsa detecção de sinal alto na calibração */
  memset((void *)adcDmaBuf,  0, sizeof(adcDmaBuf));
  memset((void *)adcDmaBuf2, 0, sizeof(adcDmaBuf2));

  /* Calibração automática do piso de ruído — certifique-se de não ter cargas conectadas */
  Calibrate_NoiseFloor();

  /* Inicia primeira aquisição DMA — ADC2 e ADC1 armados antes do timer para
   * capturarem o mesmo primeiro trigger (fases alinhadas) */
  HAL_ADC_Stop_DMA(&hadc1);
  HAL_ADC_Stop_DMA(&hadc2);
  __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
  __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
  HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adcDmaBuf2, NUM_SAMPLES * 2u);
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf,  NUM_SAMPLES * 2u);
  HAL_TIM_Base_Start(&htim2);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* Espera as duas fases (ADC1 + ADC2) — ambas terminam no mesmo trigger */
    if (samplesReady && samplesReady2) {
        samplesReady  = 0u;
        samplesReady2 = 0u;
        Sampler_Deinterleave(adcDmaBuf,  ph1.vRaw, ph1.iRaw);
        Sampler_Deinterleave(adcDmaBuf2, ph2.vRaw, ph2.iRaw);
        Sampler_ComputeAndSend(&ph1, 0x01u);   /* fase 1 */
        Sampler_ComputeAndSend(&ph2, 0x03u);   /* fase 2 */
        frame_count++;
        if ((frame_count % HARM_FRAME_DIV) == 0u) {
            Harmonics_Compute(&ph1);
            Send_HarmonicsFrame(&ph1, 0x02u);  /* harmônicos fase 1 */
            Harmonics_Compute(&ph2);
            Send_HarmonicsFrame(&ph2, 0x04u);  /* harmônicos fase 2 */
        }
        ST7735_Main_Update(ph1.vrms, ph1.irms, ph1.preal, ph1.preati, ph1.papar, ph1.fp,
                           ph2.vrms, ph2.irms, ph2.preal, ph2.preati, ph2.papar, ph2.fp,
                           HAL_GetTick() / 1000u, frame_count);
        /* Reinicia aquisição das duas fases para o próximo frame */
        HAL_ADC_Stop_DMA(&hadc1);
        HAL_ADC_Stop_DMA(&hadc2);
        __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
        __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
        HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adcDmaBuf2, NUM_SAMPLES * 2u);
        HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf,  NUM_SAMPLES * 2u);
        HAL_TIM_Base_Start(&htim2);
    }

    /* Botão KEY (PC13, ativo ALTO) — debounce 300 ms
     * Pressionar com nada conectado recalibra o piso de ruído. */
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_SET) {
        if ((HAL_GetTick() - btnLastTick) >= 300u) {
            btnLastTick = HAL_GetTick();
            HAL_TIM_Base_Stop(&htim2);
            HAL_ADC_Stop_DMA(&hadc1);
            HAL_ADC_Stop_DMA(&hadc2);
            Calibrate_NoiseFloor();
            Debug_SendStatus();
            /* Reinicia aquisição das duas fases após calibração */
            HAL_ADC_Stop_DMA(&hadc1);
            HAL_ADC_Stop_DMA(&hadc2);
            __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
            __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
            samplesReady  = 0u;
            samplesReady2 = 0u;
            HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adcDmaBuf2, NUM_SAMPLES * 2u);
            HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf,  NUM_SAMPLES * 2u);
            HAL_TIM_Base_Start(&htim2);
        }
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 60;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_ADC1_Init(void)
{


  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};


  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler        = ADC_CLOCK_ASYNC_DIV4;        /* 80MHz/4=20MHz (Mais tempo para DMA respirar) */
  hadc1.Init.Resolution            = ADC_RESOLUTION_16B;
  hadc1.Init.ScanConvMode          = ADC_SCAN_ENABLE;
  hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait      = DISABLE;
  hadc1.Init.ContinuousConvMode    = DISABLE;
  hadc1.Init.NbrOfConversion       = 2;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIG_T2_TRGO;
  hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
  hadc1.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.LeftBitShift          = ADC_LEFTBITSHIFT_NONE;
  hadc1.Init.OversamplingMode      = DISABLE;
  hadc1.Init.Oversampling.Ratio    = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** CH3 = PA6 — tensão (Rank 1)
  */
  sConfig.Channel      = ADC_CHANNEL_3;
  sConfig.Rank         = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_32CYCLES_5;

  sConfig.SingleDiff   = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset       = 0;
  sConfig.OffsetSignedSaturation = DISABLE;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** CH4 = PC4 — corrente (Rank 2)
  */
  sConfig.Channel      = ADC_CHANNEL_4;
  sConfig.Rank         = ADC_REGULAR_RANK_2;
  sConfig.SamplingTime = ADC_SAMPLETIME_32CYCLES_5;

  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/* ADC2 — fase 2 da análise bifásica. Mesma config do ADC1, disparado pelo
 * mesmo TIM2_TRGO (conversão paralela → não aumenta o tempo de amostragem).
 * Canais: CH7=PA7 (tensão V2, Rank 1), CH8=PC5 (corrente I2, Rank 2). */
static void MX_ADC2_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc2.Instance = ADC2;
  hadc2.Init.ClockPrescaler        = ADC_CLOCK_ASYNC_DIV4;
  hadc2.Init.Resolution            = ADC_RESOLUTION_16B;
  hadc2.Init.ScanConvMode          = ADC_SCAN_ENABLE;
  hadc2.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
  hadc2.Init.LowPowerAutoWait      = DISABLE;
  hadc2.Init.ContinuousConvMode    = DISABLE;
  hadc2.Init.NbrOfConversion       = 2;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv      = ADC_EXTERNALTRIG_T2_TRGO;
  hadc2.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc2.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
  hadc2.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
  hadc2.Init.LeftBitShift          = ADC_LEFTBITSHIFT_NONE;
  hadc2.Init.OversamplingMode      = DISABLE;
  hadc2.Init.Oversampling.Ratio    = 1;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /** CH7 = PA7 — tensão fase 2 (Rank 1)
  */
  sConfig.Channel      = ADC_CHANNEL_7;
  sConfig.Rank         = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_32CYCLES_5;
  sConfig.SingleDiff   = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset       = 0;
  sConfig.OffsetSignedSaturation = DISABLE;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** CH8 = PC5 — corrente fase 2 (Rank 2)
  */
  sConfig.Channel      = ADC_CHANNEL_8;
  sConfig.Rank         = ADC_REGULAR_RANK_2;
  sConfig.SamplingTime = ADC_SAMPLETIME_32CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */

// Pinos LPUART1: PA9 (TX), PA10 (RX)

static void MX_LPUART1_UART_Init(void)
{
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 460800;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  hlpuart1.FifoMode = UART_FIFOMODE_DISABLE;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }

}

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* Botão KEY WeAct — PC13, ativo ALTO (SW2 conecta PC13 a VDD quando pressionado) */
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin  = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* GPIOE necessário para o display ST7735 (PE10–PE14) */
  __HAL_RCC_GPIOE_CLK_ENABLE();
}

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  HAL_MPU_Disable();

  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  char msg[] = "\r\n!!! ERROR_HANDLER !!!\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, 25u, 100u);
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
