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

/* ── Retângulo preenchido ───────────────────────────────────────────────── */
static void fill_rect(uint8_t x0, uint8_t y0, uint8_t w, uint8_t h, uint16_t color)
{
    if (x0 >= ST7735_W || y0 >= ST7735_H || w == 0u || h == 0u) return;
    if ((uint16_t)x0 + w > ST7735_W) w = (uint8_t)(ST7735_W - x0);
    if ((uint16_t)y0 + h > ST7735_H) h = (uint8_t)(ST7735_H - y0);
    set_addr_window(x0, y0, (uint8_t)(x0 + w - 1u), (uint8_t)(y0 + h - 1u));
    DC_H(); CS_L();
    uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)(color & 0xFFu);
    for (uint32_t i = 0u; i < (uint32_t)w * h; i++) {
        spi_write_byte(hi); spi_write_byte(lo);
    }
    CS_H();
}

/* ── Caractere 1× nativo (5×7 px + 1 col gap à direita = 6×7 px) ───────── */
static void draw_char1x(uint8_t x, uint8_t y, char c, uint16_t fg, uint16_t bg)
{
    if ((uint8_t)c < 0x20u || (uint8_t)c > 0x7Eu) c = '?';
    const uint8_t *glyph = font5x7[(uint8_t)(c - 0x20u)];
    /* Janela de 6×7: 5 colunas de glifo + 1 coluna de espaço */
    set_addr_window(x, y, (uint8_t)(x + 5u), (uint8_t)(y + 6u));
    DC_H(); CS_L();
    uint8_t fhi = (uint8_t)(fg >> 8), flo = (uint8_t)(fg & 0xFFu);
    uint8_t bhi = (uint8_t)(bg >> 8), blo = (uint8_t)(bg & 0xFFu);
    for (uint8_t row = 0u; row < 7u; row++) {
        for (uint8_t col = 0u; col < 5u; col++) {
            if (glyph[col] & (1u << row)) {
                spi_write_byte(fhi); spi_write_byte(flo);
            } else {
                spi_write_byte(bhi); spi_write_byte(blo);
            }
        }
        /* Coluna de espaço */
        spi_write_byte(bhi); spi_write_byte(blo);
    }
    CS_H();
}

/* ── String em fonte 1× (stride 6 px) ──────────────────────────────────── */
void ST7735_DrawString1x(uint8_t x, uint8_t y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s && (uint16_t)x + 6u <= ST7735_W) {
        draw_char1x(x, y, *s++, fg, bg);
        x = (uint8_t)(x + 6u);
    }
}

/* ── Formatador 5 chars sem %f ──────────────────────────────────────────── */
/* Produz exatamente 5 chars right-justified em out[6]. d = casas decimais (0-3). */
static void fmt5(char *out, float v, uint8_t d)
{
    if (v != v || v > 99999.0f || v < -9999.0f) {
        out[0]='-'; out[1]='-'; out[2]='-'; out[3]='-'; out[4]='-'; out[5]='\0';
        return;
    }
    uint8_t neg = (v < 0.0f) ? 1u : 0u;
    float   av  = neg ? -v : v;
    uint32_t scale = 1u;
    for (uint8_t i = 0u; i < d; i++) scale *= 10u;
    uint32_t iv = (uint32_t)(av * (float)scale + 0.5f);
    uint32_t ip = iv / scale;
    uint32_t dp_ = iv % scale;
    char tmp[12];
    uint8_t len;
    if (d == 0u)
        len = (uint8_t)snprintf(tmp, sizeof(tmp), "%s%lu",     neg?"-":"", (unsigned long)ip);
    else if (d == 1u)
        len = (uint8_t)snprintf(tmp, sizeof(tmp), "%s%lu.%lu", neg?"-":"", (unsigned long)ip, (unsigned long)dp_);
    else if (d == 2u)
        len = (uint8_t)snprintf(tmp, sizeof(tmp), "%s%lu.%02lu",neg?"-":"", (unsigned long)ip, (unsigned long)dp_);
    else
        len = (uint8_t)snprintf(tmp, sizeof(tmp), "%s%lu.%03lu",neg?"-":"", (unsigned long)ip, (unsigned long)dp_);
    if (len >= 5u) {
        for (uint8_t i = 0u; i < 5u; i++) out[i] = tmp[i + (len - 5u)];
    } else {
        uint8_t pad = (uint8_t)(5u - len);
        for (uint8_t i = 0u; i < pad; i++) out[i] = ' ';
        for (uint8_t i = 0u; i < len; i++) out[pad + i] = tmp[i];
    }
    out[5] = '\0';
}

