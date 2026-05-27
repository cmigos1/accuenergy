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
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
TIM_HandleTypeDef htim2;

UART_HandleTypeDef hlpuart1;

/* USER CODE BEGIN PV */
/* DMA buffer em AXI SRAM (DMA2-acessível) — seção .dma_buffer no linker script */
static volatile uint32_t adcDmaBuf[NUM_SAMPLES * 2u] __attribute__((section(".dma_buffer"), aligned(32)));
static volatile uint8_t  samplesReady = 0u;

/* Canais separados (pós-deinterleave) */
static uint32_t vRaw[NUM_SAMPLES];
static uint32_t iRaw[NUM_SAMPLES];

/* Buffers de trabalho para Goertzel (sinais centrados em unidades físicas) */
static float vFloat[NUM_SAMPLES];
static float iFloat[NUM_SAMPLES];

/* Últimos valores calculados — compartilhados entre Sampler e display/debug */
static float last_vrms   = 0.0f;
static float last_irms   = 0.0f;
static float last_preal  = 0.0f;
static float last_papar  = 0.0f;
static float last_preati = 0.0f;
static float last_fp     = 0.0f;
static uint32_t frame_count = 0u;

/* Raw ADC para calibração / debug */
static uint32_t last_adc_mean_v = 0u;
static uint32_t last_adc_mean_i = 0u;
static uint32_t last_adc_pp_v   = 0u;
static uint32_t last_adc_pp_i   = 0u;

/* Pisos de ruído dinâmicos — inicializados conservadoramente */
static float g_v_floor = 2.0f;
static float g_i_floor = 1.2f;

/* Harmônicos e THD */
static float last_thd_v = 0.0f;
static float last_thd_i = 0.0f;
static float harmMagV[HARM_MAX + 1u];
static float harmMagI[HARM_MAX + 1u];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM2_Init(void);
static void MX_LPUART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static void Sampler_Deinterleave(void);
static void Calibrate_Acquire(void);
static void Calibrate_NoiseFloor(void);
static void Sampler_ComputeAndSend(void);
static float Goertzel_Magnitude(float *x, uint32_t N, uint32_t bin);
static void Harmonics_Compute(void);
static void Send_HarmonicsFrame(void);
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
}

/* Separa buffer DMA interleaved [V0 I0 V1 I1 ...] → vRaw[] e iRaw[] */
static void Sampler_Deinterleave(void)
{
    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        vRaw[i] = adcDmaBuf[i * 2u];
        iRaw[i] = adcDmaBuf[i * 2u + 1u];
    }
}

/* Aquisição bloqueante — para uso em Calibrate_NoiseFloor */
static void Calibrate_Acquire(void)
{
    char _d[160]; int _n;
    samplesReady = 0u;

    _n = snprintf(_d, sizeof(_d), "# [CAL] A stop\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    HAL_TIM_Base_Stop(&htim2);
    HAL_ADC_Stop_DMA(&hadc1);

    /* Limpa flags residuais e erros (importante no H7 para evitar travamentos em re-start) */
    __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);

    _n = snprintf(_d, sizeof(_d), "# [CAL] B start\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    HAL_StatusTypeDef st = HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf, NUM_SAMPLES * 2u);

    _n = snprintf(_d, sizeof(_d), "# [CAL] C tim st=%d\r\n", (int)st);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    HAL_TIM_Base_Start(&htim2);

    _n = snprintf(_d, sizeof(_d), "# [CAL] D t=%lu\r\n", (unsigned long)HAL_GetTick());
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    _n = snprintf(_d, sizeof(_d), "# [CAL] E pre-loop\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    uint32_t t0 = HAL_GetTick();
    uint32_t t_last = t0;
    while (!samplesReady && (HAL_GetTick() - t0) < 500u) {
        uint32_t now = HAL_GetTick();
        if (now - t_last >= 20u) {
            t_last = now;
            /* Diagnóstico de registros para entender onde o pipeline trava */
            _n = snprintf(_d, sizeof(_d), "# [CAL] E t=%lu NDTR=%lu ISR=0x%lx\r\n",
                          (unsigned long)(now - t0),
                          (unsigned long)((DMA_Stream_TypeDef *)hdma_adc1.Instance)->NDTR,
                          (unsigned long)hadc1.Instance->ISR);
            HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 10u);
        }
        __NOP();
    }

    _n = snprintf(_d, sizeof(_d), "# [CAL] F rdy=%d t=%lu\r\n",
                  (int)samplesReady, (unsigned long)(HAL_GetTick() - t0));
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)_d, (uint16_t)((_n > (int)sizeof(_d)) ? sizeof(_d) : _n), 50u);

    if (samplesReady) {
        Sampler_Deinterleave();
    }
}

