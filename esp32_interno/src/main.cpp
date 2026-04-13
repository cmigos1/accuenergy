#include "calculo.h"
#include "envio.h"
#include <Arduino.h>
#include <cstring>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <esp_task_wdt.h>

/* ==========================================================================
 *  Constantes de Amostragem
 *
 *  Frequência alvo: 7200 Hz (= 120 amostras por ciclo de 60 Hz)
 *  Período por par V+I: 1 / 7200 = 138.9 µs
 *
 *  Com adc1_get_raw() ~10 µs × 2 canais = ~20 µs para ler o par.
 *  Delay restante = 138.9 − 20 ≈ 119 µs → usamos delayMicroseconds(119).
 *
 *  600 amostras = exatos 5 ciclos de 60 Hz (83,3 ms por janela).
 *
 *  SEND_INTERVAL_MS = 100 ms → protocolo binário envia ~9-10 frames/s.
 *  (O texto anterior a 1000 ms = 1 frame/s era o principal gargalo de UI.)
 * ========================================================================== */
static const int   NUM_SAMPLES      = 600;
static const int   TARGET_RATE_HZ   = 7200;
static const float SAMPLE_PERIOD_US = 1000000.0f / TARGET_RATE_HZ;  // ~138.9 µs
static const unsigned long SEND_INTERVAL_MS     = 100;   // intervalo normal (ms)
static const unsigned long SEND_INTERVAL_CAL_MS = 200;   // intervalo calibração (ms)

static const int ADC_READ_PAIR_US = 20;
static const int DELAY_US = (int)(SAMPLE_PERIOD_US - ADC_READ_PAIR_US);

/* Canais ADC1 (GPIOs 32–39 são ADC1, seguros com WiFi) */
static const adc1_channel_t V_CHANNEL = ADC1_CHANNEL_6;  // GPIO 34
static const adc1_channel_t I_CHANNEL = ADC1_CHANNEL_7;  // GPIO 35

static esp_adc_cal_characteristics_t adc_chars;

/* ==========================================================================
 *  Modo de Operação
 *
 *  Controlado via comandos seriais (enviados pela GUI):
 *    "NORMAL\n"  → medição normal, envia frames DATA binários
 *    "CAL_POT\n" → modo calibração potenciômetro, envia frames CALPOT
 *    "CAL_PIN\n" → modo calibração pino-a-pino, envia frames CALPIN
 *
 *  g_mode é escrito por loop() (Core 1) e lido por taskCalculoEnvio (Core 0).
 *  Um enum de 4 bytes tem leitura/escrita atômica em Xtensa de 32 bits;
 *  volatile evita que o compilador cache o valor em registrador.
 * ========================================================================== */
enum class Mode { NORMAL, CAL_POT, CAL_PIN };
static volatile Mode g_mode = Mode::NORMAL;

/* ==========================================================================
 *  Buffers Ping-Pong
 * ========================================================================== */
typedef struct {
    float V_Raw[NUM_SAMPLES];
    float I_Raw[NUM_SAMPLES];
    int   count;
} SampleBuffer;

static SampleBuffer bufferA;
static SampleBuffer bufferB;

static volatile SampleBuffer *writeBuffer = &bufferA;
static volatile SampleBuffer *readBuffer  = &bufferB;

/* ==========================================================================
 *  Sincronização FreeRTOS
 * ========================================================================== */
static SemaphoreHandle_t dataReadySemaphore = NULL;
static portMUX_TYPE      bufferMux          = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t resultMutex        = NULL;

static float g_V_RMS      = 0.0f;
static float g_I_RMS      = 0.0f;
static float g_pf         = 0.0f;
static float g_p_active   = 0.0f;
static float g_p_reactive = 0.0f;
static float g_p_apparent = 0.0f;

static float g_V_Send[NUM_SAMPLES];
static float g_I_Send[NUM_SAMPLES];
static int   g_sendCount = 0;

/* ==========================================================================
 *  Task de Amostragem — Core 1, prioridade alta
 * ========================================================================== */
