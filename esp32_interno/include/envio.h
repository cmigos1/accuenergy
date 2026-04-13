#if !defined(ENVIO_H)
#define ENVIO_H
#include <Arduino.h>

/* Número máximo de amostras decimadas enviadas por frame DATA. */
#define MAX_SEND_SAMPLES 120

/* ==========================================================================
 *  Protocolo Binário
 *
 *  Todos os frames: [MAGIC 2B][TYPE 1B][payload...][FOOTER 2B]
 *  Byte order: little-endian (nativo ESP32 / x86).
 *
 *  DATA (0x01):
 *    [magic 2][type 1][v_rms 4][i_rms 4][pf 4][p_act 4][p_rea 4][p_app 4]
 *    [n_samples 1][step 1][V0 int16][I0 int16]...[footer 2]
 *    Tamanho: 31 + n_samples × 4 bytes
 *
 *  CALPOT (0x02):
 *    [magic 2][type 1][v_dc 4][v_pp 4][v_ac_rms 4][i_dc 4][i_pp 4][i_ac_rms 4][footer 2]
 *    Tamanho: 29 bytes
 *
 *  CALPIN (0x03):
 *    [magic 2][type 1][v_dc 4][i_dc 4][v_ac_rms 4][i_ac_rms 4][footer 2]
 *    Tamanho: 21 bytes
 * ========================================================================== */
#define FRAME_MAGIC_0   0xAB
#define FRAME_MAGIC_1   0xCD
#define FRAME_FOOTER_0  0xEF
#define FRAME_FOOTER_1  0xFE
#define FRAME_TYPE_DATA    0x01
#define FRAME_TYPE_CALPOT  0x02
#define FRAME_TYPE_CALPIN  0x03

void enviarDados(float V_RMS, float I_RMS, float pf,
                 float p_active, float p_reactive, float p_apparent,
                 float *V_Raw, float *I_Raw, int numSamples);

/* Calibração por potenciômetro: DC offset, pico-a-pico e AC-RMS em mV brutos */
void enviarCalPot(float v_dc, float v_pp, float v_ac_rms,
                  float i_dc, float i_pp, float i_ac_rms);

/* Calibração pino-a-pino: ambos canais no mesmo sinal */
void enviarCalPin(float v_dc, float i_dc, float v_ac_rms, float i_ac_rms);

#endif /* ENVIO_H */