/* Mede piso de ruído com NOISE_CAL_FRAMES aquisições e atualiza g_v_floor / g_i_floor */
static void Calibrate_NoiseFloor(void)
{
    const char *startMsg = "# [CAL] Iniciando calibracao — desconecte cargas!\r\n";
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)startMsg, (uint16_t)strlen(startMsg), 200u);

    ST7735_FillScreen(ST7735_BLACK);
    ST7735_DrawString(0, 30, "CALIBRANDO...", ST7735_YELLOW, ST7735_BLACK);
    ST7735_DrawString(0, 50, "Desconecte",   ST7735_WHITE,  ST7735_BLACK);
    ST7735_DrawString(0, 62, "as cargas!",   ST7735_WHITE,  ST7735_BLACK);

    float max_vrms = 0.0f, max_irms = 0.0f;

    for (uint32_t f = 0u; f < NOISE_CAL_FRAMES; f++) {
        char frameMsg[32];
        int frameLen = snprintf(frameMsg, sizeof(frameMsg), "# [CAL] Frame %lu/%u\r\n", (unsigned long)(f+1), (unsigned)NOISE_CAL_FRAMES);
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)frameMsg, (uint16_t)frameLen, 50u);

        Calibrate_Acquire();

        /* Cálculo simplificado para evitar hangs/stack overflow */
        uint64_t sumV = 0, sumI = 0;
        for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
            sumV += vRaw[i];
            sumI += iRaw[i];
        }
        float meanV = (float)sumV / NUM_SAMPLES;
        float meanI = (float)sumI / NUM_SAMPLES;

        float sumV2 = 0.0f, sumI2 = 0.0f;
        for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
            float v = (float)vRaw[i] - meanV;
            float c = (float)iRaw[i] - meanI;
            sumV2 += v * v;
            sumI2 += c * c;
        }

        float vn = sqrtf(sumV2 / NUM_SAMPLES) * CAL_V;
        float in_ = sqrtf(sumI2 / NUM_SAMPLES) * CAL_I;
        
        if (vn > max_vrms) max_vrms = vn;
        if (in_ > max_irms) max_irms = in_;
    }

    /* Imprime valores medidos para diagnóstico */
    {
        char dbg[96];
        int  dlen = snprintf(dbg, sizeof(dbg),
            "# [CAL] medido: Irms=%.3fA (max=%.3fA)\r\n", max_irms, max_irms);
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)dbg, (uint16_t)dlen, 100u);
    }

    /* Tensão: ZMPT101B sempre ligado à rede — usa piso fixo.
     * Corrente: aborta só se > 5A (carga real ligada, não ruído de CT). */
    if (max_irms > 5.0f) {
        const char *warnMsg = "# [CAL] AVISO: corrente alta — carga ligada? Usando pisos anteriores.\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)warnMsg, (uint16_t)strlen(warnMsg), 200u);
        ST7735_FillScreen(ST7735_BLACK);
        ST7735_DrawString(0, 30, "CAL ABORTADA", ST7735_RED,   ST7735_BLACK);
        ST7735_DrawString(0, 50, "Corrente alta",ST7735_WHITE, ST7735_BLACK);
        ST7735_DrawString(0, 62, "Carga ligada?",ST7735_WHITE, ST7735_BLACK);
        HAL_Delay(2000u);
        return;
    }

    g_v_floor = V_NOISE_FLOOR_MIN;   /* sempre fixo — sensor de tensão sempre em rede */
    g_i_floor = max_irms * NOISE_CAL_MARGIN;
    if (g_i_floor < I_NOISE_FLOOR_MIN) g_i_floor = I_NOISE_FLOOR_MIN;

    char msg[128];
    int  len = snprintf(msg, sizeof(msg),
        "# [CAL] OK: Vfloor=%ld.%02ldV Ifloor=%ld.%04ldA (margem x%d.%d)\r\n",
        (long)g_v_floor,  labs((long)(g_v_floor  * 100.0f)   % 100),
        (long)g_i_floor,  labs((long)(g_i_floor  * 10000.0f) % 10000),
        (int)NOISE_CAL_MARGIN, (int)((NOISE_CAL_MARGIN * 10.0f)) % 10);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, (uint16_t)len, 200u);

    char lcd1[24], lcd2[24];
    snprintf(lcd1, sizeof(lcd1), "Vfloor:%ld.%02ldV",
             (long)g_v_floor, labs((long)(g_v_floor * 100.0f) % 100));
    snprintf(lcd2, sizeof(lcd2), "Ifloor:%ld.%04ldA",
             (long)g_i_floor, labs((long)(g_i_floor * 10000.0f) % 10000));
    ST7735_FillScreen(ST7735_BLACK);
    ST7735_DrawString(0, 30, "CAL OK",  ST7735_GREEN, ST7735_BLACK);
    ST7735_DrawString(0, 50, lcd1,      ST7735_CYAN,  ST7735_BLACK);
    ST7735_DrawString(0, 62, lcd2,      ST7735_CYAN,  ST7735_BLACK);
    HAL_Delay(2000u);
}

