#if !defined(ENVIO_H)
#define ENVIO_H
#include <Arduino.h>
void enviarDados(float V_RMS, float I_RMS, float *V_Raw, float *I_Raw, int numSamples);
#endif