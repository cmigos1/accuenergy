/**
 * @file    main.c
 * @brief   NILM STM32H743VIT6 — Main application
 *
 * Architecture:
 *   1. SystemClock → 480 MHz via PLL1 (HSI 64MHz source)
 *   2. ADC1 (2 channels) + DMA triggered by TIM2 @ 10 kHz
 *   3. Process a window of samples (~10 mains cycles = 100ms)
 *   4. Transmit result via SPI1 to ESP32 every ~500ms
 *
 * Data flow:
 *   ADC → DMA buffer → Processing (Vrms, Irms, P, S, PF, freq) → SPI → ESP32
 */

#include "main.h"
#include "nilm_adc.h"
#include "nilm_spi.h"
#include "nilm_processing.h"
#include "nilm_data.h"

/* ================================================================== */
/*                    Forward Declarations                            */
/* ================================================================== */
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);

/* ================================================================== */
/*                    Global Variables                                */
/* ================================================================== */
static nilm_data_t g_spi_packet;       /* Packet to send via SPI      */
static uint32_t    g_last_send_tick;   /* Timestamp of last SPI send  */

#define SPI_SEND_INTERVAL_MS   500     /* Send data every 500ms       */

/* ================================================================== */
/*                    Main Entry Point                               */
/* ================================================================== */
int main(void)
{
    /* --- MCU Initialization --- */
    MPU_Config();
    HAL_Init();
    SystemClock_Config();

    /* --- Peripheral Initialization --- */
    MX_GPIO_Init();      /* LED heartbeat on PE3 */
    NILM_ADC_Init();     /* ADC1 + DMA + TIM2   */
    NILM_SPI_Init();     /* SPI1 Master          */

    /* --- Start ADC acquisition --- */
    NILM_ADC_Start();
    g_last_send_tick = HAL_GetTick();

    /* --- Main loop --- */
    while (1)
    {
        /* Check if a full window of ADC samples is ready */
        if (NILM_ADC_IsWindowReady())
        {
            /* Process the raw samples into power metrics */
            NILM_Processing_Compute(
                NILM_ADC_GetBuffer(),
                NILM_SAMPLES_PER_WINDOW,
                &g_spi_packet
            );

            /* LED toggle = "I'm processing" visual feedback */
            HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_PIN);
        }

        /* Send data via SPI at the configured interval */
        uint32_t now = HAL_GetTick();
        if ((now - g_last_send_tick) >= SPI_SEND_INTERVAL_MS)
        {
            g_last_send_tick = now;

            /* Only send if we have valid data (sample_count > 0) */
            if (g_spi_packet.sample_count > 0)
            {
                NILM_SPI_Transmit(&g_spi_packet);
            }
        }
    }
}

/* ================================================================== */
/*        System Clock Configuration — 480 MHz via PLL1              */
/* ================================================================== */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* Supply configuration: LDO */
    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

    /* Voltage scaling VOS1 for 480 MHz operation */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

    /* HSI = 64 MHz (internal oscillator)
     * PLL1: HSI/4 = 16 MHz input → ×60 = 960 MHz VCO → /2 = 480 MHz SYSCLK */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState       = RCC_HSI_DIV1;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM       = 4;    /* /4 → 16 MHz              */
    RCC_OscInitStruct.PLL.PLLN       = 60;   /* ×60 → 960 MHz VCO        */
    RCC_OscInitStruct.PLL.PLLP       = 2;    /* /2 → 480 MHz SYSCLK      */
    RCC_OscInitStruct.PLL.PLLQ       = 4;    /* /4 → 240 MHz (USB etc.)  */
    RCC_OscInitStruct.PLL.PLLR       = 2;    /* /2 → 480 MHz             */
    RCC_OscInitStruct.PLL.PLLRGE     = RCC_PLL1VCIRANGE_3;  /* 8-16 MHz input range */
    RCC_OscInitStruct.PLL.PLLVCOSEL  = RCC_PLL1VCOWIDE;     /* Wide VCO 192-960 MHz */
    RCC_OscInitStruct.PLL.PLLFRACN   = 0;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /* Bus clocks:
     * SYSCLK = 480 MHz
     * AHB    = SYSCLK / 2 = 240 MHz (max for H743)
     * APB1   = AHB / 2    = 120 MHz
     * APB2   = AHB / 2    = 120 MHz
     * APB3   = AHB / 2    = 120 MHz
     * APB4   = AHB / 2    = 120 MHz
     */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK   | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2
                                | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.SYSCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
    RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

    /* Flash latency for 240 MHz AHB clock: 4 wait states */
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }
}

/* ================================================================== */
/*                         GPIO Init                                 */
/* ================================================================== */
static void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* LED on PE3 (same as Blinky project) */
    HAL_GPIO_WritePin(LED_GPIO_PORT, LED_PIN, GPIO_PIN_RESET);

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin   = LED_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_GPIO_PORT, &gpio);
}

/* ================================================================== */
/*                        MPU Config                                 */
/* ================================================================== */
static void MPU_Config(void)
{
    MPU_Region_InitTypeDef MPU_InitStruct = {0};

    HAL_MPU_Disable();

    /* Region 0: Default 4GB background — deny access to catch stray pointers */
    MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
    MPU_InitStruct.Number           = MPU_REGION_NUMBER0;
    MPU_InitStruct.BaseAddress      = 0x0;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_4GB;
    MPU_InitStruct.SubRegionDisable = 0x87;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable      = MPU_ACCESS_SHAREABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

/* ================================================================== */
/*                       Error Handler                               */
/* ================================================================== */
void Error_Handler(void)
{
    __disable_irq();
    /* Fast blink LED to indicate error */
    while (1)
    {
        /* In a real application, log the error or reset */
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    /* User can add implementation to report file name and line number */
}
#endif
