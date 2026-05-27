#include "st7735.h"
#include <stdio.h>
#include <string.h>

/* ── Macros de pino via BSRR (acesso direto, sem overhead HAL) ─────────── */
#define CS_L()   LCD_PORT->BSRR = (uint32_t)LCD_CS_PIN  << 16
#define CS_H()   LCD_PORT->BSRR = LCD_CS_PIN
#define DC_L()   LCD_PORT->BSRR = (uint32_t)LCD_DC_PIN  << 16
#define DC_H()   LCD_PORT->BSRR = LCD_DC_PIN
#define SCL_L()  LCD_PORT->BSRR = (uint32_t)LCD_SCL_PIN << 16
#define SCL_H()  LCD_PORT->BSRR = LCD_SCL_PIN
#define SDA_L()  LCD_PORT->BSRR = (uint32_t)LCD_SDA_PIN << 16
#define SDA_H()  LCD_PORT->BSRR = LCD_SDA_PIN
#define LED_H()  LCD_PORT->BSRR = LCD_LED_PIN
#define LED_L()  LCD_PORT->BSRR = (uint32_t)LCD_LED_PIN << 16

/* Offsets do módulo 0.96" 80×160 */
#define COL_OFFSET  26u
#define ROW_OFFSET  1u

/* ── SPI bit-bang (modo 0, MSB first) ───────────────────────────────────── */
/* Delay necessário: H743 @ 480 MHz, sem delay → ~60 MHz no clock.
 * ST7735 suporta no máximo 15 MHz. ~20 NOPs por half-clock ≈ 8 MHz. */
#define SPI_HALF_DELAY()  do { \
    __NOP();__NOP();__NOP();__NOP();__NOP(); \
    __NOP();__NOP();__NOP();__NOP();__NOP(); \
    __NOP();__NOP();__NOP();__NOP();__NOP(); \
    __NOP();__NOP();__NOP();__NOP();__NOP(); \
} while(0)

static void spi_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        SCL_L();
        SPI_HALF_DELAY();
        if (b & (1u << i)) SDA_H(); else SDA_L();
        SCL_H();
        SPI_HALF_DELAY();
    }
}

static void lcd_cmd(uint8_t cmd)
{
    DC_L(); CS_L();
    spi_write_byte(cmd);
    CS_H();
}

static void lcd_data(uint8_t d)
{
    DC_H(); CS_L();
    spi_write_byte(d);
    CS_H();
}

static void lcd_data16(uint16_t d)
{
    DC_H(); CS_L();
    spi_write_byte(d >> 8);
    spi_write_byte(d & 0xFF);
    CS_H();
}

/* ── Configuração do window de endereçamento ────────────────────────────── */
static void set_addr_window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    lcd_cmd(0x2A);                          /* CASET */
    lcd_data(0); lcd_data(x0 + COL_OFFSET);
    lcd_data(0); lcd_data(x1 + COL_OFFSET);

    lcd_cmd(0x2B);                          /* RASET */
    lcd_data(0); lcd_data(y0 + ROW_OFFSET);
    lcd_data(0); lcd_data(y1 + ROW_OFFSET);

    lcd_cmd(0x2C);                          /* RAMWR */
}

