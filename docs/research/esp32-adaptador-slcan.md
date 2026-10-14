# ESP32 clássico como adaptador USB-CAN (slcan)

Pesquisa do ticket #3 (parte do mapa #1, decisão D20; bloqueia o #11). Feita em 13 e 14/10/2026.

As fontes primárias foram:

- o manual do protocolo ASCII da Lawicel (CANUSB e CAN232);
- o código e a documentação do python-can 4.6.1, a versão mais recente no PyPI;
- o driver `slcan` do kernel Linux e o `slcand` do can-utils;
- o datasheet do ESP32, a documentação do TWAI no ESP-IDF e a errata do chip;
- os datasheets das pontes USB-UART CP2102 e CH340;
- o código dos firmwares candidatos, lido nos repositórios e fixado num commit.

Cada afirmação traz a fonte entre colchetes, como [S1]. A lista completa está no fim da seção "Achados com fontes". As contas de vazão são nossas e mostram os números de partida.

## Pergunta

Existe firmware pronto, de preferência mantido e de código aberto, que transforme um **ESP32 clássico** (DevKit WROOM-32, USB por ponte CP2102 ou CH340) num **adaptador USB-CAN slcan** compatível com a interface `slcan` do python-can?

- Quais os limites: baud da UART pela ponte USB, quadros por segundo, buffers, velocidades CAN e modo *listen-only*?
- Se não houver, quanto custa escrever uma ponte mínima sobre o TWAI do ESP-IDF, e quais comandos ela precisa?
- Alternativas: a interface `serial` do python-can com protocolo próprio, e o `gs_usb` (impossível sem USB nativo?).
- No Linux: o `slcan` do python-can funciona bem em `/dev/ttyUSB*` com baud alto? E o `slcand`, que transforma o adaptador numa interface SocketCAN, permite que a ECU simulada use `socketcan` nas duas fases?

O contexto: barramento de 500 kbit/s, IDs de 11 bits, tráfego baixo (até 20 requisições por segundo, mais as respostas).

## Resposta curta

- **Firmware pronto existe, mas nenhum é maduro.**
  - O único com licença aberta e binário pronto para o ESP32 clássico, com TWAI e a porta serial na UART0 (que é a do USB), é o **Doggie**. Ele é MIT, está na v0.2.1, de 27/10/2025, e foi apresentado no Black Hat Arsenal 2025 [S25].
  - A documentação do Doggie mostra o uso com o `slcan` do python-can e com o `slcand` [S29c][S29d].
  - A manutenção é fraca. O último commit é de 27/10/2025, e dois PRs de correção seguem abertos. Num deles, um usuário relata que o `espflash` atual recusa o binário [S30].
  - Os outros candidatos têm um destes problemas:
    - não têm licença (mintynet, raibisch, thakurpaudel);
    - foram feitos para outras placas (ESP32RET);
    - dependem do núcleo Arduino 2.x, de Wi-Fi e de servidor web (NewTec).
- **Os limites estão na UART, não no CAN.**
  - A 115200 baud cabem cerca de 520 quadros de 8 bytes por segundo em cada sentido. Nosso tráfego usa menos de 4 % disso.
  - O Doggie fixa a UART em 115200 e aceita só 125, 250, 500 e 1000 kbit/s. Ele tem *listen-only* (comando `L`), ignora filtros e sempre responde `F00` [S26][S27][S28].
  - O TWAI do ESP32 clássico tem modo *listen-only* e uma FIFO de recepção de 64 bytes. Ele vai de 25 kbit/s a 1 Mbit/s, ou de 12,5 kbit/s nas revisões 3.x do chip [S15].
- **Escrever uma ponte própria é trabalho pequeno.**
  - São cerca de 300 a 500 linhas de C no ESP-IDF. Dois exemplos públicos têm umas 430 linhas cada [S33][S36].
  - O mínimo que o python-can e o kernel usam são estes comandos: `S0`–`S8`, `O`, `L`, `C`, `t` e `T`, e a linha vazia. Vale incluir também `r`, `R`, `F`, `V` e `N`.
  - Estimativa: 1 a 2 dias, com teste.
- **O `gs_usb` é impossível no ESP32 clássico (confirmado).**
  - O `gs_usb` exige um dispositivo USB que fale esse protocolo, reconhecido pelo VID/PID [S7][S13].
  - O ESP32 clássico não tem periférico USB: o datasheet não lista nenhum [S15], e o componente TinyUSB da Espressif não o inclui entre os alvos [S20].
  - O conector USB do DevKit vai para a ponte USB-UART [S22]. O notebook só enxerga uma porta serial.
- **A interface `serial` do python-can não compensa.** Ela usa um formato binário próprio do python-can, sem comando de bitrate, sem abrir e fechar canal e sem equivalente no SocketCAN [S5].
- **No Linux, as duas vias funcionam.**
  - O `slcan` do python-can abre `/dev/ttyUSB0@<baud>` com qualquer baud que a ponte aceite [S4][S14a].
  - O `slcand` cria uma interface SocketCAN (`can0`) a partir do adaptador [S12][S6].
  - Com o kernel 6.0 ou mais novo, `ip link set can0 type can bitrate 500000` faz o próprio kernel mandar `C`, `S6` e `O` ao subir a interface [S9][S11].
  - Assim a ECU simulada usa `socketcan` nas duas fases e só troca `vcan0` por `can0`.
- **Recomendação:**
  1. Usar SocketCAN nas duas fases.
  2. Gastar no máximo 2 h testando o Doggie na bancada.
  3. Se ele não passar no teste de aceitação, escrever a ponte mínima descrita na seção 6.

## Opções encontradas