/* Auto-range decimal: tensão, corrente, potência, fator de potência */
static void fmtV (char *o, float v) { fmt5(o, v, 1u); }
static void fmtI (char *o, float v) { fmt5(o, v, v < 10.0f ? 3u : v < 100.0f ? 2u : 1u); }
static void fmtW (char *o, float v) { fmt5(o, v, v < 100.0f ? 1u : 0u); }
static void fmtFP(char *o, float v) { fmt5(o, v < 0.0f ? -v : v, 3u); }

/* ── Barra horizontal FP ────────────────────────────────────────────────── */
/* x=posição, y=topo, w=largura total, h=altura, val em [0,1], fg=cheio, bg=vazio */
static void draw_bar(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                     float val, uint16_t fg, uint16_t bg)
{
    uint8_t filled = (val > 0.0f && val <= 1.0f)
                     ? (uint8_t)(val * (float)w + 0.5f) : 0u;
    if (filled > w) filled = w;
    if (filled > 0u) fill_rect(x, y, filled, h, fg);
    if (filled < w)  fill_rect((uint8_t)(x + filled), y, (uint8_t)(w - filled), h, bg);
}

/* ── Painel principal bifásico ──────────────────────────────────────────── */
/*
 * Layout portrait 80×160, fonte 1× (6×7 px, stride 8 px vertical):
 *
 *  y=  0  [2×] "AccuEnergy"  (cyan)
 *  y= 14  ─── separador ───
 *  y= 15  "     F1    F2 "   (gray 1×)
 *  y= 23  "V  NNN.N NNN.N"   amarelo / cyan
 *  y= 31  "I  NN.NN NN.NN"
 *  y= 39  "P   NNNN  NNNN"   verde  / cyan
 *  y= 47  "Q   NNNN  NNNN"   laranja/ cyan
 *  y= 55  "S   NNNN  NNNN"   branco / cyan
 *  y= 63  "fp N.NNN N.NNN"   verde  / cyan
 *  y= 71  ─── separador ───
 *  y= 72  barra FP fase 1    (verde, 74×5 px)
 *  y= 78  barra FP fase 2    (cyan,  74×5 px)
 *  y= 84  ─── separador ───
 *  y= 85  "up NNNNN  #NNN"
 *  y= 93  (spare)
 *
 * Coluna F1: x=12   Coluna F2: x=48   (5 chars × 6 px = 30 px cada)
 */

static uint8_t s_panel_inited = 0u;

void ST7735_Main_Reset(void)
{
    s_panel_inited = 0u;
}