/* Cálculo Vrms, Irms, Preal, S, Q, FP e transmissão do frame 0x01 */
static void Sampler_ComputeAndSend(void)
{
    double   sumV = 0.0, sumI = 0.0;
    uint32_t vmax = 0u,  vmin = UINT32_MAX;
    uint32_t imax = 0u,  imin = UINT32_MAX;
    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        sumV += vRaw[i];
        sumI += iRaw[i];
        if (vRaw[i] > vmax) vmax = vRaw[i];
        if (vRaw[i] < vmin) vmin = vRaw[i];
        if (iRaw[i] > imax) imax = iRaw[i];
        if (iRaw[i] < imin) imin = iRaw[i];
    }
    double meanV = sumV / NUM_SAMPLES;
    double meanI = sumI / NUM_SAMPLES;

    double sumV2 = 0.0, sumI2 = 0.0, sumVI = 0.0;
    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        double v  = (double)vRaw[i] - meanV;
        double ci = (double)iRaw[i] - meanI;
        sumV2 += v  * v;
        sumI2 += ci * ci;
        sumVI += v  * ci;
    }

    float Vrms  = (float)(sqrt(sumV2 / NUM_SAMPLES)) * CAL_V;
    float Irms  = (float)(sqrt(sumI2 / NUM_SAMPLES)) * CAL_I;
    float Preal = (float)(sumVI / NUM_SAMPLES) * CAL_V * CAL_I * SCT013_SIGN;

    static float    irms_buf[IRMS_AVG_N] = {0.0f};
    static uint32_t irms_idx = 0u;
    irms_buf[irms_idx] = Irms;
    irms_idx = (irms_idx + 1u) % IRMS_AVG_N;
    float irms_sum = 0.0f;
    for (uint32_t k = 0u; k < IRMS_AVG_N; k++) irms_sum += irms_buf[k];
    Irms = irms_sum / (float)IRMS_AVG_N;

    if (Vrms < g_v_floor) { Vrms = 0.0f; Preal = 0.0f; }
    if (Irms < g_i_floor) { Irms = 0.0f; Preal = 0.0f; }

    float S  = Vrms * Irms;
    float Q2 = S * S - Preal * Preal;
    float Q  = (Q2 > 0.0f) ? sqrtf(Q2) : 0.0f;
    float FP = (S > 0.001f) ? (Preal / S) : 0.0f;
    if (FP >  1.0f) FP =  1.0f;
    if (FP < -1.0f) FP = -1.0f;

    last_vrms       = Vrms;
    last_irms       = Irms;
    last_preal      = Preal;
    last_papar      = S;
    last_preati     = Q;
    last_fp         = FP;
    last_adc_mean_v = (uint32_t)meanV;
    last_adc_mean_i = (uint32_t)meanI;
    last_adc_pp_v   = vmax - vmin;
    last_adc_pp_i   = imax - imin;
    frame_count++;

    uint8_t hdr[29];
    hdr[0] = 0xAB; hdr[1] = 0xCD; hdr[2] = 0x01;
    memcpy(&hdr[3],  &Vrms,  4);
    memcpy(&hdr[7],  &Irms,  4);
    memcpy(&hdr[11], &FP,    4);
    memcpy(&hdr[15], &Preal, 4);
    memcpy(&hdr[19], &Q,     4);
    memcpy(&hdr[23], &S,     4);
    hdr[27] = (uint8_t)IDF_N;
    hdr[28] = (uint8_t)IDF_STEP;

    int16_t samples[IDF_N * 2u];
    for (uint32_t i = 0u; i < IDF_N; i++) {
        uint32_t idx     = i * IDF_STEP;
        samples[i*2u]    = (int16_t)((double)vRaw[idx] - meanV);
        samples[i*2u+1u] = (int16_t)((double)iRaw[idx] - meanI);
    }

    uint8_t footer[2] = {0xEF, 0xFE};
    HAL_UART_Transmit(&hlpuart1, hdr,                29u,          50u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)samples, IDF_N * 4u, 200u);
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