| Projeto | Licença | Atividade | Compatibilidade (ESP32 clássico, python-can, `slcand`) | Limites |
|---|---|---|---|---|
| **Doggie** ([infobyte/doggie][S25]) | MIT [S25] | v0.2.1 e último commit em 27/10/2025; 82★; PRs #5 e #12 abertos [S30][S30b] | Binário pronto `doggie_esp32_twai_ble` para o ESP32 com TWAI [S25]. Porta na UART0, GPIO 1/3, que passa pela ponte USB [S27]. Documentação com python-can e `slcand` [S29c][S29d] | UART fixa em 115200 [S27]; CAN 125/250/500/1000 kbit/s, e qualquer outra velocidade vira 500 [S26]; `L` sim [S26]; filtros ignorados [S26]; `F` sempre `F00` [S28]; não responde `\r` a `O`, `C`, `S` e `t` [S28]. TWAI no RX = GPIO 25 e TX = GPIO 26 no código, mas a documentação diz D4/D3 [S26][S31][S29]. Escrito em Rust (esp-hal 0.23) |
| **esp32-slcan** ([mintynet][S33]) | **Nenhuma** [S33][S42] | Commits em jan. e fev. de 2026; 110★; nenhuma issue | O esboço TWAI (núcleo Arduino 3.x) foi testado só no ESP32-S3 [S33]. Os esboços para o ESP32 clássico usam uma biblioteca antiga e o núcleo 2.x [S33] | 500000 baud; S2–S8 no clássico (S0/S1 só no S3); sem `L`; `t` responde `\r` mesmo com o canal fechado; RTR padrão enviado como estendido (*bug*); filtros ignorados [S33] |
| **ESP32RET** ([collin80][S34]) | MIT [S34] | Commits em jan. de 2026; 414★ | Feito para as placas EVTV e Macchina A0, que ele escolhe pela revisão do chip [S34]. O Lawicel divide a porta com o GVRET e o console. O próprio autor o chama de "somewhat tested. Still experimental" [S34] | 1 Mbaud; cria um ponto de acesso Wi-Fi por padrão; `V` e `N` terminam em `\n`, fora do protocolo [S34] |
| **esp32-can-iot** ([NewTec][S35]) | BSD-3-Clause [S35] | Commit em mar. de 2026; 50★ | ESP32 (`esp32dev`) com o conjunto CAN232 completo: `L`, `Q` (*auto-start*), `U`, `X` e outros [S35] | Núcleo Arduino 2.x fixado; Wi-Fi, servidor web e portal cativo; driver CAN por registradores, não o TWAI do ESP-IDF; feito para a placa própria; 115200 por padrão [S35] |
| **ESP32-SLCAN** ([thakurpaudel][S36]) | **Nenhuma** | Criado em dez. de 2025; 1★ | ESP-IDF puro, UART0, `L` sim; README com python-can e `slcand` [S36] | 115200; TWAI nos GPIO 21/22; driver TWAI legado; 432 linhas, um bom tamanho de referência [S36] |
| **ESP32-SLCAN** ([raibisch][S37]) | **Nenhuma** | Último commit em jan. de 2025; 13★ | Arduino/PlatformIO, derivado do mintynet [S37] | 115200 ou 460800 (comando `b`); sem `L`; driver CAN por registradores [S37] |
| Projetos para ESP32-C3, S3 ou C6 (WiCAN, c3can e outros) | GPL-3.0 e outras | Ativos | **Não servem**: dependem de USB nativo, que só existe nos chips mais novos [S41][S20] | Exigiriam outra placa |

Nenhum dos projetos acima tem relato público de uso na nossa configuração: ESP32 clássico, TWAI, ponte USB-UART e python-can a 500 kbit/s.

## Achados com fontes

### 1. O protocolo slcan (Lawicel)

- Todo comando termina em CR (ASCII 13) e diferencia maiúsculas de minúsculas [S1, §2].
- A resposta é `\r` para OK e BEL (ASCII 7) para erro. Os comandos de envio são a exceção: `t` e `r` respondem `z\r`, e `T` e `R` respondem `Z\r` [S1, §1.4 e §2.5–2.8].

| Comando | Sintaxe | Função | Fonte |
|---|---|---|---|
| `S` | `Sn\r`, com n de 0 a 8 | 10, 20, 50, 100, 125, 250, 500 e 800 kbit/s, e 1 Mbit/s. Só com o canal fechado | [S1, §2.1] |
| `s` | `sxxyy\r` | Bit timing direto pelos registradores BTR0 e BTR1 do SJA1000 | [S1, §2.2] |
| `O` / `C` | `O\r` / `C\r` | Abre e fecha o canal | [S1, §2.3–2.4] |
| `L` | `L\r` | Abre o canal em *listen-only*: recebe, mas não transmite | [S2] (CAN232; não aparece no manual do CANUSB) |
| `t` / `T` | `tiiildd…\r` / `Tiiiiiiiildd…\r` | Envia um quadro de 11 ou de 29 bits | [S1, §2.5–2.6] |
| `r` / `R` | `riiil\r` / `Riiiiiiiil\r` | Envia um quadro RTR | [S1, §2.7–2.8] |
| `F` | `F\r` → `Fxx\r` | Lê os *flags*: FIFO cheia, *error warning*, *overrun*, *error passive*, perda de arbitragem e erro de barramento | [S1, §2.9] |
| `M` / `m` | `Mxxxxxxxx\r` | Código e máscara de aceitação do SJA1000 | [S1, §2.10–2.11] |
| `V` / `N` | `V\r` → `V1013\r` | Versão (2 dígitos de hardware e 2 de software) e número de série | [S1, §2.12–2.13] |
| `Z` | `Z0\r` / `Z1\r` | Liga ou desliga o *timestamp*: 4 dígitos hex em ms, que dão a volta em 0xEA5F (60 000 ms) | [S1, §2.14] |
| `Q` | `Qn\r` | *Auto-start*: o adaptador já liga com o canal aberto (CAN232) | [S2] |

Outros pontos do protocolo:

- **Quadros recebidos.** O CANUSB manda cada quadro recebido assim que ele chega, sem *polling*. O CAN232 antigo precisava dos comandos `P`, `A` ou `X` para isso [S1, §1.4][S2].
- **Abertura de sessão.** O manual recomenda mandar 2 ou 3 CR para limpar o buffer, conferir a versão com `V`, configurar com `S`, abrir com `O` e consultar `F` de tempos em tempos [S1, §1.5].
- **Limites do adaptador original.** O CANUSB tem FIFO de transmissão de 8 quadros e de recepção de 32. Foi testado com 1000 quadros/s a 125 kbit/s, e o próprio manual o descreve como voltado a "low speed CAN networks" [S1, §1.4].

### 2. O que o python-can e o Linux mandam e esperam

