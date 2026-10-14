# Transceptor em modo silencioso e pinos seguros no boot do ESP32

Pesquisa do ticket #4, feita em 13/10/2026. As fontes são primárias: datasheets da TI, NXP, Microchip e Espressif, a documentação do ESP-IDF e o código-fonte do ESP-IDF. As poucas fontes secundárias aparecem marcadas como tal. Os links estão no fim do arquivo.

## Pergunta

Queremos uma salvaguarda **no hardware**: por padrão o transceptor CAN fica num modo em que não transmite, preso por um resistor. Ele só é liberado por um GPIO do ESP32 depois que o firmware escutou o barramento e o achou saudável. Assim, um reset, um travamento, uma gravação ou um firmware errado nunca colocam nada no barramento do carro (500 kbit/s, IDs de 11 bits).

1. **Escuta em modo silencioso.** O que faz o pino Rs do SN65HVD230? No modo standby o driver desliga e o receptor continua funcionando a 500 kbit/s? Como se comparam o SN65HVD231/232, o TJA1051T/3 (pino S) e o MCP2562? Qual deles oferece escuta de verdade em velocidade plena?
2. **TXD flutuando.** Cada transceptor tem pull-up interno no TXD? Tem timeout de dominante? Um TXD flutuando ou em nível baixo durante o reset do ESP32 pode travar o barramento em dominante?
3. **Pinos do ESP32 no boot.** Quais GPIOs ficam em alta impedância, com pull ou pulsando durante o reset, o boot e a gravação? Quais servem para o TX e o RX do TWAI e para a linha de liberação?
4. **Circuito.** Que resistores usar, qual o estado padrão, que peça comprar e como o firmware libera o TX depois da escuta?

## Resposta curta

- **Escuta de verdade em velocidade plena** só é prometida no datasheet pelos transceptores com *Silent mode* no pino S: o TJA1051 ([NXP §7.1.2][nxp1051]), o TCAN330/TCAN337 ([TI §6.4.3][tcan33x]) e o TCAN1051 com sufixo V ([TI §8.4.3][tcan1051]). Neles o driver desliga e o receptor segue igual ao do modo normal.
- **No SN65HVD230**, Rs acima de 0,75·VCC liga o *standby (listen only)*: o driver desliga, e o datasheet diz que o receptor continua ligado e "espelha o barramento" ([TI Tab. 2][ti230]). Porém esse é um modo de baixo consumo, e nenhum tempo do receptor é especificado com Rs alto. A recepção a 500 kbit/s é provável, mas não é garantida de forma explícita, e precisa ser validada na bancada.
- **Não servem para escutar:**
  - o SN65HVD231, que no *sleep* desliga também o receptor ([TI §10.4.5][ti230]);
  - o SN65HVD232, que não tem pino de modo ([TI §7][ti230]);
  - o MCP2562, cujo *standby* só tem um receptor de baixo consumo com filtro de *wake-up* de 0,5 a 4 µs, enquanto o bit a 500 kbit/s dura 2 µs ([Microchip §1.1.2 e §2.3][mcp2562]).
- **TXD:** todos os modelos analisados têm um pull-up interno fraco no TXD, então TXD aberto vira recessivo. Só o TJA1051, o TCAN33x/TCAN1051 e o MCP2562 têm timeout de dominante; o SN65HVD230 não tem. Um TXD **puxado para baixo** pode, sim, travar o barramento em dominante, e no SN65HVD230 sem limite de tempo. Isso acontece, por exemplo, com um GPIO que tem pull-down interno no reset. Com o modo silencioso como padrão, o nível do TXD deixa de importar.
- **ESP32:** durante o reset, todos os pinos ficam com a saída desabilitada ([DS Tab. 6-1, nota 9][esp32ds]). Os GPIO25, 26 e 27 não têm pull nem função de boot, nem durante nem logo depois do reset. **Porém** no ESP32 o `esp_restart()` e o reboot depois de um *panic* fazem só um reset de CPU, que **não** reinicia os GPIOs nem o TWAI ([TRM Tab. 7.1-1][esp32trm]; [system_internal.c][idfsysint]; [panic_handler.c][idfpanic]). Nesses casos, a linha de liberação continua como estava até o novo firmware mexer nela. O resistor sozinho não cobre esse caso; o firmware precisa cobrir.
- **Recomendação:**
  - Comprar um TJA1051T/3 com o pino S acessível, por exemplo o módulo Adafruit CAN Pal, que roda só com 3,3 V e expõe o S como SLNT ([Adafruit][canpalpins]).
  - Ligar 10 kΩ do S para 3,3 V, o que deixa o transceptor silencioso por padrão, e 10 kΩ de pull-up no TXD.
  - Pôr um jumper físico em série com a linha de liberação.
  - Usar TX = GPIO25, RX = GPIO26 e liberação = GPIO27.
  - No firmware: TWAI em *listen-only* durante a escuta; depois baixar o S e só então passar o TWAI ao modo normal.
  - O TCAN337 é a alternativa só de 3,3 V. O SN65HVD230 fica como plano B, com retrabalho e validação.

## Comparação de transceptores

