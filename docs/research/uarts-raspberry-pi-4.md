# UARTs extras da Raspberry Pi 4 para os fios do ESP32

Pesquisa do ticket #5 (parte do mapa #1, decisão D13). Feita em 13/10/2026.

As fontes primárias foram:

- a documentação oficial da Raspberry Pi;
- o `README` dos overlays (`raspberrypi/firmware`);
- o device tree e o driver PL011 do kernel `raspberrypi/linux` (ramo `rpi-6.12.y`; os mesmos trechos foram conferidos no `rpi-6.18.y`, o ramo padrão hoje);
- o documento de periféricos do BCM2711, o datasheet e o esquema reduzido da Pi 4;
- para o lado do ESP32, o manual técnico da Espressif e o ESP-IDF.

Cada afirmação traz a fonte entre colchetes, como [S1]. A lista completa está no fim da seção "Achados com fontes".

## Pergunta

Quais UARTs da **Raspberry Pi 4** podem receber os 4 fios do ESP32 (TX, RX, GND e 5 V) sem conflitar com:

- o Bluetooth;
- o GPS (UART e PPS);
- a câmera CSI;
- o I²C?

Para a UART escolhida: quais pinos, qual `dtoverlay`, qual nome em `/dev` e qual velocidade máxima confiável? E o ESP32 pode ser alimentado pelo pino de 5 V da Pi?

## Resposta curta

- **UARTs da Pi 4.** São seis: cinco PL011 (UART0, 2, 3, 4 e 5) e um mini UART (UART1) [S13][S1].
  - Por padrão, o UART0 fica com o Bluetooth e o mini UART fica nos GPIO 14/15 do conector [S1][S17].
  - UART2 a UART5 vêm desligados. Cada um é ligado com `dtoverlay=uartN`, num par fixo de GPIO [S12].
- **Recomendação:**
  - **GPS no UART3**: GPIO 4/5, pinos 7/29, `/dev/ttyAMA3`;
  - **PPS no GPIO 18**: pino 12, `dtoverlay=pps-gpio`;
  - **ESP32 no UART5**: GPIO 12/13, pinos 32/33, `/dev/ttyAMA5`.

  Assim nada encosta no Bluetooth, no console dos GPIO 14/15, no I²C1 (GPIO 2/3), no SPI0 (GPIO 7–11), na EEPROM de HAT (GPIO 0/1) nem na câmera.
- **Velocidade.** O PL011 vai até `UARTCLK/16` [S13]. O relógio de UART é de 48 MHz [S5], então o teto é 3 Mbaud. O próprio Bluetooth da Pi 4 roda a 3 Mbaud num PL011 [S18].
  - **921600 funciona**: o erro de divisor é de 0,16 % na Pi e 0,06 % no ESP32.
  - **1 000 000 baud** dá divisor exato nos dois lados. É a melhor escolha se quisermos passar de 460800.
- **5 V.** Alimentar o ESP32 (~0,1 A) pelos pinos 2/4 cabe na folga da fonte de 3 A, mas tem dois cuidados:
  - esses pinos são o mesmo nó da entrada USB-C, sem fusível [S15];
  - o ESP32 não pode ficar ligado ao mesmo tempo no 5 V da Pi e no USB do notebook [S30].

## Mapa de UARTs e pinos

Os pinos físicos são do conector de 40 pinos (J8) [S15]. As funções vêm da tabela de funções alternativas do BCM2711 [S13][S14] e dos overlays [S12].

| UART | Tipo | Como ativar na Pi 4 | TX (GPIO · pino) | RX (GPIO · pino) | CTS/RTS (só com `ctsrts`) | Dispositivo | Conflitos no V-GUARD |
|---|---|---|---|---|---|---|---|
| UART0 | PL011 | Por padrão vai para o Bluetooth (GPIO 30–33, internos). `disable-bt` ou `miniuart-bt` trazem para os GPIO 14/15 | 14 · 8 | 15 · 10 | — | `/dev/ttyAMA0` (com `disable-bt`/`miniuart-bt`) | Exige tirar o Bluetooth do PL011 |
| UART1 | mini UART | Primário padrão (`/dev/serial0`); só funciona com `enable_uart=1` | 14 · 8 | 15 · 10 | — | `/dev/ttyS0` | Baud preso ao clock do núcleo; evitar |
| UART2 | PL011 | `dtoverlay=uart2` | 0 · 27 | 1 · 28 | 2/3 · 3/5 | `/dev/ttyAMA2` | GPIO 0/1 reservados à EEPROM de HAT e ao mux de I²C da câmera; CTS/RTS em cima do I²C1 |
| UART3 | PL011 | `dtoverlay=uart3` | 4 · 7 | 5 · 29 | 6/7 · 31/26 | `/dev/ttyAMA3` | Só 1-Wire (`w1-gpio`) e `i2c3`, que não usamos |
| UART4 | PL011 | `dtoverlay=uart4` | 8 · 24 | 9 · 21 | 10/11 · 19/23 | `/dev/ttyAMA4` | Ocupa o SPI0 (CE0 e MISO) |
| UART5 | PL011 | `dtoverlay=uart5` | 12 · 32 | 13 · 33 | 14/15 · 8/10 | `/dev/ttyAMA5` | Só o PWM de hardware dos GPIO 12/13 e o `i2c5`, que não usamos |

