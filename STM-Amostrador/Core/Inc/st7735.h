#ifndef INC_ST7735_H
#define INC_ST7735_H

#include "stm32h7xx_hal.h"
#include <stdint.h>

/* Pinos — todos em GPIOE */
#define LCD_PORT      GPIOE
#define LCD_LED_PIN   GPIO_PIN_10
#define LCD_CS_PIN    GPIO_PIN_11
#define LCD_SCL_PIN   GPIO_PIN_12
#define LCD_DC_PIN    GPIO_PIN_13   /* WR_RS: HIGH=dado, LOW=comando */
#define LCD_SDA_PIN   GPIO_PIN_14

/* Dimensões do módulo 0.96" 80x160 */
#define ST7735_W      80u
#define ST7735_H      160u

/* Cores RGB565 */
#define ST7735_BLACK    0x0000u
#define ST7735_WHITE    0xFFFFu
#define ST7735_RED      0xF800u
#define ST7735_GREEN    0x07E0u
#define ST7735_BLUE     0x001Fu
#define ST7735_YELLOW   0xFFE0u
#define ST7735_CYAN     0x07FFu
#define ST7735_ORANGE   0xFD20u
#define ST7735_GRAY     0x8410u

void ST7735_Init(void);
void ST7735_FillScreen(uint16_t color);
void ST7735_DrawString(uint8_t x, uint8_t y, const char *s, uint16_t fg, uint16_t bg);
void ST7735_Debug_Update(float vrms, float irms, float preal, float fp, uint32_t uptime_s,
                          uint32_t adc_mean_v, uint32_t adc_mean_i,
                          uint32_t adc_pp_v,   uint32_t adc_pp_i);

#endif /* INC_ST7735_H */