| Peça | Modo que não transmite (pino e nível) | Receptor nesse modo | Recepção a 500 kbit/s garantida nesse modo? | Pino de modo aberto vira… | TXD aberto vira… | Timeout de dominante no TXD | Alimentação | Serve? |
|---|---|---|---|---|---|---|---|---|
| **SN65HVD230** | Rs > 0,75·VCC: *standby (listen only)* ([§8.3, §10.4.3][ti230]) | ligado, "Mirrors Bus State" ([Tab. 2, Tab. 4][ti230]) | **Não está explícito.** Os tempos do receptor (§8.8) não citam Rs, e o standby é descrito como modo para detectar atividade ([§5][ti230]) | Não especificado. Entre 1,2 V e 0,75·VCC o modo é indefinido ([Tab. 2][ti230]), e a TI manda usar resistor externo ([§13.1][ti230]) | recessivo ([Tab. 3, linha "Open"][ti230]), por pull-up fraco ([§8.5, Fig. 2][ti230]) | **não** | 3,0 a 3,6 V ([§8.3][ti230]) | sim, com ressalvas |
| SN65HVD231 | Rs alto: *sleep* | **desligado**, R fica em nível alto ([§10.4.5, Tab. 5][ti230]) | não | — | recessivo | não | 3,3 V | **não** |
| SN65HVD232 | não tem pino de modo; os pinos 5 e 8 são NC ([§7][ti230]) | — | — | — | recessivo | não | 3,3 V | **não** |
| **TJA1051T/3** | S em nível alto: *Silent* ([Tab. 4][nxp1051]) | "including the receiver, continue to operate as in Normal mode" ([§7.1.2][nxp1051]). Os atrasos do receptor são especificados para "Normal/Silent mode" ([Tab. 8][nxp1051]) | **sim** | pull-down interno, ou seja, modo Normal ([§7.2.2][nxp1051]). Precisa de pull-up externo | pull-up interno para VIO ([§7.2.2][nxp1051]) | sim, 0,3 a 5 ms ([§7.2.1, Tab. 8][nxp1051]) | VCC 4,5–5,5 V e VIO 2,8–5,5 V ([Tab. 7][nxp1051]) | **sim** |
| **TCAN330 / TCAN337** | S em nível alto: *Silent* ([Tab. 6-3][tcan33x]) | "the receiver is fully operational" ([§6.4.3][tcan33x]) | **sim** | pull-down interno, ou seja, modo normal ([Tab. 6-2][tcan33x]). Precisa de pull-up externo | pull-up interno ([Tab. 6-2][tcan33x]) | sim, 1,2 a 3,8 ms. Também tem timeout de dominante no RXD e, no TCAN337, um pino FAULT ([§5.6, §6.3][tcan33x]) | 3,0 a 3,6 V ([§5.3][tcan33x]) | **sim** |
| TCAN1051 (sufixo V) | S em nível alto: *Silent* | "The high speed receiver remains active" ([§8.4.3][tcan1051]) | sim | pull-down interno, ou seja, modo Normal ([§8.3.5][tcan1051]) | pull-up interno ([§8.3.5][tcan1051]) | sim, 1,2 a 3,8 ms ([§6.8][tcan1051]) | VCC 4,5–5,5 V e VIO 2,8–5,5 V | sim |
| MCP2562 | STBY em nível alto: *Standby* | o receptor rápido desliga e sobra o de baixo consumo com filtro de *wake-up*. O RXD mostra uma "delayed representation" ([§1.1.2, §1.7.9][mcp2562]) | **não**: tFLTR(WAKE) de 0,5 a 4 µs ([§2.3, parâm. 9][mcp2562]) contra um bit de 2 µs | pull-up interno de cerca de 1,1 MΩ a 3,3 V, ou seja, *standby* ([§1.7.9][mcp2562]) | pull-up de 33 kΩ ([§1.7.1][mcp2562]) | sim, cerca de 1,25 ms ([§1.5][mcp2562]) | VDD 4,5–5,5 V e VIO 1,8–5,5 V | **não**, para escutar |

Uma curiosidade: o SN65HVD235 tem um modo *autobaud* em que o driver desliga, o receptor fica ativo e o D é ligado internamente ao R ([TI SLLS557H §10.3.2][ti235]). Não traz vantagem sobre um *Silent mode* comum.

## Pinos do ESP32 no boot

### O que cada tipo de reset faz com os pinos

| Evento | Nível de reset ([TRM §7.1, Tab. 7.1-1][esp32trm]) | Efeito nos GPIOs | Linha de liberação (S/Rs) |
|---|---|---|---|
| Ligar a placa, botão EN, gravação pelo esptool (DTR/RTS acionam EN e GPIO0, conforme [esptool][esptool]), brownout | reset de chip ou de sistema | durante o reset, todas as saídas ficam desabilitadas ([DS Tab. 6-1, nota 9][esp32ds]). Ao sair do reset, cada pino vai à "Function 0" com os pulls da tabela abaixo (nota 10) | flutua, e o pull-up externo a mantém **silenciosa** ✔ |
| Reset de núcleo (*core reset*), como "Software System Reset" e "MWDT Global Reset" | core reset | reinicia "external GPIO and digital GPIO"; o domínio RTC não é reiniciado ([TRM §7.1.1][esp32trm]) | silenciosa ✔, desde que o pino não esteja em *hold*, porque o *hold* sobrevive ao core reset ([TRM §6.7][esp32trm]) |
| `esp_restart()` e reboot depois de um *panic* no ESP-IDF | **reset só de CPU** (Software CPU Reset): `esp_restart_noos()` chama `esp_rom_software_reset_cpu()` ([system_internal.c][idfsysint]), e `panic_restart()` chama `esp_restart_noos()` ([panic_handler.c][idfpanic]) | **Os GPIOs não são reiniciados.** O ESP-IDF reinicia só timers, SPI, UART, MCPWM, I2S, UHCI, cripto e Wi-Fi/BT ([system_internal.c][idfsysint]). Depois de um reset de CPU, o boot mantém ligados os clocks de periféricos que estavam ativos, inclusive o do TWAI ([clk.c][idfclk]) | **continua como estava** ✘. Se estava liberada, segue liberada até o novo firmware mexer nela |
| *Panic* configurado para parar a CPU (*halt*) | nenhum | tudo fica congelado ([panic.c][idfpanicc]) | continua liberada ✘ |
| Modo de gravação (ROM em *download*) | entra por reset de chip | o *download* "conjunto" inclui UART e SDIO ([DS §3.1, Tab. 3-3][esp32ds]); a ROM fala pela UART0 | silenciosa ✔, porque o GPIO27 não é pino de boot |
| Firmware errado que coloca o GPIO27 em nível baixo | — | — | **liberada** ✘. Só um jumper físico impede isso |