- **Overlays `uart0` e `uart1`.** Esses dois não ligam UART nova: só trocam os pinos.
  - `uart0` aceita TX nos GPIO 14, 32 ou 36.
  - `uart1` aceita TX nos GPIO 14, 32 ou 40.
  - Os GPIO 32 em diante não estão no conector [S12][S15].
- **Nomes em `/dev`.** Desde o kernel 6.1 (Raspberry Pi OS Bookworm em diante), o device tree da Pi 4 tem os apelidos fixos `serial2`…`serial5` para UART2…UART5 [S16][S23].
  - O driver PL011 usa o número do apelido como número do `ttyAMA` [S22].
  - Por isso, o UART3 é sempre `/dev/ttyAMA3` e o UART5 é sempre `/dev/ttyAMA5`, não importa quais outros estejam ligados.
  - Os atalhos `/dev/serial0` e `/dev/serial1` só existem para o UART primário e o secundário [S24].

Outros pinos que entram na conta:

| Função | GPIO | Pino | Fonte |
|---|---|---|---|
| PPS (`pps-gpio`, padrão) | 18 | 12 | [S12] |
| I²C1 (`dtparam=i2c_arm=on`), SDA/SCL | 2 / 3 | 3 / 5 | [S10][S15] |
| SPI0 (MOSI, MISO, SCLK, CE0, CE1) | 10, 9, 11, 8, 7 | 19, 21, 23, 24, 26 | [S10][S15] |
| EEPROM de HAT (ID_SD / ID_SC) | 0 / 1 | 27 / 28 | [S15] |
| I²C da câmera CSI | 44 / 45 | fora do conector | [S12] |
| 5 V | — | 2, 4 | [S15] |
| 3,3 V | — | 1, 17 | [S15] |
| GND | — | 6, 9, 14, 20, 25, 30, 34, 39 | [S15] |

## Configuração recomendada

### `/boot/firmware/config.txt`

No Raspberry Pi OS atual, o arquivo fica em `/boot/firmware/config.txt` [S1]. O `raspi-config` também usa esse caminho quando ele existe [S27].

As linhas vão no **fim** do arquivo, depois de um `[all]`. Assim nenhum filtro anterior, como `[cm4]`, as limita a outro modelo [S7].

```ini
[all]
# V-GUARD: GPS no UART3. TX = GPIO 4 (pino 7), RX = GPIO 5 (pino 29) -> /dev/ttyAMA3
dtoverlay=uart3
# V-GUARD: ESP32 no UART5. TX = GPIO 12 (pino 32), RX = GPIO 13 (pino 33) -> /dev/ttyAMA5
dtoverlay=uart5
# V-GUARD: PPS do GPS no GPIO 18 (pino 12)
dtoverlay=pps-gpio,gpiopin=18
```

- **Não é preciso** `enable_uart`, `disable-bt` nem `miniuart-bt`. O Bluetooth continua no UART0, nos pinos internos [S17], e os overlays `uart3`/`uart5` só ligam o próprio UART [S21].
- **I²C1, se for preciso:** `dtparam=i2c_arm=on` (GPIO 2/3). Não conflita com nada acima [S10].

### Console serial

O console do kernel e o login pela serial seguem o `console=serial0,115200` do `cmdline.txt` [S8]:

- o `serial0` é sempre o UART primário, nos GPIO 14/15 [S1][S24];
- o systemd só abre login nos consoles do kernel [S26].

Por isso o console **nunca** cai no `ttyAMA3` nem no `ttyAMA5`. Mesmo assim, vale desligá-lo, para os GPIO 14/15 ficarem quietos e ninguém ter surpresa depois.

```bash
sudo raspi-config nonint do_serial_cons 1   # tira "console=serial0,115200" do /boot/firmware/cmdline.txt
sudo raspi-config nonint do_serial_hw 1     # grava enable_uart=0 (GPIO 14/15 sem uso)
sudo reboot
```

- **O que esses comandos fazem.** São as mesmas funções do menu `3 Interface Options > I6 Serial Port` [S2][S27]:
  - `do_serial_cons 1` apaga `console=serial0,<baud>` e `console=ttyAMA0,<baud>` do `cmdline.txt`;
  - `do_serial_hw` grava `enable_uart` no `config.txt`.
- **À mão:** editar `/boot/firmware/cmdline.txt`, apagar `console=serial0,115200` e manter tudo numa linha só [S8].
- **Se um dia o GPS for para os GPIO 14/15** (alternativa com `disable-bt`, ver "Riscos"), desligar o console passa a ser **obrigatório**: o `serial0` vira o `ttyAMA0` [S21]. Nesse caso a documentação manda responder "No" ao login pela serial e "Yes" ao hardware [S1].

### Fiação

ESP32 (os 4 fios do ticket):

