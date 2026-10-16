# Do módulo CAN comprado, o firmware usa só o transceptor TJA1050

A equipe comprou 3 módulos MCP2515 + TJA1050 no lugar dos TJA1051T/3 que o [#4](https://github.com/V-guard-org/firmware_telemetria/issues/4) recomendava. O módulo traz dois chips: o controlador CAN MCP2515, que fala SPI, e o transceptor TJA1050. O firmware usa só o TJA1050, como transceptor do CAN interno do ESP32 (TWAI). O MCP2515 sai da placa.

A razão é a máquina do OBD ([`MAQUINAS-DE-ESTADO.md` §2.3](../MAQUINAS-DE-ESTADO.md)), que separa a falta de ACK do erro de barramento:
- **Sem ACK:** o carro está desligado, e o OBD pergunta de novo em 2 s.
- **Erro de bit, de forma ou de enchimento na descoberta:** o carro está em outra velocidade. O OBD vai a FALHA_CAN e se bloqueia na 3ª vez.

O TWAI informa o tipo do erro. O MCP2515 só acende o bit TXERR ("ocorreu um erro de barramento"), igual nos dois casos ([datasheet](https://ww1.microchip.com/downloads/en/DeviceDoc/MCP2515-Stand-Alone-CAN-Controller-with-SPI-20001801J.pdf), registrador 3-1). Com ele, uma das duas regras quebraria. Ou o ESP32 continuaria perguntando a um carro em outra velocidade, ou se bloquearia toda noite com o carro desligado.

O TJA1050 atende ao #4 ([datasheet](https://www.nxp.com/docs/en/data-sheet/TJA1050.pdf)). No modo silencioso, com o pino S em nível alto (a partir de 2,0 V), o transmissor desliga e o receptor continua. As entradas TXD e S aceitam 3,3 V.

## Opções consideradas

- **O MCP2515 pelo SPI, com conversor de nível:** não exige dessoldar nada. Em troca, pede um driver novo, um fio a mais para o contador de pulsos da escuta e um adaptador USB-CAN que fale com o MCP2515. E perde a diferença entre falta de ACK e erro de barramento.
- **Comprar os TJA1051T/3 mesmo assim:** é o plano do #4, mas depende do prazo de entrega e não faz nada que o TJA1050 não faça.

## Consequências

- **O firmware não muda.** O TWAI segue nos GPIO25 e GPIO26, e a trava no GPIO27.
- **Cada módulo é preparado à mão** ([`BANCADA.md` §9.1](../BANCADA.md)):
  - o MCP2515 sai da placa;
  - o pino S deixa o GND e vai ao GPIO27, com 10 kΩ para o 3,3 V;
  - o RXD passa por um divisor de 10 kΩ e 20 kΩ, porque o TJA1050 só funciona de 4,75 a 5,25 V e o RXD sai na tensão de alimentação.
- **O resistor do pino S não pode faltar.** Com o pino S solto, o TJA1050 fica em modo normal e transmite. É o resistor que garante o silêncio por padrão.
- **No carro, o jumper de 120 Ω do módulo sai,** porque o barramento do carro já tem terminação.
- **Os 3 módulos têm destino:** o carro, o adaptador (bancada e testemunha no carro) e a reserva.