### Estado de cada GPIO no reset

As colunas "Durante o reset" e "Logo após o reset" vêm da tabela IO_MUX ([DS Apêndice A.4][esp32ds]). "ie" é a entrada habilitada; "wpu" e "wpd" são o pull-up e o pull-down internos, de cerca de 75 µA, o que equivale a uns 45 kΩ ([DS Tab. 6-1, nota 8][esp32ds]; [esptool][esptool]).

| GPIO | Durante o reset | Logo após o reset | Papel no boot e restrições | TX do TWAI | RX do TWAI | Liberação |
|---|---|---|---|---|---|---|
| 0 | ie, **wpu** | ie, wpu | *strapping* do modo de boot; botão BOOT e DTR ([DS Tab. 3-1, 3-3][esp32ds]; [esptool][esptool]) | ✘ | ✘ | ✘ |
| 1, 3 | ie, wpu | ie, wpu | UART0: log da ROM e gravação ([DS §3.3][esp32ds]; [GPIO IDF][idfgpio]) | ✘ | ✘ | ✘ |
| 2 | ie, **wpd** | ie, wpd | *strapping* do modo de boot; SD_DATA0 ([DS Tab. 2-1, 3-1][esp32ds]) | ✘ | ✘ | ✘ |
| 4 | ie, **wpd** | ie, wpd | SD_DATA1 | ✘ (o pull-down puxa o TXD para dominante) | evitar | ✘ |
| 5 | ie, wpu | ie, wpu | *strapping* do timing do SDIO ([DS Tab. 3-5][esp32ds]); uma fonte secundária relata pulsos no boot ([RNT][rnt]) | evitar | evitar | ✘ |
| 6 a 11 | wpu | wpu | flash SPI ([DS §2.3.1][esp32ds]; [DevKitC][devkitc]) | ✘ | ✘ | ✘ |
| 12 | ie, **wpd** | ie, wpd | *strapping* MTDI, que define a tensão da flash, e JTAG ([DS §3.2][esp32ds]; [esptool][esptool]) | ✘ | ✘ | ✘ |
| 13 | sem pull | ie, **wpd** | JTAG MTCK; SD_DATA3 | ✘ | evitar | ✘ |
| 14 | sem pull | ie, **wpu** | JTAG MTMS; SD_CLK; fonte secundária relata pulsos ([RNT][rnt]) | evitar | evitar | ✘ |
| 15 | ie, wpu | ie, wpu | *strapping* MTDO, que cala o log da ROM; JTAG; SD_CMD ([DS §3.3][esp32ds]) | evitar | evitar | ✘ |
| 16, 17 | sem pull | ie | livres no WROOM, reservados no WROVER ([DevKitC][devkitc]; [GPIO IDF][idfgpio]); candidatos à UART2 da Pi | ok | ok | ok |
| 18, 19, 21, 22, 23 | sem pull | ie, sem pull | livres. Os pinos 21 e 22 já são o I²C da IMU ([PLANO][plano]) | ok | ok | ok |
| **25, 26, 27** | **sem pull, ie=0** | **sem pull, ie=0** | livres, sem função de boot | **25** | **26** | **27** |
| 32, 33 | sem pull | sem pull | livres; o cristal de 32 kHz não é usado no DevKitC | ok | ok | ok |
| 34 a 39 | sem pull | sem pull | só entrada, sem pull ([GPIO IDF][idfgpio]) | ✘ | ok | ✘ |

O TWAI pode usar qualquer GPIO pela matriz de GPIO ([DS Tab. 4-6][esp32ds]). O plano já usa TX = GPIO25 e RX = GPIO26 ([PLANO][plano]), o que está correto. O GPIO27 fica vizinho deles no conector J2 do DevKitC, nos pinos 9 a 11 ([DevKitC][devkitc]).

## Circuito recomendado

### Com TJA1051T/3 (por exemplo, o Adafruit CAN Pal)

```
                     3V3 do ESP32
                      │         │
                    [R1]      [R2]          R1 = R2 = 10 kΩ
                      │         │
GPIO25 (TWAI TX) ─────●─────────┼──────────► TXD  ─┐
GPIO26 (TWAI RX) ◄──────────────┼─────────── RXD   │
GPIO27 ────o JP1 o──────────────●──────────► S     │  TJA1051T/3
3V3 ───────────────────────────────────────► VIO   │  (VCC = 5 V; no CAN Pal os 5 V
GND ───────────────────────────────────────► GND  ─┘   saem de uma bomba de carga)
                                             CANH ──► CANH do conector OBD-II
                                             CANL ──► CANL do conector OBD-II

JP1 aberto  → S fica em 3,3 V → SILENCIOSO sempre, seja qual for o firmware.
JP1 fechado → GPIO27 em alta impedância ou 1 → silencioso; GPIO27 = 0 → normal.
Terminação de 120 Ω: DESLIGADA no carro; LIGADA nas duas pontas da bancada.
```