| Fio | Pi 4 | ESP32 DevKit |
|---|---|---|
| TX da Pi → RX do ESP32 | pino 32 (GPIO 12, TXD5) | RX do UART do enlace (a definir no firmware, por exemplo o UART2) |
| RX da Pi ← TX do ESP32 | pino 33 (GPIO 13, RXD5) | TX do UART do enlace |
| GND | pino 34 (vizinho do 32/33) | GND |
| 5 V | pino 2 ou 4 | 5V |

GPS:

| Fio | Pi 4 |
|---|---|
| TX do GPS → RX da Pi | pino 29 (GPIO 5, RXD3) |
| RX do GPS ← TX da Pi | pino 7 (GPIO 4, TXD3) |
| PPS | pino 12 (GPIO 18) |
| GND | pino 6, 9 ou 14 |
| Alimentação | 3,3 V (pino 1) ou 5 V (pino 4), conforme o módulo |

- **Níveis lógicos.** Os dois lados precisam de lógica de 3,3 V. Todas as UARTs da Pi são de 3,3 V e 5 V nos pinos causa dano [S1]. TX e PPS do GPS precisam sair em 3,3 V.

### Conferência depois do boot

```bash
ls -l /dev/ttyAMA3 /dev/ttyAMA5 /dev/pps*   # os dois UARTs e o dispositivo PPS
sudo pinctrl get 4,5,12,13,18               # 4/5 e 12/13 em ALT4 (TXD3/RXD3, TXD5/RXD5)
vcgencmd measure_clock uart                 # esperado: cerca de 48000000
cat /proc/cmdline                           # não deve ter console=serial0
systemctl list-units 'serial-getty@*'       # não deve aparecer ttyAMA3 nem ttyAMA5
id                                          # o usuário do leitor precisa estar no grupo dialout
```

- O `pinctrl` substituiu o `raspi-gpio` [S31].
- O `vcgencmd measure_clock uart` mede o relógio de UART [S11].
- Os `/dev/tty[A-Z]*N` pertencem ao grupo `dialout` [S25].

## Alimentação de 5 V

**O que a Pi oferece:**

- Os pinos 2 e 4 estão ligados direto no nó `5V`. No esquema reduzido, esse nó é o próprio VBUS do USB-C [S15].
- Entre os dois, só há um diodo de proteção (TVS, D1, SMBJ5.0A) para o GND. Não aparece fusível nem limitador de corrente [S15].
- O máximo absoluto da entrada de 5 V é 6,0 V [S14].

**Orçamento de corrente, pior caso.** Os números são da documentação oficial [S9], mais o ESP32 do ticket:

| Carga | Corrente |
|---|---|
| Pi 4B sob estresse (máximo medido, sem USB nem HAT) | 1,25 A |
| Módulo de câmera | 0,25 A |
| Periféricos USB, como o modem 4G (limite total da Pi 4) | até 1,2 A |
| ESP32 (ticket) | ~0,1 A |
| **Total** | **~2,8 A**, contra 3,0 A da fonte recomendada |

- O ESP32 sozinho é pouco.
- A folga total é pequena, e a documentação cita modems como dispositivos de alto consumo [S9].
- O datasheet fala em ~1,1 A para o USB somado [S14], o que dá um pouco mais de folga.
- Conclusão: a fonte do carro (conversor 12 V → 5 V) precisa entregar de verdade 5,1 V e pelo menos 3 A [S9][S14].

**Cuidados:**

1. **Sem fusível.** Imagine o fio de 5 V do ESP32 soltar e encostar no chassi: o curto puxa direto da fonte. A Pi reinicia por queda de tensão, ou o fio esquenta.
   - Sugestão (de engenharia, não da documentação): um fusível rearmável (PTC) de ~0,5 A em série no fio de 5 V, perto da Pi.
2. **Nunca duas fontes no ESP32.** A Espressif diz que o DevKitC aceita uma, e só uma, destas fontes: USB, pino 5V ou pino 3V3. Caso contrário, a placa ou a fonte podem ser danificadas [S30].
   - Na prática: para regravar o ESP32 pelo notebook, **desconectar antes o fio de 5 V da Pi**.
   - Placas clones podem ter ou não um diodo nesse caminho. Não contar com ele.
3. **Ligam e desligam juntos.** Com o ESP32 alimentado pela Pi, as duas placas sobem e caem juntas. Assim o TX do ESP32 nunca fica em 3,3 V num pino de uma Pi desligada.
   - Isto é uma inferência, não está na documentação. É uma vantagem desta ligação.
4. **Não usar os pinos de 3,3 V para o ESP32.** A documentação descreve a fonte de 3,3 V como dimensionada para poucos mA por GPIO [S10]. O plano já usa o 5 V.

## Achados com fontes

### 1. UARTs da Pi 4 e seus pinos

- **Seis UARTs no BCM2711.** São um mini UART (UART1) e cinco PL011 (UART0, 2, 3, 4 e 5) [S13, §11.1]. A documentação da Pi confirma isso para a Pi 4, com UART2–5 "disabled by default" [S1].
- **Overlays dos UARTs extras.** `uart2`, `uart3`, `uart4` e `uart5` ligam esses UARTs na Pi 4 [S1]. O `README` dá os pinos [S12]:
  - `uart2`: GPIOs 0–3;
  - `uart3`: GPIOs 4–7;
  - `uart4`: GPIOs 8–11;
  - `uart5`: GPIOs 12–15;
  - todos "BCM2711 only".

  Os dois últimos de cada grupo são CTS/RTS e só são usados com o parâmetro `ctsrts` [S12][S21].