void taskAmostragem(void *pvParameters) {
    (void)pvParameters;
    esp_task_wdt_delete(NULL);

    while (true) {
        volatile SampleBuffer *buf = writeBuffer;

        for (int i = 0; i < NUM_SAMPLES; i++) {
            int64_t t0 = esp_timer_get_time();

            int raw_v = adc1_get_raw(V_CHANNEL);
            int raw_i = adc1_get_raw(I_CHANNEL);

            buf->V_Raw[i] = (float)esp_adc_cal_raw_to_voltage(raw_v, &adc_chars);
            buf->I_Raw[i] = (float)esp_adc_cal_raw_to_voltage(raw_i, &adc_chars);

            int64_t elapsed = esp_timer_get_time() - t0;
            int remaining = (int)(SAMPLE_PERIOD_US) - (int)elapsed;
            if (remaining > 0) delayMicroseconds(remaining);
        }
        buf->count = NUM_SAMPLES;

        portENTER_CRITICAL(&bufferMux);
        volatile SampleBuffer *temp = writeBuffer;
        writeBuffer = readBuffer;
        readBuffer  = temp;
        portEXIT_CRITICAL(&bufferMux);

        xSemaphoreGive(dataReadySemaphore);
        vTaskDelay(1);
    }
}

/* ==========================================================================
 *  Task de Cálculo + Envio — Core 0
 *
 *  Modo NORMAL: compensação de fase → calculoRMS → frame DATA binário.
 *
 *  Modo CAL_POT / CAL_PIN: calcula estatísticas no domínio raw (mV),
 *  sem aplicar fatores de sensor. Útil para verificar:
 *    - offset DC dos canais (~1650 mV = VCC/2 esperado)
 *    - faixa linear do ADC (150–2450 mV com ATTEN_DB_11)
 *    - diferença de ganho entre canais (CAL_PIN, ratio esperado = 1,0)
 * ========================================================================== */
