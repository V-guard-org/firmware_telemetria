# Barramento CAN de bancada sem transceptor entre dois ESP32

> Pesquisa do ticket #2 · 13–14/10/2026 · fase 2 da bancada ([ADR-0005](../adr/0005-codigo-testado-no-notebook-antes-das-placas.md))
> Escopo: ESP32 clássico (WROOM-32), controlador TWAI, 500 kbit/s, IDs de 11 bits, duas placas DevKit (ESP32 A com o firmware `vguard`, ESP32 B como adaptador USB-CAN).

## Pergunta

Dá para montar um barramento CAN de bancada entre os controladores TWAI de dois ESP32 clássicos **sem transceptor**? Ele funciona de forma confiável a 500 kbit/s?

Foram comparadas duas montagens:

- **(a)** diodos e um *pull-up* (wired-AND);
- **(b)** pino TX em dreno aberto (*open-drain*).

A resposta precisa cobrir:

- o esquema exato: tipo e sentido do diodo, valor do *pull-up* e pinos do ESP32;
- os limites: velocidade, comprimento do fio e número de nós;
- as armadilhas: eco do TX no RX, comportamento do ACK e estado dos pinos no boot;
- o que muda quando os transceptores chegarem.

## Resposta curta

**Sim.** O protocolo CAN não exige transceptor, e a ligação com um diodo por placa e um *pull-up* está documentada por dois fabricantes. A nota de aplicação da Keil/ARM mostra 500 kbit/s funcionando assim entre dois controladores.

Para o ESP32, os datasheets indicam folga a 500 kbit/s nestas condições:

- diodo **Schottky de sinal (BAT43)** e ***pull-up* de 1 kΩ**;
- fios de no máximo 30 cm;
- as duas placas sempre ligadas.

A fiação provisória do `PLANO.md` (1N5817 e 4,7 kΩ) **não funciona** a 500 kbit/s. Recomendo a opção (a), com diodos. A confirmação final depende de uma medição na bancada, porque nenhuma fonte da Espressif testa dois ESP32 ligados dessa forma.

## Esquema recomendado

### Diagrama (opção a: diodos e *pull-up*)

```
                          +3V3 (da placa B, que fica sempre ligada ao notebook)
                             │
                            ┌┴┐
                            │ │ R1 = 1 kΩ   (pull-up; aceitável de 1 a 2,2 kΩ)
                            └┬┘
                             │
 LINHA CAN ●─────────●───────●─────────────●─────────●        (1 fio, ≤ 30 cm no total)
           │         │                     │         │
           │         ▼ D1 BAT43            │         ▼ D2 BAT43
           │         ┬ (faixa = catodo)    │         ┬ (faixa = catodo)
           │         │                     │         │
        GPIO26    GPIO25                GPIO26    GPIO25
       (TWAI RX) (TWAI TX)             (TWAI RX) (TWAI TX)
       ┌─┴─────────┴─┐                 ┌─┴─────────┴─┐
       │  ESP32 A    │                 │  ESP32 B    │
       │  vguard     │                 │  adaptador  │
       │             │                 │  USB-CAN    │
       └──────┬──────┘                 └──────┬──────┘
             GND ─────────────────────────── GND      (fio de GND correndo junto da linha)
```

**Sentido do diodo:** o anodo vai na linha e o catodo (lado da faixa) no pino TX. Quando o TX vai a 0 V, o diodo conduz e puxa a linha para baixo. Quando o TX está em 3,3 V, o diodo fica bloqueado. É o mesmo sentido da figura 2 da AP2921 ([AP2921, p. 2][ap2921]) e do esquema da Keil ([Keil apnt_236, p. 21][keil]).

| TX de A | TX de B | Linha | O que acontece |
|---|---|---|---|
| 1 (recessivo) | 1 (recessivo) | ≈ 3,3 V (recessivo) | Os dois diodos estão bloqueados e o *pull-up* segura a linha em nível alto. |
| 0 (dominante) | 1 | ≈ 0,45 V (dominante) | D1 conduz. |
| 0 | 0 | ≈ 0,45 V (dominante) | Os dois diodos conduzem. |

Cada RX lê a própria linha. Assim, cada controlador vê os próprios bits (o eco que o TWAI exige) e também os bits da outra placa, incluindo o ACK.

### Lista de peças

| Qtd | Peça | Especificação | Por quê |
|---|---|---|---|
| 2 | Diodo Schottky de sinal **BAT43** (ou BAT42) | CD ≈ 7 pF; VF ≤ 0,33 V a 2 mA; trr ≤ 5 ns ([Vishay BAT42/43][bat43]) | Capacitância baixa (subida rápida) e queda baixa (nível dominante longe do limite do ESP32). Um substituto deve ter CD ≤ ~10 pF e VF ≤ ~0,4 V a 3 mA. |
| 1 | Resistor de **1 kΩ**, 1/4 W | Faixa aceitável de 1 a 2,2 kΩ | Subida até o nível alto em cerca de 0,12 µs (6% do bit), com cerca de 2,8 mA por pino (cálculo nos achados 4 e 5). |
| 3 | Jumpers curtos: linha, GND e 3V3 | ≤ 30 cm no total | A AP2921 limita o fio a bem menos de 1 m ([AP2921, p. 2][ap2921]). |
| (opc.) | 1N4148 no lugar do BAT43 | Nesse caso, R1 = 2,2–2,7 kΩ | Foi o que a Keil usou a 500 kbit/s ([Keil, p. 21 e 25][keil]), mas a margem do nível baixo fica pequena (achado 5). |
| **não usar** | 1N5817, 1N5819, 1N400x | — | O 1N5817 tem 110 pF a 4 V e cerca de 190 pF a 1 V ([Diodes DS23001][d5817]), lento demais a 500 kbit/s. Diodos de fonte recuperam devagar ([Keil, p. 4][keil]). |