- **R2 (S para 3,3 V), 10 kΩ.** O TJA1051 puxa o S para baixo com no máximo 10 µA. Isso dá uma queda de 0,1 V, e o S fica perto de 3,2 V, bem acima do VIH de 0,7·VIO = 2,31 V ([Tab. 7][nxp1051]). Mesmo que algum firmware ligue o pull-down interno do ESP32 (cerca de 45 kΩ), o S fica em 3,3 × 45/55 ≈ 2,7 V, ainda silencioso. O valor está na faixa de 1 a 10 kΩ que a TI recomenda para polarizar pinos ([SN65HVD230 §13.1][ti230]). Para liberar, o GPIO só precisa de VOL ≤ 0,1·VDD = 0,33 V ([DS Tab. 5-3][esp32ds]), abaixo de 0,3·VIO = 0,99 V.
- **R1 (TXD para 3,3 V), 10 kΩ.** É uma segunda camada, para o TXD não depender só do pull-up interno, que é fraco.
- **JP1** é um jumper de dois pinos. É a única proteção contra um firmware errado que force o GPIO27 a nível baixo. No carro, ele só é fechado depois que o firmware certo for confirmado.
- **No CAN Pal** o S vai direto ao conector, sem resistor ([esquema][canpalsch]). Sem o R2, o pull-down interno do chip deixaria o transceptor em modo **normal** por padrão. A placa tem uma chave de terminação ([Adafruit][canpal]). O nó do OBD é uma derivação do barramento, e a terminação fica só nas pontas ([SN65HVD230 §11.2.1.1][ti230]).

### Variante com SN65HVD230 (plano B)

```
                     3V3
                      │         │
                    [R1]      [R3]          R1 = 10 kΩ, R3 = 4,7 kΩ
                      │         │
GPIO25 (TWAI TX) ─────●─────────┼──────────► D  (pino 1)
GPIO26 (TWAI RX) ◄──────────────┼─────────── R  (pino 4)
GPIO27 ────o JP1 o──────────────●──────────► Rs (pino 8)
```

- Rs ≥ 0,75·VCC = 2,475 V liga o standby; Rs < 1,2 V liga o modo rápido ([§10.4.1, §10.4.3][ti230]).
- Com 4,7 kΩ, mesmo contra o pull-down interno do ESP32, Rs ≈ 3,3 × 45/49,7 ≈ 3,0 V. Com 10 kΩ seriam só 2,7 V, com pouca margem.
- Com o GPIO em 0, ele absorve 0,7 mA do resistor mais até 0,45 mA que saem do Rs ([§8.10, I(Rs)][ti230]). Não use resistor em série entre o GPIO e o Rs: ele somaria tensão e poderia passar de 1,2 V.
- **Retrabalho nos módulos comuns:** a placa da Waveshare liga o Rs ao GND por 10 kΩ, não leva o Rs ao conector e tem 120 Ω fixos entre CANH e CANL ([esquema Waveshare][wsch]). É preciso retirar esses dois resistores, fazer a terminação ao lado do carro e soldar um fio no pino 8.

### Como o firmware libera o TX

1. **Primeira coisa no boot:** colocar a linha de liberação em silencioso. O jeito mais seguro é o GPIO27 como saída *open-drain*, chamando `gpio_set_level(27, 1)` **antes** do `gpio_config()`. Assim o pino nunca pulsa em 0 na configuração. Isso encurta a janela que sobra depois de um reset de CPU. Para encurtá-la ainda mais, o mesmo pode ser feito num *hook* do bootloader ([ESP-IDF, Custom Bootloader][idfboot]).
2. **Escuta:** criar o nó TWAI a 500 kbit/s com `flags.enable_listen_only = true`. Nesse modo o controlador "only receives and does not transmit any dominant bits, including ACK and error frames" ([TWAI IDF][idftwai]). No driver antigo, o equivalente é `TWAI_MODE_LISTEN_ONLY` ([TWAI IDF v5.4][idftwai54]). O controlador deve ficar em listen-only enquanto o transceptor estiver silencioso. O motivo é que o controlador espera que o transceptor devolva o TX no RX ([TWAI IDF v5.4][idftwai54]); em modo normal, ele veria erros de bit no próprio ACK.
3. **Checagem:** escutar por um tempo fixo, por exemplo 1 a 2 s.
   - **Abortar** se o RX ficar em nível baixo contínuo, sinal de barramento preso em dominante. Com o TCAN337, abortar também se o FAULT subir, porque o timeout do RXD esconde um barramento preso ([§6.3.2, §6.3.5][tcan33x]).
   - Quadros válidos recebidos confirmam a velocidade de 500 kbit/s.
   - **Barramento mudo também é aceitável**: muitos carros deixam o conector mudo por causa de um gateway ([ADR-0001][adr1]). Nesse caso a velocidade não está confirmada, e a primeira requisição faz esse papel.
   - Os contadores de erro ficam congelados em listen-only ([TRM §25.5.1.2][esp32trm]). Por isso a checagem conta quadros válidos, e não o REC.