void taskCalculoEnvio(void *pvParameters) {
    (void)pvParameters;
    unsigned long lastSendTime = 0;

    while (true) {
        if (xSemaphoreTake(dataReadySemaphore, portMAX_DELAY) != pdTRUE) continue;

        /* Cópia do buffer de leitura */
        float localV[NUM_SAMPLES];
        float localI[NUM_SAMPLES];
        int   localCount;

        portENTER_CRITICAL(&bufferMux);
        volatile SampleBuffer *buf = readBuffer;
        localCount = buf->count;
        for (int i = 0; i < localCount; i++) {
            localV[i] = buf->V_Raw[i];
            localI[i] = buf->I_Raw[i];
        }
        portEXIT_CRITICAL(&bufferMux);

        Mode currentMode = g_mode;

        /* ── Modos de Calibração ─────────────────────────────────────────── */
        if (currentMode != Mode::NORMAL) {
            double sv = 0.0, si = 0.0;
            float  vmin = localV[0], vmax = localV[0];
            float  imin = localI[0], imax = localI[0];

            for (int k = 0; k < localCount; k++) {
                sv += localV[k]; si += localI[k];
                if (localV[k] < vmin) vmin = localV[k];
                if (localV[k] > vmax) vmax = localV[k];
                if (localI[k] < imin) imin = localI[k];
                if (localI[k] > imax) imax = localI[k];
            }

            float v_dc = (float)(sv / localCount);
            float i_dc = (float)(si / localCount);

            double vac2 = 0.0, iac2 = 0.0;
            for (int k = 0; k < localCount; k++) {
                float dv = localV[k] - v_dc;
                float di = localI[k] - i_dc;
                vac2 += dv * dv;
                iac2 += di * di;
            }
            float v_ac_rms = sqrtf((float)(vac2 / localCount));
            float i_ac_rms = sqrtf((float)(iac2 / localCount));

            unsigned long now = millis();
            if (now - lastSendTime >= SEND_INTERVAL_CAL_MS) {
                lastSendTime = now;
                if (currentMode == Mode::CAL_POT) {
                    enviarCalPot(v_dc, vmax - vmin, v_ac_rms,
                                 i_dc, imax - imin, i_ac_rms);
                } else {  /* CAL_PIN */
                    enviarCalPin(v_dc, i_dc, v_ac_rms, i_ac_rms);
                }
            }
            continue;  /* Não calcula RMS nem envia DATA neste ciclo */
        }

        /* ── Modo Normal: compensação de fase + cálculo RMS ─────────────── */

        /* Compensação de fase V↔I
         * I[k] é lido Δt ≈ 10 µs depois de V[k]. Interpolação linear
         * "recua" I no tempo de Δt para alinhar com V. */
        float localI_comp[NUM_SAMPLES];
        const float phase_ratio = (float)ADC_READ_PAIR_US / SAMPLE_PERIOD_US;

        for (int i = 0; i < localCount - 1; i++) {
            localI_comp[i] = localI[i] - (localI[i + 1] - localI[i]) * phase_ratio;
        }
        if (localCount > 1) {
            int last = localCount - 1;
            localI_comp[last] = localI[last] - (localI[last] - localI[last - 1]) * phase_ratio;
        } else {
            localI_comp[0] = localI[0];
        }

        float v_rms, i_rms, pf, p_active, p_reactive, p_apparent;
        calculoRMS(localV, localI_comp, localCount,
                   &v_rms, &i_rms, &pf, &p_active, &p_reactive, &p_apparent);

        if (xSemaphoreTake(resultMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_V_RMS      = v_rms;
            g_I_RMS      = i_rms;
            g_pf         = pf;
            g_p_active   = p_active;
            g_p_reactive = p_reactive;
            g_p_apparent = p_apparent;
            g_sendCount  = localCount;
            memcpy(g_V_Send, localV, localCount * sizeof(float));
            memcpy(g_I_Send, localI, localCount * sizeof(float));
            xSemaphoreGive(resultMutex);
        }

        unsigned long now = millis();
        if (now - lastSendTime >= SEND_INTERVAL_MS) {
            lastSendTime = now;
            if (xSemaphoreTake(resultMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                enviarDados(g_V_RMS, g_I_RMS, g_pf,
                            g_p_active, g_p_reactive, g_p_apparent,
                            g_V_Send, g_I_Send, g_sendCount);
                xSemaphoreGive(resultMutex);
            }
        }
    }
}

/* ==========================================================================
 *  Setup
 * ========================================================================== */
void setup() {
    Serial.begin(230400);
    delay(500);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(V_CHANNEL, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(I_CHANNEL, ADC_ATTEN_DB_11);

    esp_adc_cal_value_t cal_type = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);

    Serial.print("ADC calibracao: ");
    switch (cal_type) {
        case ESP_ADC_CAL_VAL_EFUSE_VREF:  Serial.println("eFuse Vref");      break;
        case ESP_ADC_CAL_VAL_EFUSE_TP:    Serial.println("eFuse Two-Point"); break;
        default:                          Serial.println("Valor padrao");     break;
    }

    Serial.printf("Amostragem: %d amostras a %d Hz (%.1f ms por janela)\n",
                  NUM_SAMPLES, TARGET_RATE_HZ,
                  (float)NUM_SAMPLES / TARGET_RATE_HZ * 1000.0f);
    Serial.println("Comandos: NORMAL | CAL_POT | CAL_PIN");

    dataReadySemaphore = xSemaphoreCreateBinary();
    resultMutex        = xSemaphoreCreateMutex();

    memset((void *)&bufferA, 0, sizeof(SampleBuffer));
    memset((void *)&bufferB, 0, sizeof(SampleBuffer));

    xTaskCreatePinnedToCore(taskCalculoEnvio, "CalculoEnvio", 16384, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(taskAmostragem,   "Amostragem",   8192,  NULL, 3, NULL, 1);
}

/* ==========================================================================
 *  Loop — leitura de comandos seriais para controle de modo
 *
 *  Comandos esperados (terminados em '\n'):
 *    NORMAL   → volta a enviar frames DATA binários
 *    CAL_POT  → envia frames CALPOT com estatísticas raw dos canais
 *    CAL_PIN  → envia frames CALPIN para calibração pino-a-pino
 * ========================================================================== */
void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd == "NORMAL") {
            g_mode = Mode::NORMAL;
            Serial.println("ACK:NORMAL");
        } else if (cmd == "CAL_POT") {
            g_mode = Mode::CAL_POT;
            Serial.println("ACK:CAL_POT");
        } else if (cmd == "CAL_PIN") {
            g_mode = Mode::CAL_PIN;
            Serial.println("ACK:CAL_PIN");
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