### Pinos

| Sinal | Pino | Por que serve |
|---|---|---|
| TWAI TX | **GPIO25** | <ul><li>É uma saída válida da matriz de GPIO ([TRM §6.3.1][trm]).</li><li>Não é pino de *strapping* ([Datasheet, Tab. 3-1][ds]).</li><li>Fica com saída desligada e sem *pull* durante e logo após o reset (`oe=0, ie=0`, [Datasheet, A.4 IO_MUX][ds]).</li></ul> |
| TWAI RX | **GPIO26** | Tem as mesmas propriedades do GPIO25. |
| Evitar | GPIO0, 2, 5, 12, 15 | São de *strapping*. Um *pull-up* ou diodo neles pode mudar o modo de boot ([Datasheet, Tab. 3-1][ds]). |
| Evitar | GPIO6–11 | São os pinos SD_CLK, SD_DATA_0–3 e SD_CMD, ligados à flash externa do módulo ([Datasheet, §2.6, Tab. 2-6][ds]). |
| Evitar como TX | GPIO34–39 | São só de entrada. As saídas pela matriz são 0–19, 21–23, 25–27 e 32–33 ([TRM §6.3.1][trm]). |

São os mesmos pinos provisórios do `PLANO.md` §6, e a pesquisa confirma a escolha.

### Firmware (nas duas placas)

- Modo **normal**. Não usar `LISTEN_ONLY` nem `NO_ACK` na placa B: sem ACK do outro nó, a placa A não consegue transmitir (achado 6).
- 500 kbit/s com a temporização padrão do ESP-IDF (`TWAI_TIMING_CONFIG_500KBITS()`).
- Nenhuma chamada extra de GPIO. **O código é o mesmo que vai rodar com transceptor.**

### Regras de montagem

1. Monte tudo com as duas placas desligadas. Ligue o GND entre elas primeiro.
2. Alimente as duas pelo mesmo notebook ou hub, e mantenha **as duas ligadas** enquanto estiverem na linha. Antes de desconectar o USB de uma placa, tire o fio da linha dela. Uma placa sem alimentação puxa a linha pelas proteções dos pinos (achado 7).
3. Antes do primeiro teste, meça a linha parada com o multímetro: deve dar ≈ 3,3 V.
4. Com tráfego, confira no osciloscópio ou analisador lógico:
   - nível baixo ≤ 0,5 V;
   - subida até 2,5 V em ≤ 300 ns.

### Opção (b): dreno aberto, só se faltar diodo

**Fiação:** o TX (em dreno aberto) e o RX de cada placa vão direto na linha, com o mesmo *pull-up* de 1 kΩ e sem diodo. A outra variante, TX e RX no mesmo GPIO, também é possível; a Espressif usa esse truque, mas só em testes com um único nó (achado 3).

**Firmware:**

- Chamar `gpio_od_enable(GPIO25)` nos dois firmwares, incluindo o do adaptador USB-CAN. A função está em `esp_private/gpio.h`, que é um cabeçalho privado.
- A chamada vai depois de `twai_driver_install()` e antes de `twai_start()`. Na API nova, vai entre `twai_new_node_onchip()` e `twai_node_enable()`.
- **Nunca** chamar `gpio_config()` ou `gpio_set_direction()` no TX depois disso, porque essas funções desligam o TWAI do pino (achado 8).

**Por que não é a recomendada:** se uma das placas rodar um firmware sem essa chamada, duas saídas comuns (*push-pull*) ficam em curto, 3,3 V contra 0 V. Com diodos, isso é impossível pela própria montagem, e o código não muda.

### Quando chegarem os transceptores (SN65HVD230)

```
ESP32 GPIO25 (TX) ──► D   (pino 1) ┐                ┌ CANH (pino 7) ═╗
ESP32 GPIO26 (RX) ◄── R   (pino 4) │  SN65HVD230    │                ║ par trançado
3V3 ──────────────── VCC (pino 3)  │                │                ║
GND ──────────────── GND (pino 2)  │                └ CANL (pino 6) ═╝
GND ──────────────── Rs  (pino 8) ┘  (Rs no GND = modo de alta velocidade)

Bancada: [placa A]══ CANH/CANL ══[placa B], com 120 Ω entre CANH e CANL em CADA ponta
```