/* Calcula magnitudes dos harmônicos 1..HARM_MAX e THD para V e I */
static void Harmonics_Compute(void)
{
    double sumV = 0.0, sumI = 0.0;
    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        sumV += vRaw[i];
        sumI += iRaw[i];
    }
    float meanV = (float)(sumV / NUM_SAMPLES);
    float meanI = (float)(sumI / NUM_SAMPLES);

    for (uint32_t i = 0u; i < NUM_SAMPLES; i++) {
        vFloat[i] = ((float)vRaw[i] - meanV) * CAL_V;
        iFloat[i] = ((float)iRaw[i] - meanI) * CAL_I;
    }

    harmMagV[0u] = 0.0f;
    harmMagI[0u] = 0.0f;
    for (uint32_t h = 1u; h <= HARM_MAX; h++) {
        harmMagV[h] = Goertzel_Magnitude(vFloat, NUM_SAMPLES, h * F0_BIN);
        harmMagI[h] = Goertzel_Magnitude(iFloat, NUM_SAMPLES, h * F0_BIN);
    }

    float sumV2 = 0.0f, sumI2 = 0.0f;
    for (uint32_t h = 2u; h <= HARM_MAX; h++) {
        sumV2 += harmMagV[h] * harmMagV[h];
        sumI2 += harmMagI[h] * harmMagI[h];
    }
    last_thd_v = (harmMagV[1u] > 0.001f) ? sqrtf(sumV2) / harmMagV[1u] : 0.0f;
    last_thd_i = (harmMagI[1u] > 0.001f) ? sqrtf(sumI2) / harmMagI[1u] : 0.0f;
}

/* Frame tipo 0x02: [AB CD 02] THD_V THD_I HarmV[1..50] HarmI[1..50] [EF FE]
 * Total: 3 + 4 + 4 + 200 + 200 + 2 = 413 bytes */
static void Send_HarmonicsFrame(void)
{
    uint8_t hdr[3]    = {0xAB, 0xCD, 0x02};
    uint8_t footer[2] = {0xEF, 0xFE};
    HAL_UART_Transmit(&hlpuart1, hdr,                           3u,             10u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&last_thd_v,        4u,             10u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&last_thd_i,        4u,             10u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&harmMagV[1u], HARM_MAX * 4u, 200u);
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)&harmMagI[1u], HARM_MAX * 4u, 200u);
    HAL_UART_Transmit(&hlpuart1, footer,                         2u,             10u);
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
        (long)last_vrms,  (long)(last_vrms  * 100.0f)   % 100,
        (long)last_irms,  (long)(last_irms  * 10000.0f) % 10000,
        (long)last_preal, labs((long)(last_preal * 100.0f)  % 100),
        (long)last_fp,    labs((long)(last_fp    * 1000.0f) % 1000));

    HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, (uint16_t)len, 200u);
    
    const char *doneMsg = "# [CAL] Calibracao concluida com sucesso!\r\n";
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)doneMsg, (uint16_t)strlen(doneMsg), 50u);
}