- **Função ALT4.** É a função de TXD/RXD 2…5 nesses GPIO [S13, tabelas 94 e 171][S14, tabela 5]. No device tree, cada `uartN_pins` põe o TX sem pull e o RX com pull-up [S16].
- **Pino físico de cada GPIO.** Vem do conector J8 no esquema reduzido [S15].
- **Nomes em `/dev`:**
  - os apelidos `serial2 = &uart2` … `serial5 = &uart5` estão em `bcm2711-rpi-ds.dtsi` [S16];
  - o driver usa `of_alias_get_id(np, "serial")` como número da porta, e a porta vira `ttyAMA<N>` [S22];
  - os apelidos entraram no commit "ARM: dts: bcm2711: Add extra serial aliases", de fev/2023, no ramo `rpi-6.1.y` [S23];
  - no 5.15 eles não existiam, e o driver pegava a primeira posição livre [S22].

### 2. Bluetooth e mini UART

- **Divisão padrão na Pi 3 e na Pi 4.** O primário (console) é o UART1 e o secundário (Bluetooth) é o UART0 [S1].
  - O device tree da Pi 4 diz "uart0 communicates with the BT module", nos GPIO 30–33, com CTS/RTS.
  - E diz "uart1 is mapped to the pin header", no GPIO 14 [S17].
- **Velocidade do Bluetooth.**
  - No UART0, o nó do Bluetooth tem `max-speed = <3000000>`.
  - No mini UART, a variante tem `max-speed = <230400>` [S18].
- **`disable-bt`.** Desliga o Bluetooth e devolve o UART0/`ttyAMA0` aos GPIO 14/15 [S12].
  - O overlay também desliga o mini UART e troca os apelidos: `serial0` passa a ser o UART0 [S21].
  - A documentação manda também `sudo systemctl disable hciuart` [S1].
- **`miniuart-bt`.** Passa o Bluetooth para o mini UART (`ttyS0`) e devolve o UART0 aos GPIO 14/15 [S12][S21].
  - Pede clock de núcleo fixo, com `core_freq=250` ou `force_turbo=1` [S1].
  - "uses a lower baudrate, and should only be used with low-bandwidth peripherals" [S12].
- **Bookworm em diante.** O Bluetooth é ligado pelo próprio kernel (`krnbt`), e o `/dev/serial1` pode não existir. `dtparam=krnbt=off` o traz de volta [S1][S12].
- **Por que o mini UART é ruim.**
  - Seu relógio vem do clock do núcleo (VPU). Quando a frequência do núcleo muda, o baud muda junto.
  - Por isso, com frequência variável, o firmware desliga o mini UART. `enable_uart=1` o liga fixando o núcleo; a tabela da documentação fala em 250 MHz [S1].
  - O núcleo da Pi 4 tem padrão de 500 MHz, com escala dinâmica [S6].
  - No silício: FIFO de só 8 posições, sem detecção de break, sem detecção de erro de quadro (framing), sem paridade e sem interrupção de timeout de recepção [S13, §2.2]. O baud é derivado do clock do sistema com sobreamostragem de 8× [S13, §2.2.1].
  - A documentação resume: o mini UART é "more prone to losing characters at higher baud rates" [S1].

### 3. Escolha dos UARTs e conflitos

- **UART2 descartado:**
  - O esquema da Pi 4 diz dos GPIO 0/1 (ID_SD/ID_SC): "DO NOT USE these pins for anything other than attaching an I2C ID EEPROM" [S15].
  - O firmware lê a EEPROM e o ventilador da PoE HAT nesses pinos no boot [S4].
  - Os overlays de câmera (exemplo: `imx219`) ligam o `i2c0mux` [S19]. Ele é um mux por pinctrl que alterna o controlador I²C0 entre os GPIO 0/1 e os GPIO 44/45 [S19].
  - Com a câmera ligada, o UART2 dividiria pinos com um mux ativo. Isto é uma inferência a partir do device tree; não testamos.
  - O CTS/RTS do UART2 cairia nos GPIO 2/3, do I²C1 [S12][S10].
- **UART4 descartado.** Ocupa os GPIO 8/9, que são CE0 e MISO do SPI0 [S10].
- **UART3 e UART5 livres no V-GUARD:**
  - Os GPIO 4/5 só concorrem com o 1-Wire (`w1-gpio`, padrão no GPIO 4) e o `i2c3` (padrão nos GPIO 4/5) [S12].
  - Os GPIO 12/13 só concorrem com o PWM de hardware [S10] e o `i2c5` (padrão nos GPIO 12/13) [S12].
  - O áudio analógico da Pi 4 usa o PWM nos GPIO 40/41, fora do conector [S17].