| Item | Bancada sem transceptor | Com SN65HVD230 |
|---|---|---|
| Diodos e *pull-up* | Necessários | Saem da montagem. O TX vai direto no pino D e o RX vem do pino R. |
| Fios | 1 linha + GND | Par trançado CANH/CANL + GND. O ISO 11898 pede par de 120 Ω ([SN65HVD230 §11.2.1.1][hvd230]). |
| Terminação | Não existe | <ul><li>**120 Ω em cada ponta** do cabo ([SN65HVD230 §11.2.1.1][hvd230]; [SLLA270 §4][slla270]).</li><li>Muitas placas de módulo já trazem esse resistor.</li><li>Confira medindo **≈ 60 Ω entre CANH e CANL** com tudo desligado.</li></ul> |
| No carro | — | <ul><li>O ESP32 entra como derivação (*stub*) pela tomada OBD, não como ponta do barramento.</li><li>Não acrescente 120 Ω. Retire ou desligue o resistor do módulo.</li><li>A derivação deve ter ≤ 0,3 m ([SLLA270 §4][slla270]: *"placing RL on a node should be avoided"*).</li><li>Isso é inferência a partir da regra das duas pontas; o ISO 15765-4 não foi consultado.</li></ul> |
| Pino Rs | — | <ul><li>Rs no GND: alta velocidade.</li><li>Rs em nível alto: *standby* "listen only", com o driver desligado e o receptor ativo ([SN65HVD230, Pin Functions e Feature Description][hvd230]).</li><li>O uso como modo silencioso é outra pesquisa.</li></ul> |
| Código | — | <ul><li>Na opção (a), não muda.</li><li>Na (b), remover o `gpio_od_enable`.</li></ul> |
| Atraso de ida e volta | Nanossegundos | t(LOOP) ≈ 70–100 ns (típico) e ≤ 115–135 ns (máximo) com Rs = 0 V ([SN65HVD230 §8.9][hvd230]). É pequeno perto do bit de 2 µs. |

## Achados com fontes

### 1. O CAN não exige transceptor, só um meio com nível dominante e recessivo

- A especificação Bosch deixa o meio físico livre: *"The way in which this channel is implemented is not fixed in this specification. E.g. single wire (plus ground), two differential wires, optical fibres, etc."* Ela também dá o wired-AND como exemplo: com transmissões simultâneas, o nível dominante (0) prevalece ([Bosch CAN 2.0, Parte B, p. 40][bosch]).
- O TRM do ESP32 diz o mesmo: *"The physical implementation on the bus is left to the application level to decide (e.g., differential wiring)"* ([TRM §25.3.1][trm]).
- O transceptor é exigido para ligar a um **barramento padrão**. O ESP-IDF diz que *"to connect the TWAI controller to a TWAI bus, an external transceiver is required"* e cita os SN65HVD23x para ISO 11898-2 ([ESP-IDF v5.4, Signal Lines and Transceiver][idf54]; [TRM §25.1][trm]).
- A Bosch observa que o número de nós *"will be limited by delay times and/or electrical loads on the bus line"* ([Bosch, Parte B, p. 40][bosch]).

### 2. A técnica com diodos é documentada por fabricantes

**Siemens AP2921** ("On-Board Communication via CAN without Transceiver", 12/1996) ([AP2921][ap2921]):

- *"All TxDC output lines (push-pull) are connected to a single data line via fast diodes to avoid short circuits at the output pins."*
- *"The RxDC inputs are directly connected to this line, which is pulled to +5V by a pull-up resistor"*, de 3k3 na figura 2.
- O limite *"current at one port pin does not exceed 1.6 mA"* vale para os pinos dos Siemens C515C/C167CR. Para o ESP32, o que vale é o achado 4.
- *"the maximum wire length is limited (<<1m) and disturbances due to noise can occur"*.
- A montagem serve para ligar *"two or more controller devices via one signal line"*.

**Keil/ARM apnt_236** ("CAN Primer", R. Boys, 2013) ([Keil][keil]):

- Na p. 4: *"There will be no noise immunity and you might have to lower the speed"*.
- Também na p. 4: *"Use a signal diode similar to 1N914 or 1N4148. Power supply diodes usually do not have a fast enough recovery time for CAN to function."*
- Na p. 21, a montagem real numa placa STM32F4 Discovery: dois 1N4148 e um resistor de 3,3 kΩ (o autor usou 2,7 kΩ) para 3 V. A nota avisa que não serve para um barramento CAN real.
- Na p. 25, o osciloscópio dessa montagem mostra 500 kbit/s: *"The most narrow pulse is 2 µsec wide … This is the CAN frequency in our example. The base is 0.665 volts and the top is at 3.0 volts."*

**Ressalva:** as duas notas usam controladores na mesma placa e nenhuma usa ESP32.

### 3. O que o TWAI do ESP32 exige

- **Níveis lógicos:** o TX e o RX *"represent/interpret a dominant bit as a low logic level (0 V) and a recessive bit as a high logic level (3.3 V)"* ([ESP-IDF v5.4][idf54]).
- **Eco do TX no RX:** *"An external transceiver must internally loop back the TX to RX such that a change in logic level to the TX signal line can be observed on the RX line. Failing to do so will cause the TWAI controller to interpret differences in logic levels between the two signal lines as a loss in arbitration or a bit error."* ([ESP-IDF v5.4, nota][idf54]).
  - É o *bit monitoring* do CAN: *"A unit that is sending a bit on the bus also monitors the bus"* ([Bosch, Parte B, p. 59][bosch]).
  - Na linha com diodos, o RX lê a própria linha, então o eco existe.
  - **Cruzar o TX de A no RX de B (e vice-versa) não funciona**, porque cada nó deixa de ver os próprios bits. Esse erro aparece em fóruns (achado 11).