/* ── Sequência de inicialização ST7735S ─────────────────────────────────── */
void ST7735_Init(void)
{
    /* Clock e pinos GPIOE */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin   = LCD_LED_PIN | LCD_CS_PIN | LCD_SCL_PIN | LCD_DC_PIN | LCD_SDA_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pull  = GPIO_NOPULL;
    HAL_GPIO_Init(LCD_PORT, &g);

    /* Estado inicial seguro */
    CS_H(); SCL_H(); SDA_H(); DC_H(); LED_H(); /* <-- CORRIGIDO: Mantém o backlight apagado durante o boot */

    /* Software reset */
    lcd_cmd(0x01); HAL_Delay(150);
    lcd_cmd(0x11); HAL_Delay(255);  /* Sleep out */

    /* Frame rate control */
    lcd_cmd(0xB1); lcd_data(0x01); lcd_data(0x2C); lcd_data(0x2D);
    lcd_cmd(0xB2); lcd_data(0x01); lcd_data(0x2C); lcd_data(0x2D);
    lcd_cmd(0xB3);
        lcd_data(0x01); lcd_data(0x2C); lcd_data(0x2D);
        lcd_data(0x01); lcd_data(0x2C); lcd_data(0x2D);

    lcd_cmd(0xB4); lcd_data(0x07);  /* Column inversion */

    /* Power */
    lcd_cmd(0xC0); lcd_data(0xA2); lcd_data(0x02); lcd_data(0x84);
    lcd_cmd(0xC1); lcd_data(0xC5);
    lcd_cmd(0xC2); lcd_data(0x0A); lcd_data(0x00);
    lcd_cmd(0xC3); lcd_data(0x8A); lcd_data(0x2A);
    lcd_cmd(0xC4); lcd_data(0x8A); lcd_data(0xEE);
    lcd_cmd(0xC5); lcd_data(0x0E);  /* VCOM */

    /* MADCTL: portrait normal (MY=MX=0), BGR=1 (maioria dos módulos 0.96") */
    lcd_cmd(0x36); lcd_data(0x08);

    /* Color mode 16-bit RGB565 */
    lcd_cmd(0x3A); lcd_data(0x05);

    /* Gamma */
    lcd_cmd(0xE0);
        lcd_data(0x0F); lcd_data(0x1A); lcd_data(0x0F); lcd_data(0x18);
        lcd_data(0x2F); lcd_data(0x28); lcd_data(0x20); lcd_data(0x22);
        lcd_data(0x1F); lcd_data(0x1B); lcd_data(0x23); lcd_data(0x37);
        lcd_data(0x00); lcd_data(0x07); lcd_data(0x02); lcd_data(0x10);
    lcd_cmd(0xE1);
        lcd_data(0x0F); lcd_data(0x1B); lcd_data(0x0F); lcd_data(0x17);
        lcd_data(0x33); lcd_data(0x2C); lcd_data(0x29); lcd_data(0x2E);
        lcd_data(0x30); lcd_data(0x30); lcd_data(0x39); lcd_data(0x3F);
        lcd_data(0x00); lcd_data(0x07); lcd_data(0x03); lcd_data(0x10);

    lcd_cmd(0x2A);
        lcd_data(0x00); lcd_data(0x00 + COL_OFFSET);
        lcd_data(0x00); lcd_data(ST7735_W - 1 + COL_OFFSET);
    lcd_cmd(0x2B);
        lcd_data(0x00); lcd_data(0x00 + ROW_OFFSET);
        lcd_data(0x00); lcd_data(ST7735_H - 1 + ROW_OFFSET);

    lcd_cmd(0x29); HAL_Delay(100);  /* Display on */
    LED_L();                        /* <-- CORRIGIDO: Liga o Backlight definitivamente */
    HAL_Delay(10);

    ST7735_FillScreen(ST7735_BLUE); /* Azul no boot confirma que o display está OK */
}

/* ── Preenche a tela com uma cor ────────────────────────────────────────── */
void ST7735_FillScreen(uint16_t color)
{
    set_addr_window(0, 0, ST7735_W - 1, ST7735_H - 1);
    DC_H(); CS_L();
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (uint32_t i = 0; i < (uint32_t)ST7735_W * ST7735_H; i++) {
        spi_write_byte(hi);
        spi_write_byte(lo);
    }
    CS_H();
}

/* ── Fonte 5×7 (ASCII 0x20–0x7E) ────────────────────────────────────────── */
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x10,0x08,0x08,0x10,0x08}, /* '~' */
};

/* ── Desenha um caractere 5×7 com escala 2× (10×14 px) ─────────────────── */
static void draw_char2x(uint8_t x, uint8_t y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = font5x7[(uint8_t)(c - 0x20)];

    for (uint8_t col = 0; col < 5; col++) {
        uint8_t coldata = glyph[col];
        for (uint8_t row = 0; row < 7; row++) {
            uint16_t color = (coldata & (1u << row)) ? fg : bg;
            /* cada pixel → bloco 2×2 */
            set_addr_window(x + col*2, y + row*2,
                            x + col*2 + 1, y + row*2 + 1);
            DC_H(); CS_L();
            uint8_t hi = color >> 8, lo = color & 0xFF;
            for (int k = 0; k < 4; k++) { spi_write_byte(hi); spi_write_byte(lo); }
            CS_H();
        }
    }
}

/* ── API pública: string ────────────────────────────────────────────────── */
void ST7735_DrawString(uint8_t x, uint8_t y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) {
        draw_char2x(x, y, *s++, fg, bg);
        x += 11;  /* 10 px largura + 1 px espaço */
        if (x + 10 > ST7735_W) break;
    }
}