void ST7735_Main_Update(float v1, float i1, float p1, float q1, float s1, float fp1,
                         float v2, float i2, float p2, float q2, float s2, float fp2,
                         uint32_t uptime_s, uint32_t frame)
{
    char a[6], b[6];

    if (!s_panel_inited) {
        ST7735_FillScreen(ST7735_BLACK);
        /* Título 2× */
        ST7735_DrawString(10u, 0u, "AccuEnergy", ST7735_CYAN, ST7735_BLACK);
        /* Separadores horizontais */
        fill_rect(0u, 14u, 80u, 1u, ST7735_GRAY);
        fill_rect(0u, 71u, 80u, 1u, ST7735_GRAY);
        fill_rect(0u, 84u, 80u, 1u, ST7735_GRAY);
        /* Cabeçalhos de coluna */
        ST7735_DrawString1x(12u, 15u, " F1  ",    ST7735_YELLOW, ST7735_BLACK);
        ST7735_DrawString1x(48u, 15u, " F2  ",    ST7735_CYAN,   ST7735_BLACK);
        /* Labels de linha (estáticos) */
        ST7735_DrawString1x(0u, 23u, "V ", ST7735_GRAY, ST7735_BLACK);
        ST7735_DrawString1x(0u, 31u, "I ", ST7735_GRAY, ST7735_BLACK);
        ST7735_DrawString1x(0u, 39u, "P ", ST7735_GRAY, ST7735_BLACK);
        ST7735_DrawString1x(0u, 47u, "Q ", ST7735_GRAY, ST7735_BLACK);
        ST7735_DrawString1x(0u, 55u, "S ", ST7735_GRAY, ST7735_BLACK);
        ST7735_DrawString1x(0u, 63u, "fp", ST7735_GRAY, ST7735_BLACK);
        /* Labels de barra */
        ST7735_DrawString1x(0u, 72u, "1", ST7735_YELLOW, ST7735_BLACK);
        ST7735_DrawString1x(0u, 78u, "2", ST7735_CYAN,   ST7735_BLACK);
        /* Label uptime */
        ST7735_DrawString1x(0u, 85u, "up", ST7735_GRAY, ST7735_BLACK);
        ST7735_DrawString1x(0u, 93u, " #", ST7735_GRAY, ST7735_BLACK);
        s_panel_inited = 1u;
    }

    /* ── Valores fase 1 (amarelo/verde/laranja/branco) ── */
    fmtV (a, v1); ST7735_DrawString1x(12u, 23u, a, ST7735_YELLOW, ST7735_BLACK);
    fmtI (a, i1); ST7735_DrawString1x(12u, 31u, a, ST7735_YELLOW, ST7735_BLACK);
    fmtW (a, p1); ST7735_DrawString1x(12u, 39u, a, ST7735_GREEN,  ST7735_BLACK);
    fmtW (a, q1); ST7735_DrawString1x(12u, 47u, a, ST7735_ORANGE, ST7735_BLACK);
    fmtW (a, s1); ST7735_DrawString1x(12u, 55u, a, ST7735_WHITE,  ST7735_BLACK);
    fmtFP(a, fp1);ST7735_DrawString1x(12u, 63u, a, ST7735_GREEN,  ST7735_BLACK);

    /* ── Valores fase 2 (cyan) ── */
    fmtV (b, v2); ST7735_DrawString1x(48u, 23u, b, ST7735_CYAN, ST7735_BLACK);
    fmtI (b, i2); ST7735_DrawString1x(48u, 31u, b, ST7735_CYAN, ST7735_BLACK);
    fmtW (b, p2); ST7735_DrawString1x(48u, 39u, b, ST7735_CYAN, ST7735_BLACK);
    fmtW (b, q2); ST7735_DrawString1x(48u, 47u, b, ST7735_CYAN, ST7735_BLACK);
    fmtW (b, s2); ST7735_DrawString1x(48u, 55u, b, ST7735_CYAN, ST7735_BLACK);
    fmtFP(b, fp2);ST7735_DrawString1x(48u, 63u, b, ST7735_CYAN, ST7735_BLACK);

    /* ── Barras de fator de potência (x=6, w=74, h=5) ── */
    /* Verde escuro = 0x02C0, Cyan escuro = 0x0240 */
    draw_bar(6u, 72u, 74u, 5u, fp1 < 0.0f ? -fp1 : fp1, ST7735_GREEN,  0x02C0u);
    draw_bar(6u, 78u, 74u, 5u, fp2 < 0.0f ? -fp2 : fp2, ST7735_CYAN,   0x0240u);

    /* ── Uptime e frame ── */
    fmt5(a, (float)(uptime_s % 99999u), 0u);
    ST7735_DrawString1x(12u, 85u, a, ST7735_WHITE,  ST7735_BLACK);
    fmt5(b, (float)(frame     % 99999u), 0u);
    ST7735_DrawString1x(12u, 93u, b, ST7735_ORANGE, ST7735_BLACK);
}