- **Sem transceptor, com um único nó:** *"For single-node testing, you can directly short the TX and RX pins to omit the transceiver."* ([ESP-IDF v5.5, Hardware Connection][idf55]).
  - Os testes da própria Espressif usam `io_cfg.rx = TEST_TX_GPIO; // Using same pin for test without transceiver` ([test_twai_common.cpp L51–52][test]).
  - O driver só exige que o TX seja uma saída válida e o RX um GPIO válido; não proíbe TX = RX ([twai.c L443–444][twai_c_chk]).
- **Pinos:** a matriz de GPIO leva o TX e o RX do TWAI a qualquer GPIO ([TRM §6.1][trm]; [ESP-IDF v5.4][idf54]). As saídas possíveis são os GPIO 0–19, 21–23, 25–27 e 32–33 ([TRM §6.3.1][trm]).
- **O driver deixa o TX em *push-pull*:** ele só seleciona a função GPIO e liga o sinal; não ativa dreno aberto ([twai.c L290–315][twai_c]). O driver novo também liga o *pull-up* interno no RX, *"to avoid noise if no connection to transceiver"* ([esp_twai_onchip.c L139–147][onchip]). Esse *pull-up* de ~45 kΩ é irrelevante ao lado de 1 kΩ.

### 4. Limites elétricos (datasheets)

**ESP32** ([Datasheet v5.3, Tab. 5-3 e notas do Apêndice A.1][ds]):

| Grandeza | Valor | Consequência |
|---|---|---|
| VIL (nível baixo de entrada) | ≤ 0,25 × VDD = **0,825 V** | A linha em dominante precisa ficar abaixo disso. |
| VIH (nível alto de entrada) | ≥ 0,75 × VDD = **2,475 V**; máximo VDD + 0,3 V | A linha precisa subir até aqui antes da amostragem. Um pino não pode receber mais que VDD + 0,3 V. |
| CIN (capacitância do pino) | 2 pF | — |
| IOL (corrente de saída em nível baixo) | 28 mA com VOL = 0,495 V (força máxima) | 3 mA de *pull-up* é pouco para o pino. |
| Força de saída padrão | ~20 mA (nível 2 de 0 a 3) | Corrente de curto na opção (b) sem dreno aberto. |
| *Pull-up* e *pull-down* internos | 45 kΩ | Fraco demais para ser o *pull-up* da linha. |

**Diodos:**

| Diodo | Capacitância | VF na corrente de uso | Recuperação | Fonte |
|---|---|---|---|---|
| BAT43 | 7 pF a 1 V (≈ 10 pF a 0 V, fig. 4) | 0,26–0,33 V a 2 mA; ≤ 0,45 V a 15 mA | ≤ 5 ns | [Vishay 85660 rev 2.2][bat43] |
| 1N4148 | ≤ 4 pF a 0 V | ≈ 0,6 V a 1 mA e ≈ 0,68 V a 3 mA (típico, fig. 1–2); ≤ 1 V a 10 mA | ≤ 4–8 ns | [Vishay 81857 rev 1.6][1n4148] |
| 1N5817 | 110 pF a 4 V; ≈ 190 pF a 1 V; ≈ 270 pF a 0,1 V (fig. 4) | — | — | [Diodes DS23001 rev 10-2][d5817] |

### 5. Temporização a 500 kbit/s e cálculo da subida

**Configuração do ESP-IDF:**

- Bit = 2 µs.
- A macro padrão para cristal de 40 MHz é `quanta_resolution_hz = 10000000, tseg_1 = 15, tseg_2 = 4, sjw = 3`. Isso dá 20 Tq por bit, **amostragem a 80% (1,6 µs)** e SJW de 300 ns ([twai_types.h L49–52 e L84][types]).

**Só a borda de recessivo para dominante sincroniza:**

- TRM: *"Synchronizations only occurs on Recessive to Dominant edges"* ([TRM §25.3.4.2][trm]).
- Bosch: regras 3 e 4 de sincronização ([Bosch, Parte B, p. 68][bosch]).
- Na linha com diodos, essa borda (descida) é rápida, porque o pino puxa a linha através do diodo. A borda lenta é a subida, puxada pelo resistor. Ela não afeta a sincronização, mas precisa passar de VIH antes da amostragem.

**Cálculo nosso:**

t(VIH) = R·C·ln[(VDD − V_baixo)/(VDD − VIH)], com V_baixo = VOL + VF.

**Premissas (são estimativas, não medições):**

- VOL ≤ 0,1 V a até 3 mA, derivado do IOL acima.
- C ≈ 60–100 pF para a montagem com BAT43, somando:
  - 2 pinos RX × 2 pF;
  - 2 diodos × 7–10 pF;
  - ~40–80 pF de trilhas do DevKit, protoboard e 30 cm de fio.
