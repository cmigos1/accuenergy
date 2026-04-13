# Projeto Accuenergy - Monitoramento de Carga Não Intrusivo (NILM)

Este projeto tem como objetivo desenvolver um sistema de monitoramento de energia e identificação de cargas elétricas baseado no conceito de NILM (Non-Intrusive Load Monitoring). 

A arquitetura do sistema é dividida em duas partes principais, rodando em microcontroladores distintos que se comunicam via SPI:

## 1. STM32H743VIT6 (Amostrador / Processamento)
Localizado na pasta `STM32/AccuenergyAmostrador/`, este é um projeto baseado em CMake e construído com STM32CubeMX.
- **Função:** Atua como o cérebro de aquisição de dados do sistema.
- **Aquisição de Sinais (ADC):** Faz a leitura em alta velocidade dos sensores de tensão e corrente. O projeto utiliza circuitos condicionadores para adaptar os sinais (como o módulo baseado no transformador de tensão ZMPT101B com amplificador operacional LM358).
- **Processamento (NILM):** Realiza o processamento digital dos sinais amostrados para identificar padrões de consumo.
- **Comunicação:** Atua como SPI Master para enviar as informações amostradas ao módulo de conectividade.

## 2. ESP32 (Conectividade e Servidor Web)
Localizado na pasta `ESP32/accuenergy/`, este é um projeto desenvolvido com a framework Arduino via PlatformIO.
- **Função:** Interface de conectividade e integração do usuário.
- **Comunicação SPI (SPI Slave):** Recebe os dados amostrados/processados enviados pelo STM32.
- **Servidor Web:** Exibe os dados de energia, status do monitoramento e provê uma interface de configuração/calibração acessível via rede Wi-Fi.
- **Calibração:** Gerencia e armazena os parâmetros de calibração do sistema para garantir a precisão das leituras de tensão e corrente.

## Estrutura do Repositório
- `STM32/`: Contém os projetos relacionados ao microcontrolador STM32 (Amostrador de dados).
- `ESP32/`: Contém o firmware do ESP32 para conectividade e interface web.
- `docs/`: Documentações de hardware, datasheets dos microcontroladores, sensores e arquivos de design.
