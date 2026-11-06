# Voyage: CAN + acelerometro + microSD

Firmware separado do painel web. Nao inicia Wi-Fi nem servidor.

## O que registra

- OBD-II: RPM (PID 0C), velocidade (0D), temperatura do arrefecimento (05),
  tempo desde a partida do motor (1F), correcao curta de combustivel banco 1
  (06) e carga calculada do motor (04).
- ADXL345: aceleracao X/Y/Z em g, alvo de 50 amostras/s.
- Os PIDs OBD sao pedidos um por vez, em rodizio. O intervalo e 80 ms; seis
  respostas em menos de 80 ms cada permitem aproximadamente 2 rodadas/s.
  Se a ECU responder mais devagar, a taxa CAN fica menor. PID nao anunciado
  pela ECU e ignorado.
- Sao criados dois CSV com o mesmo numero: `CAR0001.CSV` para snapshots OBD e
  `ACC0001.CSV` para aceleracao. Ambos usam `timestamp_ms`, milissegundos desde
  que a ESP32 ligou, para alinhar os dados. Temperatura esta em C, tempo em s,
  correcao/carga em %, velocidade em km/h e aceleracao em g.
- O ADXL345 mede tambem a gravidade. Parado e nivelado, o eixo alinhado a
  gravidade deve indicar perto de 1 g. O firmware nao remove gravidade nem
  corrige a orientacao do sensor.

## Arduino IDE

Abra `voyage_logger_sd.ino`. Mantenha `obd_core.h` e `obd_checks.h` ao lado.
Selecione `ESP32 Dev Module`, a biblioteca `mcp_can` de coryjfowler, e grave.
Monitor serial: 115200 baud. O cartao microSD deve estar formatado em FAT32.

## Ligacoes

Todos os GND devem ser comuns. O MCP2515 continua em SPI e compartilha SCK,
MISO e MOSI com o cartao SD; cada modulo tem um CS separado:

| Sinal | ESP32 DOIT DevKit V1 |
|---|---:|
| SPI SCK (MCP2515 e SD) | GPIO18 |
| SPI MISO / SO (MCP2515 e SD) | GPIO19 |
| SPI MOSI / SI (MCP2515 e SD) | GPIO23 |
| CS do MCP2515 | GPIO25 |
| CS do microSD | GPIO26 |
| SDA do ADXL345 (I2C) | GPIO21 |
| SCL do ADXL345 (I2C) | GPIO22 |

**Mude o fio CS do MCP2515 do GPIO21 para GPIO25.** O GPIO21 passa a ser SDA.
O CS do microSD vai ao GPIO26; nao una os dois CS.

Para o ADXL345 em I2C, CS deve ficar em nivel alto (3,3 V) e SDO em GND,
selecionando o endereco 0x53. INT1 e INT2 podem ficar desconectados. A conexao
dos pinos de alimentacao `VS` e `3V3` depende da placa ADXL345: confirme qual
e entrada e qual e saida no verso/esquema do seu modulo antes de ligar. Nunca
aplique 5 V aos sinais do ADXL345 ou aos GPIOs do ESP32.

Ligue o microSD conforme a identificacao do proprio modulo; esta versao espera
que o modulo funcione em SPI com logica de 3,3 V. Nao una fontes de 5 V. O cartao
e inicializado a 10 MHz no SPI.

No conector OBD2: pino 6 CAN_H, 14 CAN_L e 5 GND. Deixe o pino 16 isolado e
remova o jumper de terminacao J1 de 120 ohms do MCP2515 ao conectar ao carro.
O MCP2515/TJA1050 continua precisando da mesma conversao/protecao de nivel
logico SPI usada no teste anterior.

## Limites da verificacao

O codigo foi baseado no leitor que ja confirmou 0x7E8 no Voyage. Ainda precisa
ser compilado na Arduino IDE e testado com o ADXL345 e cartao reais. A velocidade
de 2 rodadas OBD/s depende da latencia da ECU; a aquisicao do acelerometro fica
em tarefa FreeRTOS separada para continuar amostrando durante escritas no SD.