- C ≈ 420 pF com 1N5817.

| Diodo | R | C | V_baixo | Margem até VIL | Subida até VIH | % do bit | Veredito |
|---|---|---|---|---|---|---|---|
| BAT43 | **1 kΩ** | 100 pF | 0,45 V | 0,38 V | 0,12 µs | 6% | **Folga** |
| BAT43 | 2,2 kΩ | 100 pF | 0,43 V | 0,39 V | 0,27 µs | 14% | OK |
| BAT43 | 4,7 kΩ | 100 pF | 0,40 V | 0,42 V | 0,59 µs | 30% | Apertado |
| 1N4148 | 2,7 kΩ | 100 pF | 0,72 V | 0,10 V | 0,31 µs | 15% | Funciona (Keil), mas com nível baixo perto do limite |
| 1N4148 | 1 kΩ | 100 pF | 0,78 V | 0,04 V | 0,11 µs | 6% | Evitar: nível baixo no limite |
| 1N5817 | 4,7 kΩ | 420 pF | 0,40 V | 0,42 V | **2,5 µs** | 124% | **Não funciona a 500 kbit/s** (é o `PLANO.md` atual) |
| sem diodo (b) | 1 kΩ | 50 pF | 0,10 V | 0,72 V | 0,07 µs | 3% | Folga |

**Critério adotado (nosso):** subida ≤ 25% do bit (0,5 µs). Isso deixa mais de 0,8 µs até o ponto de amostragem mais cedo possível, que é 1,6 µs menos o SJW de 0,3 µs, ou seja, 1,3 µs.

**Conferência com a Keil:** o nível baixo medido de 0,665 V com 1N4148 bate com a tabela ([Keil, p. 25][keil]).

### 6. ACK num barramento de dois nós

- Todo receptor que recebe um quadro CAN com CRC correto escreve dominante no campo de ACK ([Bosch, Parte B, p. 48][bosch]). Um nó que filtra o ID **também dá ACK**: *"will not receive those messages, but will still send acknowledgments"* ([ESP-IDF v5.5, filtros][idf55]).
- Um nó em *listen only* **não dá ACK** ([TRM §25.5.1][trm]; [ESP-IDF v5.4][idf54]).
  - Errata do ESP32: em *listen only*, o controlador ainda pode emitir bits dominantes ao detectar erro.
  - O ESP-IDF contorna isso por padrão, forçando o estado *error passive* ([Kconfig.twai L60–69][kconfig]).
- **Sem ACK, o transmissor registra *ACK error*** ([TRM §25.3.3.1][trm]):
  - o TEC sobe 8 por erro até *error passive* (≥ 128);
  - a partir daí, pela "Exception 1", um transmissor *error passive* com ACK error não incrementa mais o TEC ([Bosch, Parte B, p. 62][bosch]; [TRM §25.3.3.3][trm]).
  - Ou seja, **não chega a *bus-off* só por falta de ACK**. Ele retransmite para sempre, a menos que o quadro seja *single shot* (`TWAI_MSG_FLAG_SS`, [ESP-IDF v5.4][idf54]).
- **Na bancada:** se a placa B estiver desligada, em reset ou em *listen only*, as requisições OBD da placa A ficam sem ACK. A placa A vai a *error passive* e repete o quadro. É o cenário "ECU que para de responder" do nível 2 do `PLANO.md` §6, e a placa B precisa estar em modo normal.

### 7. Estado dos pinos no boot e alimentação

- *"During reset, all pins are output-disabled."* O GPIO25 e o GPIO26 ficam com `oe=0, ie=0`, sem *pull-up* nem *pull-down*, **durante e logo após o reset** ([Datasheet, Apêndice A.1 notas 9–10 e A.4 IO_MUX][ds]).
  - Na opção (a), o TX em alta impedância não faz o diodo conduzir, então a linha fica recessiva durante o boot e durante a gravação da flash.
  - Na opção (b), o pino também fica em alta impedância.
- Os pinos de *strapping* são GPIO0, GPIO2, MTDI (GPIO12), MTDO (GPIO15) e GPIO5 ([Datasheet, Tab. 3-1][ds]).
- Ao sair do modo reset, o controlador espera **11 bits recessivos seguidos** antes de entrar no barramento ([TRM §25.5.1][trm]). Uma linha presa em nível baixo impede todos os nós de entrar.
- **Placa sem alimentação** (inferência a partir do datasheet): a entrada não pode passar de VDD + 0,3 V ([Datasheet, Tab. 5-3][ds]). Com VDD = 0, os 3,3 V da linha entram pelos pinos (pelas proteções de ESD) e a linha pode ficar num nível intermediário ou presa em dominante. Daí a regra de montagem 2.

### 8. Opção (b), dreno aberto: o que o hardware e o ESP-IDF permitem

