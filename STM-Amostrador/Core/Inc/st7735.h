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
void ST7735_DrawString1x(uint8_t x, uint8_t y, const char *s, uint16_t fg, uint16_t bg);

/* Atualiza o painel principal bifásico. Chamar ST7735_Main_Reset() após qualquer
 * ST7735_FillScreen() para forçar o redesenho do fundo estático. */
void ST7735_Main_Update(float v1, float i1, float p1, float q1, float s1, float fp1,
                         float v2, float i2, float p2, float q2, float s2, float fp2,
                         uint32_t uptime_s, uint32_t frame);
void ST7735_Main_Reset(void);

#endif /* INC_ST7735_H */