4. **Liberar o transceptor primeiro:** GPIO27 = 0. O TWAI ainda está em listen-only, então o TX segue recessivo e nada vai ao barramento. Esperar pelo menos 10 µs, que é o tMODE máximo do TCAN33x ([§5.6][tcan33x]); o t(WAKE) do SN65HVD230 é de até 1,5 µs ([§8.10][ti230]). O datasheet do TJA1051 não dá esse tempo.
5. **Depois o TWAI:** chamar `twai_node_disable()` e `twai_node_delete()`, criar o nó de novo sem listen-only e chamar `twai_node_enable()` ([TWAI IDF][idftwai]). Ao voltar a operar, o controlador espera 11 bits recessivos antes de entrar no barramento ([TRM §25.5.1.2][esp32trm]).
6. **Primeira requisição** da lista fechada, seguida de vigilância com `twai_node_get_info()` (estado, contador de erros de TX e de RX). Diante de *error passive*, *bus-off*, ausência de resposta repetida ou violação da lista, **GPIO27 = 1 primeiro** e só depois o TWAI volta a listen-only.
7. **Antes de reiniciar:** registrar com `esp_register_shutdown_handler()` uma rotina que faz GPIO27 = 1. Ela só cobre o `esp_restart()`, não o *panic* ([esp_system.c][idfesys]).
8. **O que evitar:**
   - não usar `gpio_hold_en()` no GPIO27, porque o *hold* sobrevive ao core reset ([TRM §6.7][esp32trm]);
   - não gerar a liberação com LEDC ou RMT, porque esses periféricos não estão na lista do que o `esp_restart_noos()` reinicia ([system_internal.c][idfsysint]);
   - manter o *panic* configurado para reiniciar (`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT`), e não para parar a CPU ([panic.c][idfpanicc]).

## Achados com fontes

### SN65HVD23x (TI, SLOS346O, abril de 2018) — [datasheet][ti230]

1. O Rs (pino 8) tem três modos ([Pin Functions, §7][ti230]):
   - "strong pull down to GND = high speed mode";
   - "10kΩ to 100kΩ pull down to GND = slope control mode": 10 kΩ dão cerca de 15 V/µs e 100 kΩ dão cerca de 2 V/µs ([§5, §10.4.2][ti230]);
   - "strong pull up to VCC = low power mode".
2. No SN65HVD230 o modo de baixo consumo é um "low-current, listen only standby mode, during which the driver is switched off and the receiver remains active … the transceiver is completely passive to the bus" ([§10.4.3][ti230]). A Tabela 2 marca o receptor como "Enabled (ON)" e o RXD como "Mirrors Bus State". A Tabela 4 trata o Rs como irrelevante para o receptor.
3. Os limites são V(Rs) ≥ 0,75·VCC para standby ([§8.3][ti230]) e < 1,2 V para o modo rápido ([§10.4.1][ti230]). A faixa intermediária é "?" (indeterminada) para o receptor do HVD231 ([Tab. 5][ti230]) e não é coberta pela tabela do driver ([Tab. 3][ti230]).
4. **Nenhum tempo do receptor é dado com Rs alto.** A Tabela §8.8 (tPLH e tPHL de 35 a 50 ns) não tem condição de Rs, e as curvas típicas usam "RS = 0" ([§8.11][ti230]). O texto descreve o standby como um modo para "monitor the bus for activity" e voltar a transmitir ([§5, §10.4.3][ti230]).
5. Num fórum da TI, um usuário não recebeu nada em standby. O engenheiro da TI respondeu "With this device the receiver should remain on in standby mode" e o tópico ficou sem conclusão ([E2E 1161897][e2e]). Uma interpretação nossa: o usuário era o único receptor, e em standby ninguém dava ACK.
6. O HVD231 desliga driver **e** receptor quando o Rs está alto, e o R fica em nível alto ([§10.4.5][ti230]). O HVD232 não tem modo: "pins 5 and 8 are no connection" ([§10.4.6][ti230]).
7. **TXD (pino D):** a tabela do driver dá "Open → Z Z → Recessive" ([Tab. 3][ti230]). A corrente de entrada do D é negativa, ou seja, sai do pino, com mínimo de −30 µA ([§8.5][ti230]) e curva típica até cerca de −16 µA ([Fig. 2][ti230]): é um pull-up interno fraco. A TI recomenda resistor externo de 1 a 10 kΩ porque "the internal pull up and pull down biasing of the device is weak" ([§13.1][ti230]).
8. **Não há timeout de dominante.** Nem as especificações nem o texto o citam. A proteção contra "babbling idiot" depende de o microcontrolador colocar o Rs em nível alto ([§10.4.4][ti230]).
9. Estimativa nossa: com 45 kΩ de pull-down do ESP32 contra cerca de 15 µA do pull-up interno, o D fica perto de 0,7 V, abaixo do VIL de 0,8 V ([§8.3][ti230]). Ou seja, dominante contínuo enquanto durar o reset, se o transceptor estiver em modo rápido.
10. Os pinos do barramento ficam em alta impedância com VCC baixo ("glitch-free power-up and power-down") ([§10.3][ti230]). O t(WAKE) do standby é de até 1,5 µs ([§8.10][ti230]).

### TJA1051 (NXP, Rev. 9, 28/11/2017) — [datasheet][nxp1051]