- O hardware tem dreno aberto por pino, pelo bit `GPIO_PINx_PAD_DRIVER`: *"For an open drain output, set the GPIO_PINx_PAD_DRIVER bit"* ([TRM §6.3.2, p. 119][trm]).
- O driver TWAI não liga esse bit ([twai.c L290–315][twai_c]).
- `gpio_od_enable()` existe, mas é declarada em `esp_private/gpio.h`, cabeçalho privado que pode mudar entre versões ([esp_private/gpio.h L78–86][gpio_h]; [gpio.c L238–243][gpio_c_od]).
- `gpio_set_direction()` e `gpio_config()` chamam `gpio_output_enable()`, que faz `gpio_hal_matrix_out_default(...) // No peripheral output signal routed to the pin`. Isso **desconecta o TWAI do pino TX** ([gpio.c L222–229 e L315–345][gpio_c]).
- Sem dreno aberto, dois TX *push-pull* no mesmo fio ficam em curto (3,3 V contra 0 V). A corrente só é limitada pela força do driver: ~20 mA no padrão e até ~40 mA ([Datasheet, Apêndice A.1 nota 8][ds]).
- A Espressif não documenta dois nós em dreno aberto; ela só testa um nó com TX = RX (achado 3).

### 9. Erratas do TWAI do ESP32 que importam aqui

- **TWAI-3.13.10:** *"if the TWAI controller is acting as transmitter and encounters a negative phase error where e < 0 and |e| > SJW, the bits transmitted following the phase error will be left shifted by one bit"*, e *"There is no workaround for this issue"* ([Errata ESP32][errata]). Bordas falsas (ruído, subida lenta cruzando o limiar devagar) são o que gera esse tipo de erro de fase. Por isso convém manter bordas rápidas e fios curtos.
- As demais erratas do TWAI têm contorno no driver, ativo por padrão ([ESP-IDF v5.4, ESP32 Errata Workarounds][idf54]; [Kconfig.twai][kconfig]).

### 10. Transceptor e terminação

- **SN65HVD230** ([datasheet SLOS346O][hvd230]):
  - alimentação única de 3,3 V;
  - *"Designed for Data Rates up to 1 Mbps"*;
  - pino Rs: *"strong pull down to GND = high speed mode, strong pull up to VCC = low power mode, 10kΩ to 100kΩ pull down to GND = slope control mode"*;
  - o modo *standby* é *"listen only"*, com o driver desligado e o receptor ativo;
  - t(LOOP) de 70–100 ns (típico) e 115–135 ns (máximo) com Rs = 0 V.
- **Terminação:** *"Resistors equal to the characteristic impedance of the line should be used to terminate both ends of the cable"*, e *"Termination is typically a 120 Ω resistor at each end of the bus"* ([SN65HVD230 §11.2.1.1][hvd230]).
- **ISO 11898 segundo a TI** ([SLLA270 §4][slla270]):
  - 1 Mbps com 40 m e até 30 nós;
  - derivação sem terminação ≤ 0,3 m;
  - *"placing RL on a node should be avoided since the bus lines lose termination if the node is disconnected from the bus"*.

### 11. Evidência de fórum (só apoio, não é fonte primária)

- **ST Community, 2021–2022** ([tópico][st]):
  - um funcionário da ST diz que a técnica *"looks valid for low bitrates only"*;
  - que *"Schottky diodes with fast transient response are required"*;
  - que o *pull-up* deve diminuir com mais nós;
  - e que não serve para produção.
  - A pergunta original era justamente cruzar CAN1TX↔CAN2RX.
- **PJRC (Teensy)** ([tópico][pjrc]): relato de 4 m a 1 Mbit/s com diodos de sinal e 2,2 kΩ. **Não verificado:** a página devolveu 403, e o relato só foi visto em trecho de busca.
- **esp32.com (ESP32-P4, 2026)** ([tópico][esp32p4]): placas ligadas cruzadas, sem transceptor, com *pull-up* de 3,3 kΩ no RX, que "não recebem nenhum pacote válido". **Não verificado:** página com bloqueio anti-robô, só trecho de busca. O sintoma é coerente com o achado 3.

## Riscos e incertezas

| Risco | Chance | Impacto | Mitigação |
|---|---|---|---|
| Nenhuma fonte da Espressif valida dois ESP32 numa linha com diodos. A prova de 500 kbit/s vem da Keil (STM32) e do nosso cálculo. | Média | Médio | Fazer o teste de aceite da recomendação 4 antes de usar a bancada no M2. |
| As capacitâncias (protoboard, fios, trilhas do DevKit) são estimadas, não medidas. | Média | Baixo com 1 kΩ (margem de 10×); alto com 4,7 kΩ | Medir a subida no osciloscópio. Se passar de 300 ns, reduzir R (mínimo de 1 kΩ com BAT43) ou encurtar os fios. |
| O datasheet e o TRM não especificam histerese de entrada. Com borda lenta e ruído, podem surgir bordas falsas, e a errata 3.13.10 não tem contorno. | Baixa | Alto (quadro corrompido) | Usar 1 kΩ (borda rápida) e fios curtos com o GND junto. |
| Com 1N4148, o nível baixo (~0,7 V) fica só ~0,1 V abaixo do VIL garantido (0,825 V), e o VF sobe no frio. | Média, se usar 1N4148 | Médio | Preferir BAT43. Se não houver, usar 1N4148 com 2,2–2,7 kΩ e medir o nível baixo. |
| Placa sem alimentação na linha pode prender o barramento (achado 7). | Média (é fácil esquecer) | Médio | Seguir a regra de montagem 2. |
| Não há imunidade a ruído: o sinal não é diferencial e há laços de terra pelos cabos USB ([AP2921][ap2921]; [Keil, p. 4][keil]). | Média | Baixo na bancada | ≤ 30 cm, GND ao lado da linha, as duas placas no mesmo hub. |
| A opção (b) depende de API privada e de disciplina nos dois firmwares. | — | Alto (curto entre saídas) | Não usar a opção (b), a menos que faltem diodos. |
| A bancada não valida a camada física real: transceptor, terminação e o barramento do carro. | Certa | — | Continua no M4 e no ensaio do carro. |
| Não se sabe a versão do ESP-IDF que o PlatformIO vai trazer: driver antigo (`twai_driver_install`) ou novo (`twai_new_node_onchip`). | — | Baixo | O esquema vale para os dois. Só a opção (b) depende da API. |

