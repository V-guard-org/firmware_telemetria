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

**Central**:
Um módulo eletrônico do carro que responde às requisições OBD, como a central do motor.
_Avoid_: ECU (reservado para a ECU simulada), módulo

**Descoberta**:
A etapa em que o ESP32 pergunta às centrais quais PIDs elas suportam e escolhe a central que vai responder a cada PID.
_Avoid_: varredura, scan

**Lista fechada**:
O conjunto fixo, no código, dos únicos quadros CAN que o firmware pode transmitir: requisições OBD do modo 01 para os endereços padronizados.
_Avoid_: whitelist, filtro

**Modo silencioso**:
O estado do transceptor CAN em que ele escuta, mas não consegue transmitir. É o estado padrão, garantido por um resistor, e o firmware só o libera durante a descoberta e a leitura.
_Avoid_: standby, listen-only (que é o modo do controlador)

**Bloqueio**:
O estado em que o firmware deixa de transmitir no CAN até reiniciar, depois de falhas repetidas ou de uma violação da lista fechada.
_Avoid_: travamento, desligamento

**Presença da Pi**:
A Pi está presente se mandou um PING nos últimos 3 s. Sem ela, o firmware não pergunta nada ao carro.
_Avoid_: conexão, sessão

**Mensagem**:
A unidade de comunicação entre o ESP32 e a Pi pelo cabo.
_Avoid_: quadro, pacote, frame

**Boot**:
O período entre o ESP32 ligar, ou reiniciar, e desligar. O relógio do ESP32 recomeça a cada boot, então um horário só tem sentido dentro do seu boot.
_Avoid_: sessão, execução

**STATUS**:
A mensagem que o ESP32 envia no boot e depois 1 vez por segundo, com sua identificação, a configuração em uso e os contadores. Também serve de confirmação dos comandos da Pi.
_Avoid_: HELLO, heartbeat, ACK

**Par de horários**:
O instante da Pi no meio de um PING e do seu PONG, junto com o `t_us` que o ESP32 pôs no PONG. O decodificador ajusta a conversão de horário por esses pares.
_Avoid_: amostra de tempo, medição de relógio

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

**Amostra vazia**:
Uma amostra sem nenhum valor, que o decodificador entrega uma vez quando uma fonte para, para que a Pi não trate a última amostra como atual.
_Avoid_: amostra nula, amostra zerada

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

### Código do firmware

**Núcleo**:
O código portátil do firmware, que roda igual no notebook, no ESP32 e nos testes. Nunca espera, nunca cria tarefas e nunca pergunta a hora: recebe eventos com o instante e age pelas portas.
_Avoid_: lógica, aplicação

**Camada de plataforma**:
O código que muda entre o notebook e o ESP32: o laço, as filas, os drivers e o relógio. Entrega os eventos ao núcleo e implementa as portas.
_Avoid_: HAL, driver (para a camada inteira)

**Porta**:
Uma interface pela qual o núcleo age: CAN, UART, IMU ou sistema. Cada porta tem um adaptador por plataforma e um falso para os testes.
_Avoid_: porta serial (que é sempre chamada assim), HAL

**Portão**:
O módulo do núcleo que confere cada quadro CAN contra a lista fechada antes de entregá-lo à porta CAN. É a única passagem do OBD até ela.
_Avoid_: filtro (reservado ao filtro de recepção do controlador), firewall

**Resumo da escuta**:
O que a camada de plataforma conta durante a escuta: quadros válidos, transições no pino RX e o maior trecho dominante. O núcleo decide o próximo estado do OBD por ele.

**Mensagem de desenvolvimento**:
Uma mensagem que só existe enquanto houver simulação no firmware, como a IMU_SIM. Sai do protocolo junto com a simulação.
_Avoid_: mensagem de teste, comando de debug

### Simulação

**Simulação**:
O substituto temporário das fontes reais dentro do firmware, usado só no desenvolvimento. Nunca convive com as fontes reais no mesmo projeto.
_Avoid_: modo de teste, emulação

**Simulador do PC**:
O programa no notebook que faz o papel do carro e da Pi durante o desenvolvimento: define ao vivo o estado do carro e da IMU, roda roteiros e mostra o que chega do firmware. Permanece no projeto.
_Avoid_: simulação (para o programa do PC), dashboard

**Modelo do carro**:
A parte do simulador do PC que calcula, a partir do acelerador, do freio, do volante e da ignição, a velocidade, a rotação, os valores OBD e as leituras da IMU, todos coerentes entre si.
_Avoid_: física, simulação

**Manobra**:
Uma ação curta que o simulador do PC aplica ao carro: buraco, lombada, frenagem brusca, curva ou troca de faixa.
_Avoid_: evento (reservado à Pi), evento de direção, ocorrência

**Roteiro**:
Um arquivo de texto com ações do simulador do PC em horários fixos, uma por linha, com os mesmos comandos dos botões da tela.
_Avoid_: script, cenário (reservado aos cenários de teste das máquinas de estado)

**ECU simulada**:
A parte do simulador do PC que responde às requisições OBD como se fosse o carro, no CAN virtual ou no barramento real. Permanece no projeto.
_Avoid_: simulador OBD, ECU falsa

**ESP32 falso**:
O programa Python que imita o ESP32 no cabo, com amostras inventadas, para testar o leitor da UART e o decodificador sem placa e sem firmware. Permanece no projeto.
_Avoid_: ESP32 simulado, simulador (para ele)

**Adaptador USB-CAN**:
O segundo ESP32, que liga o notebook ao barramento CAN real na bancada.
_Avoid_: ECU simulada (para a placa)