11. "A HIGH level on pin S selects Silent mode. In Silent mode the transmitter is disabled, releasing the bus pins to recessive state. All other IC functions, including the receiver, continue to operate as in Normal mode" ([§7.1.2][nxp1051]). Os atrasos do barramento ao RXD têm valores típicos de 60 e 65 ns para "Normal/Silent mode" ([Tab. 8][nxp1051]).
12. "Pin TXD has an internal pull-up to VIO and pins S and EN … have internal pull-downs to GND" ([§7.2.2][nxp1051]). **Portanto, S aberto significa modo normal**, e é preciso um pull-up externo. A corrente do S em nível alto vai de 1 a 10 µA; o VIH é 0,7·VIO ([Tab. 7][nxp1051]).
13. Timeout de dominante no TXD: "If the LOW state on pin TXD persists for longer than tto(dom)TXD, the transmitter is disabled" ([§7.2.1][nxp1051]). O tempo vai de 0,3 a 5 ms no modo normal ([Tab. 8][nxp1051]).
14. O TJA1051T/3 tem VIO de 2,8 a 5,5 V para interface direta com microcontroladores de 3 V, mas o VCC continua de 4,5 a 5,5 V ([§7.3, Tab. 7][nxp1051]). Com VCC ou VIO abaixo do limite de subtensão, o transceptor se desliga do barramento ([§7.2.3][nxp1051]).
15. A página do produto mostra o TJA1051 como "Active", mas indica o TJA1057GT/3 como "New Generation Recommendation" para o TJA1051T/3 ([NXP][nxp1051p]). O TJA1057 não foi analisado aqui.

### MCP2561/2 (Microchip, DS20005167C) — [datasheet][mcp2562]

16. No standby, "the transmitter and the high-speed part of the receiver are switched off … The receive pin (RXD) will show a delayed representation of the CAN bus, due to the wake-up filter" ([§1.1.2][mcp2562]). O tFLTR(WAKE) vai de 0,5 a 4 µs ([§2.3, parâm. 9][mcp2562]).
17. O STBY tem "internal MOS pull-up", de 1,1 MΩ típico a 3,3 V, e o TXD tem pull-up de 33 kΩ ([§1.7.1, §1.7.9][mcp2562]). Há detecção de dominante permanente no TXD, com cerca de 1,25 ms ([§1.5][mcp2562]).

### TCAN33x (TI, SLLSEQ7F, maio de 2025) e TCAN1051 (SLLSES8E) — [TCAN33x][tcan33x], [TCAN1051][tcan1051]

18. O TCAN330 e o TCAN337 têm o pino S de *Silent*, com alimentação única de 3,3 V ([Device Options, §5.3][tcan33x]). Sobre o modo: "The CAN driver is disabled but the receiver is fully operational" ([§6.4.3][tcan33x]).
19. Polarização interna: TXD com pull-up e S com pull-down, que "weakly biases S terminal towards normal mode". A TI avisa: "The internal bias should not be relied on by design" ([§6.3.6, Tab. 6-2][tcan33x]).
20. O timeout de dominante do TXD vai de 1,2 a 3,8 ms; o do RXD, de 1,6 a 5 ms; o pino FAULT do TCAN337 avisa quando algum deles atua. A troca de modo leva até 10 µs ([§5.6, §6.3.1, §6.3.2, §6.3.5][tcan33x]).
21. No TCAN1051 com sufixo V: "The high speed receiver remains active" em Silent ([§8.4.3][tcan1051]). O TXD tem pull-up e o S tem pull-down ([§8.3.5][tcan1051]). O timeout de dominante vai de 1,2 a 3,8 ms ([§6.8][tcan1051]). Precisa de VCC de 5 V.

### ESP32 (Espressif) — [datasheet v5.3][esp32ds], [TRM v5.8][esp32trm]

22. Os pinos de *strapping* e seus padrões são: GPIO0 com pull-up, GPIO2 com pull-down, MTDI (GPIO12) com pull-down, MTDO (GPIO15) com pull-up e GPIO5 com pull-up. Eles são lidos no reset e "freed up to be used as regular IO pins after reset" ([DS §3, Tab. 3-1][esp32ds]).
23. "During reset, all pins are output-disabled" ([DS Tab. 6-1, nota 9][esp32ds]). Os estados durante e logo depois do reset estão na tabela IO_MUX ([DS A.4][esp32ds]). Os pulls internos são de cerca de 75 µA ([nota 8][esp32ds]), e o esptool fala em cerca de 45 kΩ ([esptool][esptool]).
24. O modo de *download* "conjunto" inclui o SDIO além da UART ([DS Tab. 3-3][esp32ds]). O MTDO em nível baixo cala o log da ROM na U0TXD ([DS Tab. 3-4][esp32ds]).
25. Níveis de reset: "CPU reset: Only resets the registers of one or both of the CPU cores"; "Core reset: Resets all the digital registers, including … external GPIO and digital GPIO. The RTC is not reset"; "System reset: Resets all the registers on the chip" ([TRM §7.1.1][esp32trm]). O *pin hold* mantém o estado do pino durante um core reset ([TRM §6.7][esp32trm]).
26. TWAI em Listen Only: "will not be able to transmit any messages, acknowledgments, or error signals. The error counters will remain frozen". Ao entrar em operação, o controlador "must wait for 11 consecutive Recessive bits" ([TRM §25.5.1.2][esp32trm]).
27. Os sinais twai_tx e twai_rx podem ir para "Any GPIO Pins" ([DS Tab. 4-6][esp32ds]). As entradas digitais têm VIH de 0,75·VDD e VOL de até 0,1·VDD ([DS Tab. 5-3][esp32ds]).