## Recomendação para o ticket

1. **Decisão:** usar a opção (a).
   - **2 × BAT43** com catodo no TX (GPIO25) e anodo na linha;
   - RX (GPIO26) direto na linha;
   - **1 kΩ** da linha para 3V3 da placa B;
   - GND comum e fios ≤ 30 cm;
   - firmware em modo normal nas duas placas, sem nenhuma mudança de código.
2. **Compras:**
   - 2 BAT43 (ou BAT42) e um resistor de 1 kΩ. Na falta deles, 1N4148 com 2,2–2,7 kΩ.
   - Não usar 1N5817.
   - Comprar os 2 × SN65HVD230 de qualquer forma (o carro exige). Conferir se o módulo traz 120 Ω.
3. **Corrigir o `PLANO.md` §6** (fora deste ramo):
   - trocar "BAT43 ou 1N5817" por "BAT43 (não 1N5817)";
   - trocar "4,7 kΩ" por "1 kΩ";
   - acrescentar a regra de alimentação.
4. **Teste de aceite da bancada,** a fazer antes de fechar o M2:
   - linha parada ≈ 3,3 V;
   - nível baixo ≤ 0,5 V;
   - subida até 2,5 V ≤ 300 ns;
   - 30 min a 500 kbit/s, com as duas placas transmitindo, inclusive IDs que disputam a arbitragem;
   - TEC e REC em 0 e nenhum alerta de erro de barramento (`twai_get_status_info` e alertas).
   - Se falhar: primeiro encurtar os fios e baixar R. Em último caso, cair para 250 ou 125 kbit/s, recompilando os dois lados ([ADR-0001](../adr/0001-obd-por-requisicao-padronizada.md)).
5. **Encerrar o ticket** com: "Sim, a 500 kbit/s, com BAT43 + 1 kΩ, fios curtos e as duas placas ligadas. Validação pendente pelo teste de aceite." Pela análise, os pontos de atenção do Thiago (pino Rs) e da Isabela (nenhum) não mudam.

## Referências