**python-can 4.6.1**, a versão mais recente no PyPI, de 12/08/2025 [S8]:

- **Padrões.** `tty_baudrate=115200`, `sleep_after_open=2` s, `timeout=0.001` s e `listen_only=False` [S4, l. 62 e 74–84].
- **Sequência de abertura.** Ele abre a porta, espera 2 s e manda `C`, `S6`, uma linha vazia, `O` e, de novo, `O` (ou `L` se `listen_only=True`) [S4, l. 136–168, 186–200 e 244–251].
  - A linha vazia vem do `data_bitrate` de CAN FD, que sem valor vira só `\r` [S4, l. 56–60 e 196].
- **Nenhuma resposta é esperada.** O `_read` só corta o fluxo em `\r` ou BEL [S4, l. 214–240].
- **Recepção.** Ele reconhece `t`, `T`, `r` e `R`, e as variantes FD. O horário do quadro vem do relógio do PC (`time.time()`), e o *timestamp* do adaptador é ignorado [S4, l. 253–336].
- **Sem tratamento de erro na decodificação.** O `int(…, 16)` não está protegido. Uma linha corrompida que comece com `t`, `T`, `r` ou `R` sobe como `ValueError` no `recv()` [S4, l. 270–320]. Isso vem da leitura do código; não testamos.
- **Tabela de velocidades.** Ela tem duas diferenças em relação ao Lawicel: 750 000 vira `S7` (o Lawicel define `S7` como 800 kbit/s), e 83 300 vira `S9`, que não existe no Lawicel [S4, l. 44–55][S1, §2.1]. Nada disso afeta os 500 kbit/s (`S6`).
- **`listen_only`.** Entrou pelo PR #1496 em junho de 2024 e está disponível desde a 4.4.2 [S8].
- **Leitura byte a byte.** Na 4.6.1 a leitura é feita um byte por vez. Uma rotina mais rápida (PR #2028) entrou no ramo principal em fevereiro de 2026, mas ainda não saiu em versão [S4, l. 214–240][S8].

**Linux:**

- **O driver.** O `slcan` é uma disciplina de linha (`N_SLCAN`) para "interfaces CAN de baixo custo ligadas por linha serial ou por adaptador USB-serial que usam o protocolo ASCII da LAWICEL" [S10].
- **O `slcand`** (can-utils):
  - liga a disciplina ao tty e cria a interface de rede;
  - com `-s n`, escreve `C\rSn\r`; com `-f`, `F\r`; com `-l`, `L\r`; com `-o`, `O\r`. Escreve tudo de uma vez, logo depois de abrir a porta, sem esperar resposta [S12, l. 76–90 e 326–366];
  - liga o `ASYNC_LOW_LATENCY` [S12, l. 296–302] e aceita `-S` de 9600 a 4 000 000 baud [S12, l. 118–171].
- **Kernel 6.0 ou mais novo.** A partir da série de junho e julho de 2022, o próprio driver manda os comandos [S11]:
  - ao subir a interface, com o bitrate configurado por `ip link`: `C\rS<n>\r`, `F\r` (opcional), e `L\r` ou `O\r`;
  - ao descer: `C\r` [S9, l. 672–760].
  - O driver aceita só as velocidades de `S0` a `S8` [S9, l. 115–118] e o modo `listen-only` [S9, l. 846].
- **Recepção no kernel.**
  - O kernel decodifica `t`, `T`, `r` e `R`, além das extensões `e` (erros) e `s` (estado). Ignora o resto, inclusive `z` e `Z` [S9, l. 181–260 e 451–466].
  - CR ou BEL encerram uma linha [S9, l. 472–490].
  - Uma linha que não decodifica só incrementa `rx_errors` [S9, l. 258–259]. O kernel é mais tolerante que o python-can.
- **No notebook.** O kernel 7.0.0-38 já traz o módulo `slcan.ko` (`modinfo slcan`). O can-utils ainda não está instalado.
- **Documentação do python-can.** Ela mostra exatamente esta via: `slcand` e depois `socketcan` [S6].

**O contrato mínimo**, portanto, é este:

- o adaptador deve aceitar `C`, `Sn`, `O` ou `L`, `t`/`T` e linhas vazias ou desconhecidas, sem travar;
- deve devolver os quadros recebidos como `t…\r` ou `T…\r`, assim que chegam;
- nenhum dos dois clientes depende das respostas.

### 3. Os candidatos

- **Doggie** [S25]–[S31]:
  - **Placa e porta.** Para o ESP32 clássico, a porta serial é a UART0 nos GPIO 1/3, a 115200, e os logs vão para a UART2 (GPIO 17) [S27].
  - **Pinos do TWAI.** Saem de `TwaiConfiguration::new(TWAI0, GPIO25, GPIO26, …)` [S26, l. 49–58]. No esp-hal 0.23.1 a ordem dos parâmetros é `(rx, tx)` [S31, l. 970–979], então **RX = GPIO 25 e TX = GPIO 26**. A tabela da documentação diz D4/D3 [S29], o que contradiz o código.
  - **Comandos.**
    - Interpreta `O`, `C`, `F`, `L`, `S`, `t`, `T`, `r`, `R`, `m`, `M`, `Z`, `V` e `N`; o `s` devolve "não implementado" [S28b, l. 256–272].
    - Responde só a `F` (`F00`), `V` (`V1337`), `N` (`N1337`) e `Z` [S28, l. 109–120].
    - O `S` aceita 125, 250, 500 ou 1000 kbit/s; qualquer outro valor vira 500 [S26, l. 166–176]. Os filtros estão marcados como TODO [S26, l. 178–184].
    - Um quadro enviado com o canal fechado é descartado [S26, l. 107].
  - **Um TODO dos autores.** O laço principal escolhe entre a serial e o CAN com `select` e traz o comentário "TODO: Check if no packets are dropped" [S28, l. 85–90].
  - **Gravação.** A documentação manda gravar o *release* com `espflash flash --monitor -L defmt <arquivo>` [S29b, l. 43–51]. O `espflash` tem binário pronto para Linux, sem precisar do Rust [S32b].
  - **O problema do PR #5.** Um usuário relata que o `espflash` recusa o *release* por falta do *app descriptor* [S30]. O `espflash` 4.1 ou mais novo tem a opção `--ignore-app-descriptor` [S32]. Não verificamos se a imagem dá boot assim.
  - **Errata.** No módulo TWAI do esp-hal 0.23.1, o único *workaround* de errata é o do *listen-only* [S31, l. 917–925].
- **mintynet** [S33]:
  - O esboço TWAI diz no próprio código que foi feito para o ESP32-S3 e que não funciona no C3 [S33, l. 3–10]. Ele usa o driver TWAI legado do ESP-IDF, que traz os *workarounds* de errata [S16].
  - **Defeitos no código:**
    - responde `\r` a `t` e `T` sem conferir se o canal está aberto [S33, l. 81–88];
    - envia RTR padrão com `extd = 1` [S33, l. 380–383];
    - o `twai_transmit` tem *timeout* de 1 *tick* e o retorno é ignorado [S33, l. 416].
  - O README afirma que o CP2102 é "limitado a 500 kbps". O datasheet da Silicon Labs diz até 1 Mbit/s [S23].
- **ESP32RET** [S34]:
  - Lawicel ligado por padrão (`enableLawicel`, true) [S34b, l. 94].
  - Escolhe a placa pela revisão do chip [S34b, l. 97] e abre a serial a 1 Mbaud [S34b, l. 289].
  - As respostas `V1013\n` e `ESP32RET\n` saem fora do protocolo [S34c, l. 45–49].
- **NewTec** [S35]:
  - Tem a implementação Lawicel mais completa, com *auto-poll* ligado por padrão e *auto-start* [S35b].
  - Por outro lado, fixa o núcleo Arduino 2.x e puxa Wi-Fi, servidor web e portal cativo [S35c].
- **thakurpaudel** [S36]:
  - Usa ESP-IDF com o driver legado, UART0 a 115200 [S36, l. 17 e 414] e TWAI nos GPIO 21/22 [S36, l. 13–14].
  - Implementa `L` [S36, l. 171] e espera até 100 ms no `twai_transmit` [S36, l. 274].
- **Licenças.** Repositório sem licença, pelo GitHub, significa: "no one may reproduce, distribute, or create derivative works from your work" [S42]. Podemos ler o mintynet, o raibisch e o thakurpaudel como referência, mas não copiar o código para o nosso repositório.

### 4. Vazão, buffers e latência

Uma linha slcan de um quadro de 11 bits com 8 bytes tem 22 caracteres: `t` + 3 de ID + 1 de DLC + 16 de dados + CR. Com *timestamp* são 26. No formato 8N1, cada caractere ocupa 10 bits. As contas abaixo são nossas.

| Baud da UART | Caracteres/s | Quadros/s (22 caracteres) | Quem aceita |
|---|---|---|---|
| 115 200 | 11 520 | ≈ 520 | Todos. É o padrão do python-can [S4] e o valor fixo do Doggie [S27] |
| 460 800 | 46 080 | ≈ 2 090 | CP2102 [S23], CH340 [S24] |
| 921 600 | 92 160 | ≈ 4 190 | CP2102 (está na tabela) [S23], CH340 [S24] |
| 1 000 000 | 100 000 | ≈ 4 545 | CP2102 (é o máximo) [S23], CH340 [S24] |
| 2 000 000 | 200 000 | ≈ 9 090 | Só o CH340 [S24] |

- **Lado CAN.** Um quadro de 11 bits com 8 bytes tem 108 bits, mais 3 de intervalo, e até 24 bits de *stuffing* no trecho que vai do SOF ao CRC [S38]. Isso dá de 111 a 135 bits. A 500 kbit/s, são de 3 700 a 4 500 quadros/s com o barramento 100 % ocupado. A partir de 921 600 baud a UART acompanha o barramento cheio; a 115 200, só cerca de 12 % dele.
- **Nosso tráfego.** São até 20 requisições do nosso ESP32 para o PC e até 20 respostas do PC para o barramento: 440 caracteres/s em cada sentido. Isso é **3,8 % de 115 200** e cerca de 1 % do barramento.
- **Pontes e UART do ESP32:**
  - CP2102: de 300 bps a 1 Mbit/s, com 576 bytes de buffer de recepção e 640 de transmissão [S23];
  - CH340: de 50 bps a 2 Mbit/s [S24];
  - o guia do DevKitC fala em ponte "up to 3 Mbps" sem dizer qual chip [S22];
  - a UART do ESP32 vai até 5 MBaud [S15];
  - no Linux, o pyserial aceita baud fora do padrão com `TCSETS2` e `BOTHER` [S14a, l. 161–175].
- **Buffers no ESP32:**
  - FIFO de recepção do TWAI em hardware: 64 bytes [S15];
  - fila de software do driver legado: 5 quadros de recepção e 5 de transmissão por padrão, configurável [S18a, l. 31–45];
  - errata TWAI-3.13.11: com 64 ou mais mensagens em *overrun*, a FIFO fica irrecuperável [S19]. O ESP-IDF traz *workarounds* de software ligados por padrão e recomenda mantê-los [S16];
  - no Doggie, o buffer de entrada da serial tem 256 bytes [S28, l. 76].
- **Latência.**
  - Transmitir uma linha de 22 caracteres leva 1,9 ms a 115 200 e 0,24 ms a 921 600 (conta nossa).
  - O USB *full-speed* trabalha em quadros de 1 ms [S1, §1.4].
  - Não achamos fonte primária sobre o tempo que o CP2102 ou o CH340 seguram os dados antes de enviá-los. **É preciso medir.**

### 5. TWAI: velocidades, *listen-only* e versões do ESP-IDF

- **No hardware (datasheet):** CAN 2.0 com IDs de 11 e 29 bits. Vai de 25 kbit/s a 1 Mbit/s nas revisões v0.0/v1.0/v1.1 e de 12,5 kbit/s a 1 Mbit/s nas revisões v3.0/v3.1. Tem os modos Normal, Listen Only e Self-Test, e FIFO de recepção de 64 bytes [S15].
- **Driver legado (ESP-IDF 5.4):**
  - `TWAI_TIMING_CONFIG_*` de 25 kbit/s a 1 Mbit/s; 20, 16 e 12,5 kbit/s só nas revisões novas [S16];
  - modos `TWAI_MODE_NORMAL`, `TWAI_MODE_NO_ACK` e `TWAI_MODE_LISTEN_ONLY`. No *listen-only* o controlador não transmite, não dá ACK e não manda quadro de erro [S16];
  - no modo normal, a transmissão precisa do ACK de outro nó [S16].
- **Driver novo:** `esp_twai` (`twai_new_node_onchip`), com o *listen-only* ligado por `enable_listen_only` [S17]. No ESP-IDF 6.0, o `driver/twai.h` legado emite o aviso "The legacy TWAI driver is deprecated" [S18b, l. 16–18].
- **No PlatformIO:** a plataforma `espressif32` 7.1.3 usa o `framework-espidf ~4.60100.0`, isto é, o ESP-IDF 6.1 [S39]. Uma ponte nova deve usar a API `esp_twai`.
- **Errata:** a errata do ESP32 lista 11 itens de TWAI, que afetam todas as revisões [S19].

### 6. Quanto custa escrever a nossa ponte

- **Tamanho:**
  - o esboço TWAI do mintynet tem 437 linhas (Arduino) [S33];
  - o `main.c` do thakurpaudel tem 432 linhas (ESP-IDF, com `L`) [S36];
  - com o driver novo, que entrega a recepção por *callback*, deve dar algo entre 300 e 500 linhas.
- **Comandos.** O que o python-can [S4], o `slcand` [S12] e o kernel [S9] mandam, e o que o Lawicel define [S1][S2]:

| Comando | Resposta da nossa ponte | Por quê |
|---|---|---|
| `S0`–`S8` | `\r`; BEL se o canal estiver aberto ou o valor for inválido | python-can, `slcand -s`, `ip link … bitrate` |
| `O` | `\r`; abre em modo normal | todos |
| `L` | `\r`; abre em *listen-only* | `listen_only=True`, `slcand -l`, `ip link … listen-only on` |
| `C` | `\r`, mesmo se já estiver fechado | o python-can manda `C` antes do `S` |
| `t`, `T` | `z\r` ou `Z\r`; BEL se a fila estiver cheia ou o canal fechado | envio |
| `r`, `R` | idem | opcional (o OBD não usa RTR) |
| `F` | `Fxx\r` com os bits da seção 1, lidos do estado do TWAI | `slcand -f` e o reset de erro do kernel |
| `V`, `N` | `V1001\r`, `NVG01\r` | `get_version()` exige dígitos decimais em `V` [S4, l. 384–410] |
| `Z0`, `Z1` | `\r` | opcional (o python-can e o kernel ignoram o *timestamp*) |
| linha vazia | `\r` | o python-can manda uma [S4, l. 196]; o Lawicel recomenda 2–3 CR [S1, §1.5] |
| outros (`s`, `M`, `m`…) | BEL | robustez |

- **Fora do protocolo:**
  - **UART0** a 115 200 por padrão, ajustável por constante. Console desligado (`CONFIG_ESP_CONSOLE_NONE`) [S40], para que nenhum log saia na porta do slcan.
  - **Mensagens da ROM.** A ROM ainda imprime na UART0 durante o boot. Ligar o GPIO 15 (MTDO) ao GND desliga essa impressão [S15].
  - **TWAI** com fila de recepção de 32 ou mais quadros. Recuperação automática de *bus-off* (`twai_node_recover`) [S17].
  - **Quadros recebidos** vão para a UART assim que chegam, como no CANUSB [S1, §1.4].
  - **Opcional:** abrir sozinho a 500 kbit/s no boot, como o `Q` do CAN232 [S2]. É uma extensão nossa; protege contra o reset na abertura da porta (ver "Riscos", item 4).
- **Teste.** O analisador e o formatador de linhas são lógica pura e entram nos testes do notebook (ADR-0005).
  - Na fase 1 dá até para rodar a ponte nativa entre um pseudo-terminal e o `vcan0`. O python-can (`slcan`) ou o `slcand` falam com o pseudo-terminal.
  - Assim a ponte chega testada à placa.
- **Estimativa:** 1 a 2 dias para quem já vai escrever o `TwaiBus` do firmware, porque o código de TWAI é o mesmo assunto.

### 7. Alternativas

- **Interface `serial` do python-can** [S5]:
  - Cada quadro é `0xAA`, *timestamp* (4 bytes, *little-endian*), DLC, ID (4 bytes com *flags*), dados e `0xBB` [S5, l. 112–160]. Não há *escape*: um `0xAA` ou um `0xBB` nos dados confunde a sincronia.
  - Não há comando de bitrate, de abrir nem de fechar o canal. O baud padrão é 115200 [S5, l. 54–60].
  - Não existe equivalente no SocketCAN: a ECU simulada ficaria com um código para cada fase.
  - **Conclusão: não usar.**
- **`gs_usb`:**
  - O python-can procura o dispositivo pelo pyusb, pelo VID/PID 1d50:606f, e o fala por usbfs ou WinUSB [S7].
  - O driver `gs_usb` do Linux também casa uma tabela de VID/PID [S13, l. 31–47].
  - O ESP32 clássico não tem USB [S15][S20]: o conector do DevKit é da ponte USB-UART [S22], que aparece como porta serial.
  - **Impossível sem trocar de placa** (ESP32-S2/S3, ou um candleLight ou CANable).
- **Outra placa com USB nativo** (projetos para C3/S3, WiCAN [S41]): fora de alcance, porque só temos DevKits clássicos.

### 8. Linux: `slcan` direto ou `slcand` com SocketCAN

- **`slcan` direto.**
  - Funciona com `can.Bus(interface="slcan", channel="/dev/ttyUSB0@115200", bitrate=500000)` [S3][S4].
  - Baud alto não é problema no Linux [S14a] e as duas pontes aceitam 921 600 [S23][S24]. A essa vazão, o custo da leitura byte a byte da 4.6.1 é irrelevante.
- **DTR e RTS na abertura da porta:**
  - o pyserial liga DTR e RTS ao abrir [S14b, l. 210–211][S14a, l. 347–351];
  - no DevKit, o RTS vai ao EN (reset) e o DTR ao GPIO 0 [S21];
  - a Espressif avisa que "Some OS and/or drivers may activate RTS and or DTR automatically when opening the serial port", e que o circuito de dois transistores não reseta se os dois sinais forem ativados juntos [S21];
  - **portanto, abrir a porta pode ou não resetar o adaptador.** O `sleep_after_open=2` do python-can cobre o boot. O `slcand` não espera: escreve os comandos logo depois de abrir [S12, l. 283–366].
- **Controle de fluxo por hardware.** Não usar (`rtscts=True` ou `slcand -t hw`): o RTS do DevKit é o fio de reset [S21].
- **Via recomendada: SocketCAN com o `slcand`** [S6][S12][S9]:

```bash
sudo apt install can-utils

# 1. Liga a disciplina slcan ao tty e cria a can0, sem mandar comando nenhum.
sudo slcand -S 115200 /dev/ttyUSB0 can0

# 2. Dá tempo ao adaptador de terminar o boot, caso a abertura da porta o tenha resetado.
sleep 2

# 3. Com o kernel 6.0 ou mais novo, o driver manda C, S6 e O ao subir a interface.
sudo ip link set can0 type can bitrate 500000
#    Para só escutar: sudo ip link set can0 type can bitrate 500000 listen-only on
sudo ip link set can0 up

candump -td can0
```

- **O jeito clássico também funciona:** `sudo slcand -o -c -s6 -S 115200 /dev/ttyUSB0 can0 && sudo ip link set can0 up` [S6][S12]. Mas ele manda os comandos sem esperar o boot.
- **Na ECU simulada, só muda o nome do canal:**

```python
import os
import can

canal = os.environ.get("VGUARD_CAN", "vcan0")  # fase 1: vcan0; fase 2: can0
bus = can.Bus(interface="socketcan", channel=canal)
```

### Lista de fontes

- [S1] Manual do LAWICEL CANUSB (protocolo ASCII), Rev. 1.1 (2026), baseado no V1.0D de 2011.
- [S2] Manual do LAWICEL CAN232, versão 3: comandos `L`, `Q`, `X` e `U`.
- [S3] Documentação do python-can: interface slcan.
- [S4] python-can v4.6.1, `can/interfaces/slcan.py`.
- [S5] python-can v4.6.1, `can/interfaces/serial/serial_can.py`.
- [S6] Documentação do python-can: interface socketcan (vcan e slcand).
- [S7] Documentação do python-can: interface gs_usb.
- [S8] python-can: versões no PyPI, PR #1496 (`L`) e PR #2028 (leitura mais rápida, ainda sem versão).
- [S9] Linux, `drivers/net/can/slcan/slcan-core.c`.
- [S10] Linux, `drivers/net/can/Kconfig` (`CAN_SLCAN`).
- [S11] Commits do kernel, todos no v6.0: bitrate pela API de dispositivo CAN (dca796299462), comandos de abrir e fechar (5bac315be7eb) e *listen-only* (3e720131960b).
- [S12] can-utils, `slcand.c`.
- [S13] Linux, `drivers/net/can/usb/gs_usb.c`.
- [S14a] pyserial, `serial/serialposix.py`. [S14b] pyserial, `serial/serialutil.py`.
- [S15] Datasheet do ESP32, v5.3.
- [S16] ESP-IDF v5.4, TWAI (driver legado).
- [S17] ESP-IDF estável (v6.1), TWAI (driver `esp_twai`).
- [S18a] ESP-IDF v5.4, `driver/twai.h` (valores padrão). [S18b] ESP-IDF v6.0, `driver/twai.h` (aviso de descontinuação).
- [S19] Errata do ESP32: resumo.
- [S20] esp-usb, `esp_tinyusb/idf_component.yml` (alvos suportados).
- [S21] esptool: seleção do modo de boot (DTR, RTS e o circuito de reset automático).
- [S22] Guia do usuário do ESP32-DevKitC.
- [S23] Datasheet do CP2102, Rev. 1.0 (Silicon Labs; cópia na SparkFun, porque o site da Silicon Labs bloqueia download automático).
- [S24] Datasheet do CH340 (WCH; cópia na SparkFun).
- [S25] Doggie: repositório, licença MIT e *release* v0.2.1.
- [S26] Doggie, `doggie_esp32/doggie/src/twai_can.rs`.
- [S27] Doggie, `doggie_esp32/esp_serial/src/lib.rs`.
- [S28] Doggie, `doggie_core/src/lib.rs`. [S28b] Doggie, `slcan/src/lib.rs`.
- [S29] Doggie, documentação do ESP32 (`doggie.md`). [S29b] Doggie, `intro.md` (gravação). [S29c] Doggie, "Get Started" (`slcand`). [S29d] Doggie, "SLCAN Protocol" (python-can).
- [S30] Doggie, PR #5 (*app descriptor* exigido pelo `espflash`). [S30b] Doggie, PR #12 (correções para `slcand` e `ip`). Os dois estão abertos.
- [S31] esp-hal v0.23.1, `esp-hal/src/twai/mod.rs`.
- [S32] espflash: CHANGELOG (`--ignore-app-descriptor` a partir da 4.1.0). [S32b] espflash: *releases* com binários para Linux.
- [S33] mintynet/esp32-slcan: README e `esp32-twai-can.ino`.
- [S34] ESP32RET: README. [S34b] `ESP32RET.cpp`. [S34c] `lawicel.cpp`.
- [S35] NewTec esp32-can-iot: README e licença. [S35b] `Lawicel.h`. [S35c] `platformio.ini`.
- [S36] thakurpaudel/ESP32-SLCAN, `main/main.c`.
- [S37] raibisch/ESP32-SLCAN: README e `main.cpp`.
- [S38] Bosch, CAN Specification 2.0 (formato do quadro e *bit stuffing*).
- [S39] platform-espressif32 v7.1.3, `platform.json`.
- [S40] ESP-IDF v6.1, `components/esp_stdio/Kconfig` (`ESP_CONSOLE_NONE`).
- [S41] WiCAN (meatpiHQ/wican-fw): README (ESP32-C3).
- [S42] GitHub Docs: licenciamento de repositórios (repositório sem licença).
- [S43] Pesquisa do ticket #2, `docs/research/can-bancada-sem-transceptor.md` (ramo `research/can-bancada-sem-transceptor`).

## Riscos e incertezas

1. **Ninguém testou isto ainda, nem nós, nem relatos públicos.** Não achamos relato do Doggie (ou de outro candidato) no ESP32 clássico com TWAI e ponte USB-UART. A confiança no Doggie vem só da leitura do código e da existência do *release*.
2. **Os pinos do Doggie divergem da bancada.** O código usa TX = GPIO 26 e RX = GPIO 25 [S26][S31], a documentação diz D4/D3 [S29], e o esquema de diodos do ticket #2 usa TX = 25 e RX = 26 nas duas placas [S43]. Com o Doggie, o adaptador precisa da fiação invertida, ou de uma recompilação.
3. **Pode ser que o Doggie não grave.** Ver o PR #5 [S30]. O `--ignore-app-descriptor` [S32] talvez resolva; senão, resta testar um `espflash` 3.x. Isso entra no limite de 2 h.
4. **Abrir a porta pode resetar o adaptador** [S21]. Depende do sistema e do driver.
   - Mitigações: o roteiro `slcand` → `sleep` → `ip link` da seção 8; o `sleep_after_open` do python-can; ou o *auto-start* na nossa ponte.
   - Nunca ligar RTS/CTS.
5. **Mensagens de boot na UART0.**
   - A ROM imprime no boot [S15], e o *bootloader* também pode imprimir.
   - No kernel, uma linha inválida só conta `rx_errors` [S9]. No python-can (`slcan` direto), uma linha corrompida que comece com `t`, `T`, `r` ou `R` pode levantar `ValueError` [S4].
   - Mitigações: usar SocketCAN; ligar o GPIO 15 ao GND na placa adaptadora [S15]; e, se usar o `slcan` direto, tratar a exceção na ECU simulada.
6. **Latência não medida.** Não achamos fonte sobre o tempo que o CP2102 ou o CH340 seguram os dados. Medir a ida e volta na bancada:
   - a requisição do nosso ESP32 e a resposta da ECU simulada, com `candump -td can0` e com os horários do próprio firmware;
   - se incomodar, subir para 921 600 baud.
7. **Adaptador fechado ou reiniciando.**
   - Numa bancada de dois nós, ninguém dá ACK aos quadros do nosso ESP32 enquanto isso dura [S16][S38]. O TWAI dele acumula erros e retransmite.
   - O firmware precisa aguentar isso de qualquer jeito, porque o carro também pode se calar. Ainda assim, convém abrir o adaptador antes de ligar o nosso ESP32.
8. **Errata do TWAI** [S19]:
   - o driver do ESP-IDF tem *workarounds* ligados por padrão [S16];
   - o esp-hal usado pelo Doggie só tem o do *listen-only* [S31];
   - com o nosso tráfego baixo, as condições das erratas (erros de barramento, *overrun*) são improváveis. Uma bancada de diodos mal montada pode provocá-las.
9. **Licenças.** O mintynet, o raibisch e o thakurpaudel não têm licença [S42]. Servem só de leitura.
10. **Manutenção.** O Doggie tem PRs parados desde novembro de 2025 [S30], e a rotina rápida do python-can ainda não saiu em versão [S8]. Nada disso pesa no nosso volume de tráfego.
11. **Qual ponte tem cada DevKit?** Não sabemos se é CP2102 ou CH340. Conferir com `lsusb`. As duas aceitam 115 200 e 921 600 [S23][S24].

## Recomendação para o ticket

**Decisão D20:**

1. **A ECU simulada usa `socketcan` nas duas fases.**
   - Fase 1: `vcan0`.
   - Fase 2: `can0`, criado pelo `slcand` a partir do adaptador (roteiro da seção 8).
   - O código é o mesmo; só a variável `VGUARD_CAN` muda.
   - O `slcan` direto do python-can fica de reserva, para um teste rápido sem `sudo`.
2. **Firmware do adaptador: testar o Doggie por no máximo 2 h e, se falhar, escrever a ponte mínima.**
   - **Doggie v0.2.1** (`doggie_esp32_twai_ble`, MIT):
     - gravar com o `espflash` de binário pronto (talvez com `--ignore-app-descriptor`);
     - ligar o TWAI em TX = GPIO 26 e RX = GPIO 25, e conferir;
     - UART a 115 200.
   - **Critérios de aceitação:**
     1. o `candump can0` mostra as requisições OBD do nosso ESP32;
     2. um `cansend can0 7E8#…` chega ao nosso ESP32;
     3. a ECU simulada responde por 10 min a 20 requisições/s sem perder nenhuma, com a ida e volta medida e anotada;
     4. tirar e recolocar o USB do adaptador e repetir o roteiro de subida funciona.
   - **Se falhar em qualquer item: ponte própria**, pela especificação da seção 6:
     - ESP-IDF com a API `esp_twai`, UART0 a 115 200, console desligado e *auto-start* opcional;
     - projeto separado no repositório, por exemplo `tools/adaptador-slcan/`, fora do binário do firmware do V-GUARD, como a ECU simulada;
     - analisador testado no notebook;
     - 1 a 2 dias.
3. **Velocidade da UART:** 115 200 basta, porque ocupa 3,8 % com o nosso tráfego. Subir para 921 600 só se a latência medida incomodar; o CP2102 e o CH340 aceitam.

**Recados:**

- **Thiago (#11, fiação):**
  - Os pinos do adaptador dependem da escolha acima. O Doggie usa TX = 26 e RX = 25, o contrário do desenho do #2 [S43]. A ponte própria segue o desenho do #2.
  - Na placa adaptadora, ligar o GPIO 15 ao GND (silencia a ROM). Evitar os pinos de *strapping* (0, 2, 5, 12 e 15) para o TWAI [S15].
- **Quem escreve a ECU simulada:**
  - usar `interface="socketcan"` e o canal pela variável `VGUARD_CAN`;
  - não depender das respostas `\r`/`z` do adaptador, porque o SocketCAN não as repassa;
  - se usar `slcan` direto, tratar `ValueError` no `recv()`.
- **Todos:** `sudo apt install can-utils`. O kernel 7.0 do notebook já tem o módulo `slcan`.

**Para fechar o ticket:** comentar com o link deste arquivo e acrescentar uma linha em "Decisions so far" no mapa #1.

[S1]: http://www.can232.com/docs/canusb_manual.pdf
[S2]: http://www.can232.com/docs/can232_v3.pdf
[S3]: https://python-can.readthedocs.io/en/stable/interfaces/slcan.html
[S4]: https://github.com/hardbyte/python-can/blob/v4.6.1/can/interfaces/slcan.py
[S5]: https://github.com/hardbyte/python-can/blob/v4.6.1/can/interfaces/serial/serial_can.py
[S6]: https://python-can.readthedocs.io/en/stable/interfaces/socketcan.html
[S7]: https://python-can.readthedocs.io/en/stable/interfaces/gs_usb.html
[S8]: https://pypi.org/project/python-can/#history
[S9]: https://github.com/torvalds/linux/blob/4056b3c9d3c92ea45955641dbafeb9b960e146ef/drivers/net/can/slcan/slcan-core.c
[S10]: https://github.com/torvalds/linux/blob/4056b3c9d3c92ea45955641dbafeb9b960e146ef/drivers/net/can/Kconfig#L189-L206
[S11]: https://github.com/torvalds/linux/commit/dca796299462
[S12]: https://github.com/linux-can/can-utils/blob/cbbad5eedeacd1898700d9be40270250d5b5147a/slcand.c
[S13]: https://github.com/torvalds/linux/blob/4056b3c9d3c92ea45955641dbafeb9b960e146ef/drivers/net/can/usb/gs_usb.c#L31-L47
[S14a]: https://github.com/pyserial/pyserial/blob/a5c48d445fbc1943d4fabf8d9090a50fda3172fd/serial/serialposix.py
[S14b]: https://github.com/pyserial/pyserial/blob/a5c48d445fbc1943d4fabf8d9090a50fda3172fd/serial/serialutil.py#L210-L211
[S15]: https://documentation.espressif.com/esp32_datasheet_en.pdf
[S16]: https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/peripherals/twai.html
[S17]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html
[S18a]: https://github.com/espressif/esp-idf/blob/v5.4/components/driver/twai/include/driver/twai.h#L31-L45
[S18b]: https://github.com/espressif/esp-idf/blob/v6.0/components/driver/twai/include/driver/twai.h#L16-L18
[S19]: https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32/02-errata-summary/index.html
[S20]: https://github.com/espressif/esp-usb/blob/c40b86dc3a3e760b3dfdc28ec83010b9799a7e88/device/esp_tinyusb/idf_component.yml#L9-L14
[S21]: https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html
[S22]: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html
[S23]: https://www.sparkfun.com/datasheets/IC/cp2102.pdf
[S24]: https://cdn.sparkfun.com/datasheets/Dev/Arduino/Other/CH340DS1.PDF
[S25]: https://github.com/infobyte/doggie/releases/tag/v0.2.1
[S26]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/doggie_esp32/doggie/src/twai_can.rs
[S27]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/doggie_esp32/esp_serial/src/lib.rs#L56-L100
[S28]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/doggie_core/src/lib.rs#L76-L120
[S28b]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/slcan/src/lib.rs#L256-L272
[S29]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/docs/hardware/diy/esp32/doggie.md
[S29b]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/docs/hardware/diy/esp32/intro.md#L43-L51
[S29c]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/docs/software/doggie/get_started.md
[S29d]: https://github.com/infobyte/doggie/blob/9d3e127baa3f77b5c6866560b91c4b21744a5496/docs/software/doggie/slcan.md
[S30]: https://github.com/infobyte/doggie/pull/5
[S30b]: https://github.com/infobyte/doggie/pull/12
[S31]: https://github.com/esp-rs/esp-hal/blob/v0.23.1/esp-hal/src/twai/mod.rs
[S32]: https://github.com/esp-rs/espflash/blob/f4634b90db46e1845c5499be93373a580c136592/CHANGELOG.md
[S32b]: https://github.com/esp-rs/espflash/releases/tag/v4.6.0
[S33]: https://github.com/mintynet/esp32-slcan/blob/a2268c82aace1076d35ad511e4951009adfda81e/esp32-twai-can/esp32-twai-can.ino
[S34]: https://github.com/collin80/ESP32RET/blob/ae857ea909abf6ab47b94d40f0a6f204bf8a8bbb/README.md
[S34b]: https://github.com/collin80/ESP32RET/blob/ae857ea909abf6ab47b94d40f0a6f204bf8a8bbb/src/ESP32RET.cpp
[S34c]: https://github.com/collin80/ESP32RET/blob/ae857ea909abf6ab47b94d40f0a6f204bf8a8bbb/src/lawicel.cpp#L14-L60
[S35]: https://github.com/NewTec-GmbH/esp32-can-iot/blob/da2b9f48080ed77a0811b8ad49ff4b6ec9951251/README.md
[S35b]: https://github.com/NewTec-GmbH/esp32-can-iot/blob/da2b9f48080ed77a0811b8ad49ff4b6ec9951251/Coding/fw/lib/Lawicel/src/Lawicel.h#L107-L168
[S35c]: https://github.com/NewTec-GmbH/esp32-can-iot/blob/da2b9f48080ed77a0811b8ad49ff4b6ec9951251/Coding/fw/platformio.ini
[S36]: https://github.com/thakurpaudel/ESP32-SLCAN/blob/bf92c7692a788cd6663174192d1a3826b69891ab/main/main.c
[S37]: https://github.com/raibisch/ESP32-SLCAN/blob/fb7fe3441e165fbac6143186e16f5c93cd422c07/src/main.cpp
[S38]: http://esd.cs.ucr.edu/webres/can20.pdf
[S39]: https://github.com/platformio/platform-espressif32/blob/v7.1.3/platform.json#L87-L92
[S40]: https://github.com/espressif/esp-idf/blob/v6.1/components/esp_stdio/Kconfig#L31-L32
[S41]: https://github.com/meatpiHQ/wican-fw
[S42]: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository
[S43]: https://github.com/V-guard-org/firmware_telemetria/blob/research/can-bancada-sem-transceptor/docs/research/can-bancada-sem-transceptor.md