### ESP-IDF (código na branch master, consultado em 13/10/2026, e documentação v6.1)

28. A função `esp_restart_noos()` reinicia as CPUs com `esp_rom_software_reset_cpu()`. Antes disso, ela reinicia só Wi-Fi/BT/SDIO/EMAC, timers, SPI, UART, MCPWM, I2S, UHCI e cripto ([system_internal.c][idfsysint]). O `panic_restart()` chama essa mesma função; a exceção é o erro de cache, que usa `esp_restart_noos_dig()` ([panic_handler.c][idfpanic]).
29. Depois de um reset só de CPU, o boot preserva os clocks que estavam ligados ("For reason that only reset CPU, do not disable the clocks that have been enabled before reset"). Num boot a frio, ele desligaria o `DPORT_TWAI_CLK_EN` ([clk.c][idfclk]).
30. O `esp_register_shutdown_handler()` roda no `esp_restart()` ([esp_system.c][idfesys]). O caminho do *panic* não passa por ele ([panic_handler.c][idfpanic]).
31. O resumo de GPIOs da documentação lista os *strapping* (0, 2, 5, 12 e 15), o SPI0/1 (6 a 11 e 16 a 17), o JTAG (12 a 15), a UART0 (1 e 3) e os pinos só de entrada (34 a 39) ([GPIO IDF][idfgpio]).
32. Driver novo do TWAI: `flags.enable_listen_only`, `io_cfg.tx` e `io_cfg.rx`, além de `twai_node_enable`, `twai_node_disable`, `twai_node_delete` e `twai_node_get_info`. O modo *self-test* não exige ACK ([TWAI IDF][idftwai]). O driver antigo tinha `TWAI_MODE_LISTEN_ONLY`, e reinstalar exigia `twai_driver_uninstall()` ([TWAI IDF v5.4][idftwai54]).
33. O bootloader aceita *hooks*: "custom_bootloader/bootloader_hooks presents how to connect some hooks to the bootloader initialization" ([ESP-IDF Bootloader][idfboot]).

### Módulos

34. O Adafruit CAN Pal (produto 5708) usa o TJA1051T/3 e tem uma "5V charge-pump", então dispensa uma fonte separada de 5 V ([Adafruit][canpal]). O pino SLNT "can be pulled high to put the TJA1051/3 transceiver into silent mode" ([pinagem][canpalpins]). No esquema rev. B, o S vai direto ao conector e o VIO vai ao VCC da placa ([downloads][canpalsch]). A placa tem uma chave para os 2 × 60 Ω de terminação. O preço é US$ 3,95 ([Adafruit][canpal]).
35. Na placa SN65HVD230 da Waveshare, o Rs vai ao GND por 10 kΩ, o que dá *slope control*, e há 120 Ω fixos entre CANH e CANL ([esquema][wsch]).

## Riscos e incertezas

- **SN65HVD230 em standby a 500 kbit/s.** O datasheet diz que o receptor fica ativo, mas não caracteriza a recepção nesse modo, e há um relato sem solução no fórum da TI. Confiança média. Para confirmar, basta comparar com um analisador lógico o RXD em standby e em modo rápido, com o outro ESP32 transmitindo a 500 kbit/s.
- **Janela depois de `esp_restart()` ou *panic*.** O resistor não protege porque os GPIOs não são reiniciados. A janela vai do *panic* até o novo firmware reconfigurar o GPIO27: inclui imprimir o *backtrace*, o bootloader e o início do app. Ela não foi medida.
  - Nesse intervalo, o controlador TWAI, que também não é reiniciado, pode retransmitir sozinho um quadro pendente. O CPU não envia nada novo, e o quadro pendente já passou pela lista fechada.
  - O *hook* no bootloader e a configuração do pino no início do app encurtam a janela, mas não a eliminam.
  - Para fechá-la de vez, seria preciso uma liberação "dinâmica" no hardware: uma bomba de carga com MOSFET que só mantém o S baixo enquanto o software alterna o GPIO, e que solta sozinha quando o CPU para. Isso não foi validado e é opcional.
- **Comportamento da ROM num reset de CPU.** Não está documentado se a ROM mexe em algum GPIO nesse caso. Como 25, 26 e 27 não são pinos de boot, é improvável, mas falta medir.
- **Checagem com o barramento mudo.** Com gateway, o carro pode não ter tráfego espontâneo ([ADR-0001][adr1]). Nesse caso, a escuta só prova que o barramento não está preso, e a velocidade só se confirma na primeira requisição. Os contadores de erro ficam congelados em listen-only ([TRM][esp32trm]), e a documentação não diz se o `bus_err_num` do ESP-IDF conta erros nesse modo.
- **Bancada com dois nós.** Com o ESP32 silencioso ou em listen-only, ninguém dá ACK aos quadros espontâneos da ECU simulada. Eles ficam sendo retransmitidos. A ECU simulada deve usar *self-test* para esses quadros ([TWAI IDF][idftwai]), ou a bancada precisa de um terceiro nó. É provável que o relato do fórum da TI seja esse caso.
- **Módulos.**
  - Os módulos genéricos de SN65HVD230 prendem o Rs ao GND e têm 120 Ω fixos, e por isso exigem retrabalho.
  - O CAN Pal precisa do pull-up externo no SLNT.
  - Não confirmamos se o CAN Pal ou chips TJA1051T/3 avulsos chegam ao Brasil antes do teste no carro (25/11). Também existem clones de outros fabricantes com nome parecido (HGSEMI, JSMSEMI), não avaliados.