/* ── Formata float sem %f (newlib-nano não suporta %f por defeito) ──────── */
/* Equivalente a %6.1f (ex: " 127.3") */
static void fmt_f1(char *out, float v)
{
    int     neg = (v < 0.0f);
    float   av  = neg ? -v : v;
    int32_t iv  = (int32_t)(av * 10.0f + 0.5f);
    char    tmp[12];
    snprintf(tmp, sizeof(tmp), "%s%ld.%ld", neg ? "-" : "", (long)(iv / 10), (long)(iv % 10));
    snprintf(out, 8, "%6s", tmp);
}

/* Equivalente a %6.3f (ex: "  5.234") */
static void fmt_f3(char *out, float v)
{
    int     neg = (v < 0.0f);
    float   av  = neg ? -v : v;
    int32_t iv  = (int32_t)(av * 1000.0f + 0.5f);
    char    tmp[12];
    snprintf(tmp, sizeof(tmp), "%s%ld.%03ld", neg ? "-" : "", (long)(iv / 1000), (long)(iv % 1000));
    snprintf(out, 8, "%6s", tmp);
}

/* ── Atualiza tela de debug ─────────────────────────────────────────────── */
/* Layout portrait 80×160, stride 14px (11 linhas cabem em 154px).
 * Colunas: label 1 char (x=0), valor 6 chars (x=11..76). */
void ST7735_Debug_Update(float vrms, float irms, float preal, float fp, uint32_t uptime_s,
                          uint32_t adc_mean_v, uint32_t adc_mean_i,
                          uint32_t adc_pp_v,   uint32_t adc_pp_i)
{
    static uint32_t s_frame = 0u;
    s_frame++;
    char buf[10];

    ST7735_DrawString(0,   0, "=STM32=", ST7735_CYAN,   ST7735_BLACK);

    ST7735_DrawString(0,  14, "V", ST7735_GRAY, ST7735_BLACK);
    fmt_f1(buf, vrms);
    ST7735_DrawString(11, 14, buf, ST7735_YELLOW, ST7735_BLACK);

    ST7735_DrawString(0,  28, "I", ST7735_GRAY, ST7735_BLACK);
    fmt_f3(buf, irms);
    ST7735_DrawString(11, 28, buf, ST7735_YELLOW, ST7735_BLACK);

    ST7735_DrawString(0,  42, "P", ST7735_GRAY, ST7735_BLACK);
    fmt_f1(buf, preal);
    ST7735_DrawString(11, 42, buf, ST7735_GREEN, ST7735_BLACK);

    ST7735_DrawString(0,  56, "F", ST7735_GRAY, ST7735_BLACK);
    fmt_f3(buf, fp);
    ST7735_DrawString(11, 56, buf, ST7735_GREEN, ST7735_BLACK);

    ST7735_DrawString(0,  70, "T", ST7735_GRAY, ST7735_BLACK);
    snprintf(buf, sizeof(buf), "%6lu", (unsigned long)(uptime_s % 999999u));
    ST7735_DrawString(11, 70, buf, ST7735_WHITE, ST7735_BLACK);

    ST7735_DrawString(0,  84, "#", ST7735_GRAY, ST7735_BLACK);
    snprintf(buf, sizeof(buf), "%6lu", (unsigned long)(s_frame % 999999u));
    ST7735_DrawString(11, 84, buf, ST7735_ORANGE, ST7735_BLACK);

    /* ── Raw ADC (calibração / debug) ── */
    ST7735_DrawString(0,  98, "v", ST7735_GRAY, ST7735_BLACK);
    snprintf(buf, sizeof(buf), "%6lu", (unsigned long)adc_mean_v);
    ST7735_DrawString(11, 98, buf, ST7735_CYAN, ST7735_BLACK);

    ST7735_DrawString(0, 112, "i", ST7735_GRAY, ST7735_BLACK);
    snprintf(buf, sizeof(buf), "%6lu", (unsigned long)adc_mean_i);
    ST7735_DrawString(11, 112, buf, ST7735_CYAN, ST7735_BLACK);

    ST7735_DrawString(0, 126, "a", ST7735_GRAY, ST7735_BLACK);
    snprintf(buf, sizeof(buf), "%6lu", (unsigned long)adc_pp_v);
    ST7735_DrawString(11, 126, buf, ST7735_WHITE, ST7735_BLACK);

    ST7735_DrawString(0, 140, "b", ST7735_GRAY, ST7735_BLACK);
    snprintf(buf, sizeof(buf), "%6lu", (unsigned long)adc_pp_i);
    ST7735_DrawString(11, 140, buf, ST7735_WHITE, ST7735_BLACK);
}