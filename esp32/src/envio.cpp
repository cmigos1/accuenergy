#include "envio.h"
void enviarDados(float V_RMS, float I_RMS, float *V_Raw, float *I_Raw, int numSamples) {
  // Envia um cabeçalho identificador do pacote
  Serial1.print("START,");
  Serial1.print(V_RMS);
  Serial1.print(",");
  Serial1.print(I_RMS);
  Serial1.print(",");
  Serial1.println(numSamples);

  // Envia os dados amostrados (Tensão,Corrente)
  for (int i = 0; i < numSamples; i++) {
    Serial1.print(V_Raw[i]);
    Serial1.print(",");
    Serial1.println(I_Raw[i]);
  }
  
  // Marca o fim do pacote
  Serial1.println("END");
}