- [ap2921]: Siemens Semiconductor Group, AP2921 "On-Board Communication via CAN without Transceiver", J. Barrenscheen, 12/1996, Rel 01. Cópia arquivada em https://web.archive.org/web/20201125222911/https://www.mikrocontroller.net/attachment/28831/siemens_AP2921.pdf (original: https://www.mikrocontroller.net/attachment/28831/siemens_AP2921.pdf).
- [keil]: ARM/Keil, Application Note 236 "CAN Primer: Creating Your Own Network", R. Boys, V2.0, 2013. https://www.keil.com/appnotes/files/apnt_236.pdf
- [bosch]: Robert Bosch GmbH, "CAN Specification Version 2.0", 1991. Cópia hospedada pela UC Riverside: http://esd.cs.ucr.edu/webres/can20.pdf
- [ds]: Espressif, "ESP32 Series Datasheet" v5.3. https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
- [trm]: Espressif, "ESP32 Technical Reference Manual" v5.8, cap. 6 (IO MUX e GPIO Matrix) e cap. 25 (TWAI). https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf
- [idf54]: ESP-IDF v5.4, "Two-Wire Automotive Interface (TWAI)". https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/peripherals/twai.html
- [idf55]: ESP-IDF v5.5, "Two-Wire Automotive Interface (TWAI)". https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/peripherals/twai.html
- [twai_c]: ESP-IDF `components/driver/twai/twai.c` (release/v5.4 @ 962f1ee). https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/driver/twai/twai.c#L290-L315
- [twai_c_chk]: idem, verificação dos pinos. https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/driver/twai/twai.c#L443-L444
- [types]: ESP-IDF `components/hal/include/hal/twai_types.h` (release/v5.4 @ 962f1ee). https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/hal/include/hal/twai_types.h#L49-L84
- [kconfig]: ESP-IDF `components/driver/twai/Kconfig.twai` (release/v5.4 @ 962f1ee). https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/driver/twai/Kconfig.twai#L18-L69
- [gpio_c]: ESP-IDF `components/esp_driver_gpio/src/gpio.c` (release/v5.4 @ 962f1ee). https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/esp_driver_gpio/src/gpio.c#L222-L345
- [gpio_c_od]: idem, `gpio_od_enable`. https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/esp_driver_gpio/src/gpio.c#L238-L243
- [gpio_h]: ESP-IDF `components/esp_driver_gpio/include/esp_private/gpio.h` (release/v5.4 @ 962f1ee). https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/esp_driver_gpio/include/esp_private/gpio.h#L78-L86
- [onchip]: ESP-IDF `components/esp_driver_twai/esp_twai_onchip.c` (release/v5.5 @ 2553c5a). https://github.com/espressif/esp-idf/blob/2553c5ad432927e09ccbbad3b954e1cbc4bd7655/components/esp_driver_twai/esp_twai_onchip.c#L139-L147
- [test]: ESP-IDF `components/esp_driver_twai/test_apps/test_twai/main/test_twai_common.cpp` (master @ ab95e70). https://github.com/espressif/esp-idf/blob/ab95e70748f2eddac6f87217b0cf62533684a6ba/components/esp_driver_twai/test_apps/test_twai/main/test_twai_common.cpp#L51-L52
- [errata]: Espressif, "ESP32 Series SoC Errata", TWAI-3.13.10. https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32/03-errata-description/esp32/twai-negative-phase-error.html
- [hvd230]: Texas Instruments, SN65HVD23x datasheet, SLOS346O (rev. abr/2018). https://www.ti.com/lit/ds/symlink/sn65hvd230.pdf
- [slla270]: Texas Instruments, SLLA270 "Controller Area Network Physical Layer Requirements", S. Corrigan, jan/2008. https://www.ti.com/lit/an/slla270/slla270.pdf
- [bat43]: Vishay, BAT42/BAT43 "Small Signal Schottky Diode", doc. 85660 rev. 2.2 (12/05/2025). https://www.vishay.com/docs/85660/bat42.pdf
- [1n4148]: Vishay, 1N4148 "Small Signal Fast Switching Diodes", doc. 81857 rev. 1.6 (07/11/2024). https://www.vishay.com/docs/81857/1n4148.pdf
- [d5817]: Diodes Inc., 1N5817–1N5819, DS23001 rev. 10-2 (abr/2023). https://www.diodes.com/assets/Datasheets/1N5817-1N5819.pdf
- [st] (fórum): ST Community, "On-board communication via CAN without transceiver". https://community.st.com/stm32-mcus-products-25/on-board-communication-via-can-without-transceiver-24281
- [pjrc] (fórum, não aberto): PJRC, "CAN bus Teensy-Teensy communication without transceiver". https://forum.pjrc.com/threads/43684-CAN-bus-Teensy-Teensy-communication-without-transceiver
- [esp32p4] (fórum, não aberto): esp32.com, "TWAI driver refusing to receive any valid packets". https://esp32.com/viewtopic.php?p=154892

[ap2921]: https://web.archive.org/web/20201125222911/https://www.mikrocontroller.net/attachment/28831/siemens_AP2921.pdf
[keil]: https://www.keil.com/appnotes/files/apnt_236.pdf
[bosch]: http://esd.cs.ucr.edu/webres/can20.pdf
[ds]: https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
[trm]: https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf
[idf54]: https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/peripherals/twai.html
[idf55]: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/peripherals/twai.html
[twai_c]: https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/driver/twai/twai.c#L290-L315
[twai_c_chk]: https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/driver/twai/twai.c#L443-L444
[types]: https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/hal/include/hal/twai_types.h#L49-L84
[kconfig]: https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/driver/twai/Kconfig.twai#L18-L69
[gpio_c]: https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/esp_driver_gpio/src/gpio.c#L222-L345
[gpio_c_od]: https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/esp_driver_gpio/src/gpio.c#L238-L243
[gpio_h]: https://github.com/espressif/esp-idf/blob/962f1ee3888e691896bbd3af8dd33294501ce820/components/esp_driver_gpio/include/esp_private/gpio.h#L78-L86
[onchip]: https://github.com/espressif/esp-idf/blob/2553c5ad432927e09ccbbad3b954e1cbc4bd7655/components/esp_driver_twai/esp_twai_onchip.c#L139-L147
[test]: https://github.com/espressif/esp-idf/blob/ab95e70748f2eddac6f87217b0cf62533684a6ba/components/esp_driver_twai/test_apps/test_twai/main/test_twai_common.cpp#L51-L52
[errata]: https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32/03-errata-description/esp32/twai-negative-phase-error.html
[hvd230]: https://www.ti.com/lit/ds/symlink/sn65hvd230.pdf
[slla270]: https://www.ti.com/lit/an/slla270/slla270.pdf
[bat43]: https://www.vishay.com/docs/85660/bat42.pdf
[1n4148]: https://www.vishay.com/docs/81857/1n4148.pdf
[d5817]: https://www.diodes.com/assets/Datasheets/1N5817-1N5819.pdf
[st]: https://community.st.com/stm32-mcus-products-25/on-board-communication-via-can-without-transceiver-24281
[pjrc]: https://forum.pjrc.com/threads/43684-CAN-bus-Teensy-Teensy-communication-without-transceiver
[esp32p4]: https://esp32.com/viewtopic.php?p=154892