- **PPS.** O `pps-gpio` usa o GPIO 18 por padrão [S12]. O GPIO 18 não é usado por nenhum dos UARTs escolhidos.
- **Câmera.** Na Pi 4, o I²C da câmera fica nos GPIO 44/45, fora do conector [S12]. A câmera não toma pinos do conector.
- **Por que o ESP32 no UART5 e o GPS no UART3.**
  - Os pinos 32 e 33 ficam lado a lado, com GND no 34. Isso facilita um cabo curto e trançado para o enlace mais rápido.
  - O GPS anda devagar (tipicamente até 115200) e já tem mais fios (PPS e alimentação).
  - Os dois UARTs são PL011 iguais [S13], então trocar é só uma questão de fiação.

### 4. Velocidade máxima

- **Teto do PL011.** O PL011 gera baud "up to UARTCLK/16" [S13, §11.1].
  - `init_uart_clock` tem padrão de 48 MHz, e "the maximum baudrate for the UART is limited to 1/16th of the clock" [S5]. Essa opção está na página de opções legadas; não deve ser mexida.
  - No device tree, todos os PL011 da Pi 4 usam o mesmo relógio `BCM2835_CLOCK_UART` [S20]. O mini UART usa o relógio auxiliar [S20].
  - O driver Linux aceita até `uartclk/16` e calcula o divisor como `round(uartclk·4/baud)`, separado em parte inteira e fração de 1/64 [S22].
- **Como o kernel atende o FIFO.** O driver arma a interrupção de recepção com o FIFO pela metade [S22]. Os UARTs da Pi 4 não têm DMA no device tree [S20], então a recepção é por interrupção.
- **Lado do ESP32.** O relógio do UART é o APB de 80 MHz, e o divisor tem parte inteira e fração de 4 bits [S28]. O ESP-IDF calcula `clk_div = (80 MHz << 4) / baud`, truncando [S29].

Erros calculados com essas fórmulas (Pi com `uartclk` = 48 MHz):

| Baud | Pi: divisor | Pi: baud real (erro) | ESP32: divisor | ESP32: baud real (erro) | Diferença entre os dois |
|---|---|---|---|---|---|
| 460 800 | 6 + 33/64 | 460 432 (−0,08 %) | 173 + 9/16 | 460 929 (+0,03 %) | 0,11 % |
| 921 600 | 3 + 16/64 | 923 077 (+0,16 %) | 86 + 12/16 | 922 190 (+0,06 %) | 0,10 % |
| **1 000 000** | **3 + 0/64** | **1 000 000 (0 %)** | **80 + 0/16** | **1 000 000 (0 %)** | **0 %** |
| 2 000 000 | 1 + 32/64 | 2 000 000 (0 %) | 40 + 0/16 | 2 000 000 (0 %) | 0 % |

- **Os erros não são o limite.** São de décimos de por cento. O limite prático é outro: sem controle de fluxo, o kernel precisa esvaziar o FIFO a tempo.
- **Folga do FIFO.** O FIFO de recepção tem 32 posições [S13, §11.1], e a interrupção vem com 16 ocupadas [S22]. Sobram 16 bytes de folga:

  | Baud | Tempo para encher a folga de 16 bytes |
  |---|---|
  | 460 800 | ~347 µs |
  | 1 000 000 | ~160 µs |

  São tempos confortáveis para a Pi 4, mas isto não foi medido. Fica para o ensaio de 2 horas (T24.2).
- **Ocupação da linha.** O tráfego previsto é de ~3 kB/s, cerca de 3 % da linha a 1 Mbaud (10 bits por byte em 8N1).

### 5. Alimentação de 5 V

Ver a seção "Alimentação de 5 V" acima. Fontes: [S9][S10][S14][S15][S30].

### 6. Console serial

- **Passos oficiais.** Para usar o UART primário com outro dispositivo, a documentação manda desligar o console e manter o UART ligado. No `raspi-config`, isso é `3 Interface Options > I6 Serial Port`, respondendo "No" ao login e "Yes" ao hardware [S1][S2].
- **O que o `raspi-config` faz** [S27]:
  - mexe no `cmdline.txt`: apaga `console=serial0,<baud>` e `console=ttyAMA0,<baud>`;
  - mexe no `config.txt`: grava `enable_uart`;
  - aceita os modos `nonint` (`do_serial_cons`, `do_serial_hw`).
- **`enable_uart=1` com `console=serial0,115200`** cria o console nos GPIO 14/15 [S3].
- **`cmdline.txt`:** fica em `/boot/firmware/cmdline.txt` e precisa ficar numa linha só [S8].
- **Login pela serial.** O `systemd-getty-generator` só abre `serial-getty@` nos consoles do kernel definidos com `console=` [S26].

### Lista de fontes