- **Ciclo de vida.** A NXP indica o TJA1057GT/3 como substituto do TJA1051T/3. Não verificamos se ele tem *Silent mode* com o mesmo comportamento.
- **Valores aproximados.** Os pulls internos do ESP32 são aproximados (cerca de 75 µA, ou 45 kΩ). Os cálculos de margem usam esses valores.
- **Pulsos no boot.** Os relatos de pulsos nos GPIO5, 14 e 15 vêm de fonte secundária sem referência ([RNT][rnt]). Evitamos esses pinos de qualquer forma.
- **Versão do ESP-IDF.** Lemos o código da branch master. O comportamento de reinício pode mudar entre versões, então vale conferir os mesmos arquivos na versão que o PlatformIO instalar.

## Recomendação para o ticket

1. **Comprar 3 transceptores TJA1051T/3 com o pino S acessível**, um para o carro, um para o adaptador USB-CAN da bancada e um de reserva. Opções:
   - O Adafruit CAN Pal, se chegar a tempo: roda só com 3,3 V e tem chave de terminação.
   - O TCAN337 ou o TCAN330 em SOIC-8 sobre um adaptador para DIP, se a equipe preferir montar a placa: 3,3 V puro, Silent mode, timeouts e FAULT.
   - **Plano B:** o SN65HVD230, aceitando o retrabalho (tirar o 10 kΩ do Rs e o 120 Ω, ligar um fio no Rs), a ausência de timeout de dominante e a validação do standby a 500 kbit/s na bancada.
   - **Não comprar** SN65HVD231, SN65HVD232 nem MCP2562.
2. **Circuito.**
   - Pinos: TX = GPIO25, RX = GPIO26, liberação = GPIO27.
   - Resistores: 10 kΩ do S para 3,3 V (4,7 kΩ no Rs, no plano B) e 10 kΩ do TXD para 3,3 V.
   - Jumper JP1 em série com o GPIO27.
   - Terminação desligada no carro.
   - O GPIO27 precisa entrar na pinagem do `PLANO.md`. Isso fica para quem cuida do plano; este ticket não altera outros arquivos.
3. **Firmware** (entra na tarefa do `TwaiBus`), seguindo os passos 1 a 8 acima:
   - silencioso como primeira ação do boot;
   - TWAI em listen-only durante a escuta;
   - baixar o S antes de passar o TWAI ao modo normal;
   - em qualquer anomalia, subir o S primeiro;
   - rotina de *shutdown* para o `esp_restart()`;
   - não usar *hold* nem LEDC no GPIO27.
4. **Testes de bancada para fechar o ticket.** Medir no osciloscópio o S e o CANH−CANL nestes casos:
   - (a) ao ligar a placa;
   - (b) com o botão EN;
   - (c) durante a gravação pelo esptool;
   - (d) com `esp_restart()`;
   - (e) com `abort()`, medindo a janela;
   - (f) com o JP1 aberto e o firmware forçando o GPIO27 a 0 enquanto o TWAI transmite. O barramento deve continuar recessivo.

   Com o analisador lógico, confirmar ainda que o RXD em modo silencioso é idêntico ao do modo normal a 500 kbit/s.
5. **Registrar a decisão.** Vale um ADR, "transceptor silencioso por padrão, liberado pelo firmware", que cite este arquivo.

[ti230]: https://www.ti.com/lit/ds/symlink/sn65hvd230.pdf
[ti235]: https://www.ti.com/lit/ds/symlink/sn65hvd235.pdf
[e2e]: https://e2e.ti.com/support/interface-group/interface/f/interface-forum/1161897/sn65hvd230-how-to-use-standby-mode
[nxp1051]: https://www.nxp.com/docs/en/data-sheet/TJA1051.pdf
[nxp1051p]: https://www.nxp.com/products/TJA1051
[mcp2562]: https://ww1.microchip.com/downloads/en/DeviceDoc/20005167C.pdf
[tcan33x]: https://www.ti.com/lit/ds/symlink/tcan337.pdf
[tcan1051]: https://www.ti.com/lit/ds/symlink/tcan1051h.pdf
[esp32ds]: https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
[esp32trm]: https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf
[idfgpio]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html
[idftwai]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html
[idftwai54]: https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/peripherals/twai.html
[idfboot]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/bootloader.html
[esptool]: https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html
[devkitc]: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html
[idfsysint]: https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/soc/esp32/system_internal.c
[idfpanic]: https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/panic_handler.c
[idfpanicc]: https://github.com/espressif/esp-idf/blob/master/components/esp_system/panic.c
[idfesys]: https://github.com/espressif/esp-idf/blob/master/components/esp_system/esp_system.c
[idfclk]: https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/soc/esp32/clk.c
[canpal]: https://www.adafruit.com/product/5708
[canpalpins]: https://learn.adafruit.com/adafruit-can-pal/pinouts
[canpalsch]: https://learn.adafruit.com/adafruit-can-pal/downloads
[wsch]: https://files.waveshare.com/upload/c/c5/SN65HVD230-CAN-Board-Schematic.pdf
[rnt]: https://randomnerdtutorials.com/esp32-pinout-reference-gpios/
[adr1]: ../adr/0001-obd-por-requisicao-padronizada.md
[plano]: ../PLANO.md