/* DMA completo — para timer, invalida D-cache, sinaliza main loop */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &hadc1) {
        HAL_TIM_Base_Stop(&htim2);
        /* D-cache desligada neste projeto — SCB_InvalidateDCache_by_Addr é no-op
         * e UNPREDICTABLE quando cache está off (ARM TRM): removida para evitar fault */
        __DSB();
        samplesReady = 1u;
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
  MX_TIM2_Init();
  MX_LPUART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* Habilita DWT cycle counter (mantido para compatibilidade com código legado) */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

  /* Calibração de offset do ADC antes do primeiro uso */
  HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED);

  /* Display ST7735 — inicializa e mostra tela inicial */
  ST7735_Init();
  ST7735_Debug_Update(0.0f, 0.0f, 0.0f, 0.0f, 0u, 0u, 0u, 0u, 0u);

  uint32_t btnLastTick = 0u;

  const char *bootMsg = "# [STM32 BOOT] WeAct H743 OK | ADC1 CH3+CH4 DMA | TIM2 15360Hz | LPUART1 460800\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)bootMsg, (uint16_t)strlen(bootMsg), 200);

  /* Zera adcDmaBuf: seção NOLOAD não é inicializada pelo startup — garbage causaria
   * falsa detecção de sinal alto na calibração */
  memset((void *)adcDmaBuf, 0, sizeof(adcDmaBuf));

  /* Calibração automática do piso de ruído — certifique-se de não ter cargas conectadas */
  Calibrate_NoiseFloor();

  /* Inicia primeira aquisição DMA */
  HAL_ADC_Stop_DMA(&hadc1);
  __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf, NUM_SAMPLES * 2u);
  HAL_TIM_Base_Start(&htim2);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (samplesReady) {
        samplesReady = 0u;
        Sampler_Deinterleave();
        Sampler_ComputeAndSend();
        if ((frame_count % HARM_FRAME_DIV) == 0u) {
            Harmonics_Compute();
            Send_HarmonicsFrame();
        }
        ST7735_Debug_Update(last_vrms, last_irms, last_preal, last_fp,
                            HAL_GetTick() / 1000u,
                            last_adc_mean_v, last_adc_mean_i,
                            last_adc_pp_v,   last_adc_pp_i);
        /* Reinicia aquisição para o próximo frame */
        HAL_ADC_Stop_DMA(&hadc1);
        __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR | ADC_FLAG_EOS | ADC_FLAG_EOC);
        HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf, NUM_SAMPLES * 2u);
        HAL_TIM_Base_Start(&htim2);
    }

    /* Botão KEY (PC13, ativo ALTO) — debounce 300 ms
     * Pressionar com nada conectado recalibra o piso de ruído. */
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_SET) {
        if ((HAL_GetTick() - btnLastTick) >= 300u) {
            btnLastTick = HAL_GetTick();
            HAL_TIM_Base_Stop(&htim2);
            HAL_ADC_Stop_DMA(&hadc1);
            Calibrate_NoiseFloor();
            Debug_SendStatus();
            /* Reinicia aquisição após calibração */
            HAL_ADC_Stop_DMA(&hadc1);
            HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf, NUM_SAMPLES * 2u);
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

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
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

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPUART1_UART_Init(void)
{

  /* USER CODE BEGIN LPUART1_Init 0 */

  /* USER CODE END LPUART1_Init 0 */

  /* USER CODE BEGIN LPUART1_Init 1 */

  /* USER CODE END LPUART1_Init 1 */
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
  /* USER CODE BEGIN LPUART1_Init 2 */

  /* USER CODE END LPUART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
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
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
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
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
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