| Id | Fonte | URL |
|---|---|---|
| S1 | Raspberry Pi Docs, "Configure UARTs" | https://www.raspberrypi.com/documentation/computers/configuration.html#uarts · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/configuration/interfaces.adoc#L192-L510 |
| S2 | Raspberry Pi Docs, "Enable or disable serial port" | https://www.raspberrypi.com/documentation/computers/configuration.html#serial-port · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/configuration/interfaces.adoc#L164-L190 |
| S3 | Raspberry Pi Docs, config.txt `enable_uart` | https://www.raspberrypi.com/documentation/computers/config_txt.html#enable_uart · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/config_txt/boot.adoc#L96-L98 |
| S4 | Raspberry Pi Docs, config.txt `disable_poe_fan` e `force_eeprom_read` | https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/config_txt/boot.adoc#L87-L102 |
| S5 | Raspberry Pi Docs, config.txt legado, `init_uart_clock` | https://www.raspberrypi.com/documentation/computers/legacy_config_txt.html · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/legacy_config_txt/boot.adoc#L46-L52 |
| S6 | Raspberry Pi Docs, padrões de clock (`core_freq`) | https://www.raspberrypi.com/documentation/computers/config_txt.html#overclocking · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/config_txt/overclocking.adoc#L104-L137 |
| S7 | Raspberry Pi Docs, filtro `[all]` do config.txt | https://www.raspberrypi.com/documentation/computers/config_txt.html · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/config_txt/conditional.adoc#L8-L10 |
| S8 | Raspberry Pi Docs, "Configure the kernel command line" | https://www.raspberrypi.com/documentation/computers/configuration.html#cmdline · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/configuration/boot-behaviour.adoc#L144-L182 |
| S9 | Raspberry Pi Docs, "Power supply" | https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#power-supply · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/raspberry-pi/power-supplies.adoc#L28-L178 |
| S10 | Raspberry Pi Docs, GPIO (funções alternativas e tensões) e controle dos pads | https://www.raspberrypi.com/documentation/computers/raspberry-pi.html · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/raspberry-pi/gpio-on-raspberry-pi.adoc#L65-L85 e https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/raspberry-pi/gpio-pad-controls.adoc#L42 |
| S11 | Raspberry Pi Docs, `vcgencmd measure_clock` | https://www.raspberrypi.com/documentation/computers/os.html · fonte: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/os/graphics-utilities.adoc#L91-L115 |
| S12 | `README` dos overlays (raspberrypi/firmware) | https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README. Trechos: [uart0](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L5680-L5688), [uart1](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L5711-L5716), [uart2](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L5739-L5742), [uart3](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L5779-L5782), [uart4](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L5819-L5822), [uart5](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L5859-L5862), [disable-bt](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L1150-L1154), [miniuart-bt](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L3796-L3803), [pps-gpio](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L4375-L4385), [krnbt](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L355-L360), [i2c_csi_dsi](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L314-L328), [i2c3/i2c5](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L2903-L2937), [w1-gpio](https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README#L6469-L6473) |
| S13 | BCM2711 ARM Peripherals (§2.2 mini UART; §5.3, tabela 94; §11.1 e tabela 171, PL011) | https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf |
| S14 | Raspberry Pi 4 Model B Datasheet, Release 1.1 (tabela 2, §4.1, tabela 5, §5.3) | https://datasheets.raspberrypi.com/rpi4/raspberry-pi-4-datasheet.pdf |
| S15 | Raspberry Pi 4 Model B, esquema reduzido (USB-C J1, D1, conector J8, nota ID_SD/ID_SC) | https://datasheets.raspberrypi.com/rpi4/raspberry-pi-4-reduced-schematics.pdf |
| S16 | Kernel, `bcm2711-rpi-ds.dtsi` (apelidos, pinos e nós uart2–5) | https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi-ds.dtsi#L38-L48 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi-ds.dtsi#L323-L345 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi-ds.dtsi#L512-L538 |
| S17 | Kernel, `bcm2711-rpi-4-b.dts` (uart0 no BT, uart1 no conector, PWM do áudio nos GPIO 40/41) | https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi-4-b.dts#L192-L196 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi-4-b.dts#L256-L268 |
| S18 | Kernel, `bcm271x-rpi-bt.dtsi` (`max-speed` do Bluetooth) | https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm271x-rpi-bt.dtsi#L3-L41 |
| S19 | Kernel, `i2c0mux` (`bcm2711-rpi.dtsi`), desligado por padrão (`bcm270x-rpi.dtsi`), ligado pelo overlay da câmera (`imx219-overlay.dts`) | https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi.dtsi#L21-L43 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm270x-rpi.dtsi#L141-L145 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/overlays/imx219-overlay.dts#L27-L30 |
| S20 | Kernel, relógios dos UARTs (`bcm2711.dtsi`, `bcm283x.dtsi`) | https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711.dtsi#L130-L172 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm283x.dtsi#L304-L312 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm283x.dtsi#L385-L391 |
| S21 | Kernel, overlays `disable-bt`, `miniuart-bt`, `uart3`, `uart5` | https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/overlays/disable-bt-overlay.dts · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/overlays/miniuart-bt-overlay.dts · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/overlays/uart3-overlay.dts · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/overlays/uart5-overlay.dts |
| S22 | Kernel, driver `amba-pl011.c` (FIFO e gatilho, cálculo do baud, número da porta pelo apelido) | https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/drivers/tty/serial/amba-pl011.c#L121-L167 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/drivers/tty/serial/amba-pl011.c#L2064-L2085 · https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/drivers/tty/serial/amba-pl011.c#L2678-L2756 |
| S23 | Commit "ARM: dts: bcm2711: Add extra serial aliases" (24/02/2023) | https://github.com/raspberrypi/linux/commit/b2af2fb9f272ed77d36aae773cab07a9170ab0e6 |
| S24 | Regra udev de `serial0`/`serial1` (raspberrypi-sys-mods) | https://github.com/RPi-Distro/raspberrypi-sys-mods/blob/5c66bcd45eadb55af2419805700e2fdb30d52584/etc.armhf/udev/rules.d/99-com.rules#L14-L45 |
| S25 | systemd, `50-udev-default.rules` (grupo `dialout`) | https://github.com/systemd/systemd/blob/195a17c59b22f5b0fa626fdb4917107cdc281bac/rules.d/50-udev-default.rules.in#L47 |
| S26 | systemd, `systemd-getty-generator(8)` | https://www.freedesktop.org/software/systemd/man/latest/systemd-getty-generator.html · fonte: https://github.com/systemd/systemd/blob/195a17c59b22f5b0fa626fdb4917107cdc281bac/man/systemd-getty-generator.xml |
| S27 | `raspi-config` (ramo `trixie`) | https://github.com/RPi-Distro/raspi-config/blob/293f4bb0bfe0efba41b0a8260b52b161239f2a33/raspi-config#L11-L16 · https://github.com/RPi-Distro/raspi-config/blob/293f4bb0bfe0efba41b0a8260b52b161239f2a33/raspi-config#L1287-L1396 · https://github.com/RPi-Distro/raspi-config/blob/293f4bb0bfe0efba41b0a8260b52b161239f2a33/raspi-config#L3890-L3895 |
| S28 | ESP32 Technical Reference Manual, v5.8 (§19.3.2 e `UART_CLKDIV_REG`) | https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf |
| S29 | ESP-IDF v5.4, `uart_ll_set_baudrate` do ESP32 | https://github.com/espressif/esp-idf/blob/v5.4/components/hal/esp32/include/hal/uart_ll.h#L208-L217 |
| S30 | ESP32-DevKitC User Guide, "Power Supply Options" | https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html |
| S31 | `pinctrl` (raspberrypi/utils) | https://github.com/raspberrypi/utils/blob/master/pinctrl/README.md |

## Riscos e incertezas

1. **Relógio de UART de 48 MHz.** Está documentado como padrão do `init_uart_clock`, numa página de opções legadas [S5]. É coerente com o Bluetooth a 3 Mbaud [S18], mas não foi medido na nossa Pi.
   - Conferir com `vcgencmd measure_clock uart`.
   - Se der outro valor, refazer a tabela de erros, porque o teto é sempre `uartclk/16`.
2. **Nomes `ttyAMA3`/`ttyAMA5`.** Dependem dos apelidos do kernel 6.1 em diante [S23]. Num Raspberry Pi OS antigo (Bullseye, kernel 5.15), os UARTs extras ganhavam o primeiro número livre.
   - Conferir com `ls /dev/ttyAMA*` depois de instalar o sistema.
   - O Raspberry Pi OS atual usa `/boot/firmware`; os mais antigos usavam `/boot` [S27].
3. **Conflito do UART2 com a câmera.** Foi deduzido do device tree [S19]; não foi testado. Não muda a recomendação, porque o esquema já proíbe usar os GPIO 0/1 [S15].
4. **Recepção sem controle de fluxo a 1 Mbaud.** Depende da latência de interrupção do Linux (folga de ~160 µs). Não foi medida.
   - O ensaio de 2 horas (T24.2) responde isso.
   - Se aparecer perda, as saídas são duas: voltar para 460800, ou usar CTS/RTS. O CTS/RTS do UART5 cai nos GPIO 14/15 [S12] e exigiria 2 fios a mais.
5. **Fusível no 5 V.** O esquema da Pi 4 é "reduzido" [S15]. Ele não mostra fusível no 5 V, mas o esquema completo não é público.
   - A recomendação de pôr um PTC no fio do ESP32 vale de qualquer forma.
6. **Placa ESP32.** O DevKit "clássico" pode ser clone, com outro caminho de 5 V. A regra de uma só fonte vale como precaução [S30].
7. **Módulo GPS desconhecido.** Se o GPS for uma HAT com pinos fixos, conferir o pinout dela antes. Muitas HATs usam os GPIO 14/15.
   - Alternativa nesse caso: GPS no UART0 com `dtoverlay=disable-bt` (`/dev/ttyAMA0`, pinos 8/10), sem Bluetooth.
   - Aí é **obrigatório** desligar o console [S1][S21]. O ESP32 continua no UART5.
   - O PPS da HAT tem de cair num GPIO livre. Se ela usar o GPIO 4, não dá para usar o UART3.
8. **Boot da Pi.** Antes de o overlay valer, o GPIO 12 (TX da Pi para o ESP32) é entrada com pull-down [S14, tabela 5]. O ESP32 pode ver um "break" ou lixo no RX durante o boot da Pi.
   - O protocolo (COBS + CRC) já descarta isso. O firmware só precisa tolerar erros de UART na partida.

## Recomendação para o ticket

**Decisão proposta para a D13:**

- **ESP32 no UART5:**
  - pinos 32 (TX da Pi) e 33 (RX da Pi), GND no 34 e 5 V no pino 2 ou 4;
  - `dtoverlay=uart5`, dispositivo `/dev/ttyAMA5`, 8N1, sem controle de fluxo.
- **GPS no UART3:** pinos 7 e 29, `dtoverlay=uart3`, `/dev/ttyAMA3`.
- **PPS:** pino 12 (GPIO 18), `dtoverlay=pps-gpio,gpiopin=18`.
- **Bluetooth:** sem mudança.
- **Console serial:** desligado com `raspi-config nonint do_serial_cons 1` e `do_serial_hw 1`.

**Velocidade:**

- Manter **460800** como padrão (é o que o plano já prevê).
- Depois que o ensaio T24.2 passar, testar **1 000 000 baud**: divisor exato nos dois lados, e o dobro da folga.
- Preferir 1 000 000 a 921600.
- Acima disso não há ganho para ~3 kB/s.

**5 V pela Pi: sim, com duas regras:**

1. Um PTC de ~0,5 A no fio de 5 V do ESP32.
2. Nunca ligar o USB do ESP32 no notebook com o fio de 5 V da Pi conectado. Desconectar antes de regravar.

A fonte do carro precisa entregar 5,1 V e pelo menos 3 A.

**Recados:**

- **Thiago (T19, fiação):** o pinout da seção "Fiação"; pinos 32/33/34 para o enlace.
- **Nicolas (T16, GPS e chrony):** GPS em `/dev/ttyAMA3` e PPS no GPIO 18, pelo `pps-gpio`.
  - Se o módulo GPS for uma HAT de pinos fixos, avisar antes de soldar (ver "Riscos", item 7).
- **Isabela (leitor da Pi):** a porta é `/dev/ttyAMA5`; o usuário do leitor precisa estar no grupo `dialout`.
- **Todos:** depois do primeiro boot com a configuração, rodar os comandos de "Conferência depois do boot" e anotar a saída no ticket.

**Para fechar o ticket:** comentar com o link deste arquivo e acrescentar uma linha em "Decisions so far" no mapa #1.

[S1]: https://www.raspberrypi.com/documentation/computers/configuration.html#uarts
[S2]: https://www.raspberrypi.com/documentation/computers/configuration.html#serial-port
[S3]: https://www.raspberrypi.com/documentation/computers/config_txt.html#enable_uart
[S4]: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/config_txt/boot.adoc#L87-L102
[S5]: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/legacy_config_txt/boot.adoc#L46-L52
[S6]: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/config_txt/overclocking.adoc#L104-L137
[S7]: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/config_txt/conditional.adoc#L8-L10
[S8]: https://www.raspberrypi.com/documentation/computers/configuration.html#cmdline
[S9]: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/raspberry-pi/power-supplies.adoc#L28-L178
[S10]: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/raspberry-pi/gpio-on-raspberry-pi.adoc#L65-L85
[S11]: https://github.com/raspberrypi/documentation/blob/ddea98e98dafe9419feb4fd82710c2fbdef7dc44/documentation/asciidoc/computers/os/graphics-utilities.adoc#L91-L115
[S12]: https://github.com/raspberrypi/firmware/blob/1eea140da8c84a4f11baf6ba4dff513fa5d9874c/boot/overlays/README
[S13]: https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf
[S14]: https://datasheets.raspberrypi.com/rpi4/raspberry-pi-4-datasheet.pdf
[S15]: https://datasheets.raspberrypi.com/rpi4/raspberry-pi-4-reduced-schematics.pdf
[S16]: https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi-ds.dtsi#L38-L48
[S17]: https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi-4-b.dts#L256-L268
[S18]: https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm271x-rpi-bt.dtsi#L3-L41
[S19]: https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711-rpi.dtsi#L21-L43
[S20]: https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/broadcom/bcm2711.dtsi#L130-L172
[S21]: https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/arch/arm/boot/dts/overlays/disable-bt-overlay.dts
[S22]: https://github.com/raspberrypi/linux/blob/43c132e8863c3bff3647033b6a7d2bf87b15501c/drivers/tty/serial/amba-pl011.c#L2678-L2756
[S23]: https://github.com/raspberrypi/linux/commit/b2af2fb9f272ed77d36aae773cab07a9170ab0e6
[S24]: https://github.com/RPi-Distro/raspberrypi-sys-mods/blob/5c66bcd45eadb55af2419805700e2fdb30d52584/etc.armhf/udev/rules.d/99-com.rules#L14-L45
[S25]: https://github.com/systemd/systemd/blob/195a17c59b22f5b0fa626fdb4917107cdc281bac/rules.d/50-udev-default.rules.in#L47
[S26]: https://www.freedesktop.org/software/systemd/man/latest/systemd-getty-generator.html
[S27]: https://github.com/RPi-Distro/raspi-config/blob/293f4bb0bfe0efba41b0a8260b52b161239f2a33/raspi-config#L1287-L1396
[S28]: https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf
[S29]: https://github.com/espressif/esp-idf/blob/v5.4/components/hal/esp32/include/hal/uart_ll.h#L208-L217
[S30]: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html
[S31]: https://github.com/raspberrypi/utils/blob/master/pinctrl/README.md
