/**
 * @file    main.h
 * @brief   Main header for NILM_STM32 project
 */

#ifndef MAIN_H
#define MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"

void Error_Handler(void);

/* LED for debug / heartbeat — PE3 (same as Blinky project) */
#define LED_PIN        GPIO_PIN_3
#define LED_GPIO_PORT  GPIOE

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
