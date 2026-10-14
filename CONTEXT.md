# Firmware de telemetria V-GUARD

O nó de aquisição do V-GUARD: lê a IMU e o OBD-II do carro e entrega amostras à Raspberry Pi, que cuida de eventos, armazenamento e envio ao backend.

## Language

### Comunicação

**Quadro CAN**:
Uma mensagem no barramento CAN do carro.
_Avoid_: frame, pacote CAN

**Requisição OBD**:
Um quadro CAN padronizado (modo 01) que pede o valor de um PID ao carro. É a única coisa que o firmware transmite no barramento.
_Avoid_: comando, mensagem CAN

**PID**:
O identificador de um parâmetro do carro no OBD-II, como RPM ou velocidade.
_Avoid_: parâmetro OBD, sensor do carro

**Mensagem**:
A unidade de comunicação entre o ESP32 e a Pi pelo cabo.
_Avoid_: quadro, pacote, frame

**Boot**:
O período entre o ESP32 ligar, ou reiniciar, e desligar. O relógio do ESP32 recomeça a cada boot, então um horário só tem sentido dentro do seu boot.
_Avoid_: sessão, execução

**STATUS**:
A mensagem que o ESP32 envia no boot e depois 1 vez por segundo, com sua identificação, a configuração em uso e os contadores. Também serve de confirmação dos comandos da Pi.
_Avoid_: HELLO, heartbeat, ACK

**Taxa de envio**:
Quantas amostras de uma fonte o ESP32 entrega à Pi por segundo. Quem escolhe é a Pi.
_Avoid_: frequência de amostragem (que é a das leituras da IMU)

**Decodificador**:
O código Python, entregue pela equipe do firmware, que transforma as mensagens recebidas em amostras com o horário da Pi e prepara as mensagens que a Pi envia. Não abre a porta serial.
_Avoid_: parser, driver

**Leitor da UART**:
O código da Pi, da Isabela, que abre a porta serial, passa os bytes ao decodificador e entrega as amostras ao gerenciador de eventos.
_Avoid_: decodificador (para o leitor)

### Dados

**Fonte**:
A origem das amostras no ESP32: a IMU ou o OBD.
_Avoid_: sensor (para o OBD), canal

**Leitura**:
Uma medição bruta da IMU, interna ao ESP32. Várias leituras formam uma amostra de IMU.
_Avoid_: amostra (para a medição bruta)

**Amostra**:
O que o ESP32 entrega à Pi: o estado de uma fonte num instante, com seu horário.
_Avoid_: telemetria (para uma única amostra), pacote, registro

**Amostra de IMU**:
A média e o pico de cada eixo de aceleração e rotação das leituras de um intervalo, no referencial do carro.

**Pico**:
A leitura de maior módulo de um eixo no intervalo de uma amostra de IMU, com o sinal mantido.
_Avoid_: máximo (que perde o sinal)

**Referencial do carro**:
Os eixos fixos no carro: x para a frente, y para a esquerda e z para cima. As amostras de IMU sempre usam este referencial, com a gravidade incluída.
_Avoid_: referencial do sensor (para as amostras)

**Amostra OBD**:
O conjunto completo dos valores OBD disponíveis num instante, cada um com sua idade. Nunca é um valor isolado.
_Avoid_: leitura de PID

**Idade**:
Há quanto tempo o carro respondeu um valor da amostra OBD, contado até o instante da amostra.
_Avoid_: atraso, latência

**Telemetria consolidada**:
A linha por segundo que a Pi grava a partir das amostras. Não é produzida pelo firmware.

**Janela do evento**:
As amostras dos segundos antes e depois de um evento, preservadas pela Pi.

### Eventos

**Evento**:
O registro que a Pi cria quando a visão detecta fadiga ou distração. O firmware nunca produz eventos.
_Avoid_: alerta, ocorrência

**Detecção**:
Uma condição percebida pelo próprio firmware, como uma frenagem brusca (extensão futura). A Pi decide se ela vira evento.
_Avoid_: evento (no firmware)

### Simulação

**Simulação**:
O substituto temporário das fontes reais dentro do firmware, usado só no desenvolvimento. Nunca convive com as fontes reais no mesmo projeto.
_Avoid_: modo de teste, emulação

**Simulador do PC**:
O programa no notebook que define em tempo real o estado do carro e da IMU durante o desenvolvimento e mostra os resultados. Permanece no projeto.
_Avoid_: simulação (para o programa do PC)

**ECU simulada**:
O programa no notebook que responde às requisições OBD como se fosse o carro, no CAN virtual ou no barramento real. Permanece no projeto.
_Avoid_: simulador OBD, ECU falsa

**Adaptador USB-CAN**:
O segundo ESP32, que liga o notebook ao barramento CAN real na bancada.
_Avoid_: ECU simulada (para a placa)
