#include <Arduino.h>
#include <Adafruit_ADS1x15.h>
#include "calculo.h"
#include "envio.h"

const unsigned long interval = 1000; // 1 segundo de intervalo (não trava o loop)
const int numSamples = 100; // 100 amostras duram aprox 230ms no ADS1115
unsigned long previousMillis = 0;
unsigned long now = 0;

float V_RMS = 0;
float I_RMS = 0;

// Arrays para armazenar as amostras de tensão e corrente
typedef struct {
  float V_Raw[numSamples];
  float I_Raw[numSamples];
} Samples;

Samples samples; // Estrutura para armazenar as amostras de tensão e corrente
Adafruit_ADS1115 ads; // Instância do sensor ADS1115

const int tensaoPin = 0;   // Canal 0 do ADS1115 para tensão
const int correntePin = 1; // Canal 1 do ADS1115 para corrente 

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200); // Porta serial para envio dos dados
  ads.begin(); // Inicializa o ADS1115
  ads.setGain(GAIN_ONE); // Configura o ganho para 1x (±4.096V)
  ads.setDataRate(RATE_ADS1115_860SPS); // Configura a taxa de amostragem para 860 amostras por segundo
}

void loop() {
  now = millis();
  if (now - previousMillis >= interval) {
    previousMillis = now;
    for (int i = 0; i < numSamples; i++) {
      samples.V_Raw[i] = ads.readADC_SingleEnded(tensaoPin);
      samples.I_Raw[i] = ads.readADC_SingleEnded(correntePin);
    }
  }
  calculoRMS(samples.V_Raw, samples.I_Raw, numSamples, &V_RMS, &I_RMS);
  enviarDados(V_RMS, I_RMS, samples.V_Raw, samples.I_Raw, numSamples);
}


