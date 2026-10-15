# Testes e critérios de aceite

Como provar cada fase e o que precisa passar para começar a seguinte. Especificação fechada no [ticket #13](https://github.com/V-guard-org/firmware_telemetria/issues/13) em 15/10/2026.

As decisões por trás deste documento estão nas ADRs:
- [ADR-0003](adr/0003-simulacao-ou-real-nunca-os-dois.md): simulação e fontes reais nunca convivem no projeto.
- [ADR-0005](adr/0005-codigo-testado-no-notebook-antes-das-placas.md): o código é testado no notebook antes de ir para as placas, em quatro fases.
- [ADR-0008](adr/0008-nucleo-passivo-chamado-pela-plataforma.md): o núcleo passivo, que os testes chamam com portas falsas e um relógio falso.

O que se prova está no [`PROTOCOLO.md`](PROTOCOLO.md), no [`MAQUINAS-DE-ESTADO.md`](MAQUINAS-DE-ESTADO.md), no [`PLATAFORMA.md`](PLATAFORMA.md), no [`DECODIFICADOR.md`](DECODIFICADOR.md) e no [`SIMULADOR.md`](SIMULADOR.md). Os termos seguem o [`CONTEXT.md`](../CONTEXT.md).

## Resumo

- **Passagem de fase.** Só os cinco itens de segurança bloqueiam, e só antes do ensaio no carro. O resto vira pendência anotada, com prazo, e a fase seguinte começa (§2).
- **Testes automáticos no notebook:** Unity para o núcleo e `unittest` para o Python. Um único comando, `./verifica.sh`, roda os dois antes de cada push. Sem CI por enquanto (§3).
- **Roteiros que se conferem sozinhos.** O comando `confere` transforma os roteiros do simulador em testes de ponta a ponta, iguais no notebook e no ESP32 (§4).
- **Lista fechada (D2):** o teste do portão em três partes, mais uma conferência fora do firmware em todo ensaio: a ECU simulada conta os quadros do ESP32 fora da lista, e a conta deve dar 0 (§5).
- **Ensaio de 2 h (D16)** nas fases 1, 2 e 4, sempre seguido de 30 min nas taxas máximas. Na fase 4 ele vale oficialmente (T24.2) (§6).
- **Horário (D12, T41):** provado pela conta e medido com um fio entre o ESP32 e a Pi na fase 4 (§7).
- **Build de teste:** um firmware à parte, que trava de propósito e pulsa um pino. Nunca é o firmware entregue (§8).
- **Bancada com três placas:** o firmware, o adaptador USB-CAN e um gerador de tráfego, que imita as centrais do carro conversando (§10).
- **Osciloscópio,** numa única ida ao laboratório, para a subida da linha e o GPIO27 (§10.4).
- **Sessão com transceptor:** antes do carro, dois TJA1051T/3 na bancada provam o modo silencioso e o jumper do GPIO27 (§10.7).
- **Resultados** em `docs/ensaios/`, um arquivo por ensaio (§15).
- **Ensaio no carro:** fica para quando o carro for definido (§13).

## 1. O que se prova onde

| Camada | Onde roda | O que prova | Quando |
|---|---|---|---|
| Testes do núcleo (Unity) | Notebook, `pio test -e native` | Protocolo, máquinas de estado, portão, IMU, com portas falsas e relógio falso | A cada mudança |
| Testes do Python (`unittest`) | Notebook e Pi | Decodificador, fórmulas da ECU simulada, leitura dos roteiros | A cada mudança |
| Roteiros com `confere` | Notebook (fase 1) e ESP32 pelo USB (fase 2) | O sistema inteiro, de ponta a ponta | A cada passagem de fase |
| Ensaio de 2 h | Fases 1, 2 e 4 | Perdas, reinícios, memória, conteúdo das amostras | A cada passagem de fase |
| Bancada | Três ESP32 e o notebook | TWAI, falhas do CAN, carga, watchdog, GPIO27 | Fase 2 |
| Pi | Raspberry Pi | Decodificador no Python da Pi e a UART5 | Fase 3 |
| Integração | ESP32, Pi e notebook | A ligação pelos fios e o horário (T40, T41) | Fase 4 |
| Sessão com transceptor | Placa do firmware e adaptador com TJA1051T/3 | O modo silencioso de verdade e o jumper do GPIO27 | Antes do carro |
| Carro | O carro do ensaio | Transceptor, centrais de verdade, tempos do grupo 3 (§14) | 25/11 |

## 2. Passagem de fase e itens de segurança

Cada fase termina com uma lista do que precisa passar (§9, §10.6, §11 e §12).
- **Item de segurança que falha:** a fase seguinte pode começar, mas o ensaio no carro não acontece até ele passar.
- **Qualquer outro item que falha:** vira pendência no arquivo do ensaio (§15), com um prazo, e a fase seguinte começa.

Exemplo: na bancada, tudo passa, menos a contagem do PCNT no GPIO26. Não é item de segurança, então a fase 4 começa, e a pendência ganha um prazo antes do carro.

A bancada não tem carro, então transmitir nela não oferece risco. Por isso a regra mais dura só vale antes do carro.

**Os cinco itens de segurança:**

| Item | Onde se prova |
|---|---|
| **Lista fechada:** só saem requisições do modo 01 para os endereços padronizados | Teste do portão e conferência externa em todo ensaio (§5) |
| **Transceptor silencioso no boot e depois de uma pane:** o GPIO27 nunca vai a 0 antes de o firmware liberá-lo | Osciloscópio no boot, depois de `esp_restart()` e depois de uma pane provocada pelo build de teste (§10.4); com o transceptor de verdade, na sessão com transceptor (§10.7) |
| **Silêncio sem a Pi:** 3 s sem PING levam a ESPERANDO_PI com o transceptor silencioso | Teste do cenário "a Pi trava", roteiro `falhas.txt` e osciloscópio (§10.4) |
| **Bloqueio na 3ª falha** | Teste do cenário a 250 kbit/s mudo e o mesmo caso na bancada (§10.3) |
| **Escuta antes de falar:** nenhum quadro antes de 1 s de escuta | Testes de todos os cenários e osciloscópio (§10.4) |

## 3. Testes automáticos no notebook

### 3.1 Núcleo (Unity)

Os testes montam o núcleo com as quatro portas falsas e um relógio falso que salta de prazo em prazo ([`PLATAFORMA.md` §4.3](PLATAFORMA.md)). As máquinas têm um teste por cenário.

*Emenda da spec da fase 1 (15/10/2026).* Os testes de `lib/obd/`, `lib/imu/` e `lib/core/` entram todos pelo núcleo: eles olham só o que sai nas portas falsas (quadros, bytes e chamadas de `talk()`/`listen()`) e o que o STATUS mostra, sem chamar as classes internas. Assim, reorganizar o código dentro de um módulo não quebra nenhum teste. Só a moldura do protocolo e o portão são testados direto, porque o núcleo não os alcança: ele nunca manda um quadro fora da lista, e os vetores do [`PROTOCOLO.md` §8](PROTOCOLO.md) pedem `boot`, `seq` e `t_us` fixos. A tabela abaixo diz o que se cobre, e não por onde.

| Módulo | O que os testes cobrem |
|---|---|
| `lib/protocol/` | CRC (`"123456789"` dá `0x29B1`); COBS; os exemplos byte a byte do [`PROTOCOLO.md` §8](PROTOCOLO.md); a ordem e os descartes da fila de saída (§6.5); `seq` que não é gasto por mensagem descartada |
| `lib/obd/` | Cada linha da tabela de cenários do [`MAQUINAS-DE-ESTADO.md` §4](MAQUINAS-DE-ESTADO.md); a agenda; `arb_lost` que não conta como falta; resposta atrasada que ainda vale; resposta em vários quadros ignorada; a escolha da central de menor endereço; a noite de 10 h do §2.5, com cerca de 3 600 perguntas; o portão (§5) |
| `lib/obd/`, emenda do §2.4 | Com a ignição desligada, nenhum PID sai da agenda antes do SEM_RESPOSTA; um PID mudo, com os outros respondendo, sai depois de 5 faltas |
| `lib/imu/` | Os exemplos de média e pico do [`PROTOCOLO.md` §3.2](PROTOCOLO.md); o `t_us` no meio do intervalo; leituras com falha; 10 falhas seguidas levando a FALHA; o reinício a cada 1 s. Em `test/test_imu/`, pelo núcleo, com a IMU falsa (#21) |
| `lib/imu_sim/` | Os valores da IMU_SIM nas amostras; a marca de falha; mais de 1 s sem mensagem; a espera de 1 s na partida; o corpo de tamanho errado em `rejeitadas`. Em `test/test_imu_sim/`, pelo núcleo, com a IMU simulada montada como no `main` e a IMU_SIM chegando pela UART falsa (#21) |
| `lib/core/` | STATUS a cada 1 s; presença da Pi; TAXAS com cada valor aceito e recusado (`rejeitadas`); a `0x83` entregue a quem se registrou e recusada sem registro; `on_dev_bytes()` que só entrega mensagens de desenvolvimento; o limite de 10 LOGs/s; o próximo prazo |

O esboço de um teste de cenário está no [`PLATAFORMA.md` §4.3](PLATAFORMA.md).

### 3.2 Python (`unittest`)

Os testes Python usam o `unittest` da biblioteca padrão, para rodar na Pi sem instalar nada. O pytest, se alguém o tiver, roda os mesmos testes.

| Arquivo | O que cobre |
|---|---|
| `host/test_esp32_decoder.py` | Os vetores do [`PROTOCOLO.md` §8](PROTOCOLO.md); as amostras vazias; o boot novo; a versão desconhecida; lixo na linha, que nunca levanta exceção (bytes aleatórios com semente fixa); a conta do horário (§7.1) |
| `host/test_ecu_sim.py` | A ida e volta de cada PID: a fórmula inversa da ECU simulada e a fórmula do decodificador devolvem o valor de partida, dentro da resolução do PID. Também o PID ausente e o mudo, as duas centrais e o `fora_da_lista`. Roda sem o `vcan0`, porque a ECU simulada não abre o socket ([`SIMULADOR.md` §2](SIMULADOR.md)) |
| `host/test_fake_esp32.py` | 60 s de ESP32 falso no relógio falso, ligados ao `Decoder` sem pty: chegam amostras de IMU e de OBD, sem `lost` nem `corrupt` ([`DECODIFICADOR.md` §4](DECODIFICADOR.md)) |
| `host/test_roteiros.py` | Os roteiros prontos são lidos sem erro (eles chegam no #28); uma linha desconhecida, um tempo que volta, uma linha depois do `fim` ou um valor fora da faixa dá erro com o número da linha |
| `host/test_simulator.py` | O simulador sem tela, com o firmware nativo de verdade como processo filho: um roteiro curto com `reinicia esp32` e `confere boots 2` termina com código 0, e o mesmo roteiro com um `confere` errado termina com 1 (#20). Um roteiro com `buraco`, `imu falha liga`, `confere imu falha`, `confere vazias imu 1`, `imu falha desliga` e `confere imu ok` termina com 0, e a IMU parada chega com `az` ≈ 9,81 (#21). É a parte da costura 7 que não precisa do `vcan0`. As corridas vão lado a lado, em cerca de 10 s, então entram no `./verifica.sh`. Sem o firmware compilado, o teste é pulado |
| `host/test_native_firmware.py` | O firmware nativo de verdade, no pty, com o `Decoder`: o STATUS chega com a plataforma notebook e o OBD em ESPERANDO_PI, os PONGs dão a sincronização, e o atalho some quando o firmware termina. Sem o firmware compilado, o teste é pulado, como na Pi |

### 3.3 Um comando

```
./verifica.sh
```

O script roda `pio test -e native`, compila o firmware nativo com `pio run -e native` e roda `python3 -m unittest discover -s host`. Ele precisa passar antes de cada push. Não há CI no GitHub por enquanto (#13 Q9); se a equipe crescer, o mesmo script vira o passo da CI.

## 4. Roteiros que se conferem sozinhos

*Emenda ao [`SIMULADOR.md` §6](SIMULADOR.md), do #13.*

O roteiro ganha o comando `confere`. Ele compara o que o simulador sabe naquele instante com o valor esperado:

| Comando | O que confere |
|---|---|
| `confere obd <estado>` | O `estado_obd` do último STATUS: `esperando-pi`, `escutando`, `descobrindo`, `lendo`, `sem-resposta`, `velocidade-errada`, `linha-presa`, `falha-can` ou `bloqueado` |
| `confere imu ok`, `confere imu falha` | O `estado_imu` do último STATUS |
| `confere pid <nome> presente`, `confere pid <nome> ausente` | Se o PID está na última amostra OBD |
| `confere vazias obd <n>`, `confere vazias imu <n>` | Quantas amostras vazias daquela fonte chegaram desde o início |
| `confere central <nome> <id>` | Para qual ID foi a última requisição daquele PID, vista pela ECU simulada (útil com `--duas-centrais`) |
| `confere boots <n>` | Quantos boots o decodificador viu |

- O STATUS sai 1 vez por segundo, então um `confere` de estado precisa de folga: escreva-o pelo menos 2 s depois da mudança esperada.
- Um `confere` que falha vai para o registro, com o número da linha, o valor esperado e o visto. O roteiro continua.
- **Sem tela,** o programa termina com código de saída 1 se algum `confere` falhou, ou se algum critério do §6 falhou. Assim, cada passagem de fase é uma sequência de comandos que dá certo ou errado.

Exemplo, no `liga-e-desliga.txt`:

```
0:15 confere obd lendo
0:20 confere vazias obd 0      # a partida, às 0:07, não interrompeu o OBD
0:32 confere obd sem-resposta
0:32 confere vazias obd 1      # a ignição desligou às 0:27
0:50 confere obd lendo
```

Os três roteiros prontos ganham os seus `confere`, e um 4º, `volta-no-maximo.txt`, repete a `volta-no-bairro.txt` com `0:00 taxas imu 100 obd 10`, para os 30 min nas taxas máximas (§6).

## 5. Lista fechada (D2)

**A regra.** Um quadro passa pelo portão se, e só se:
- o ID tem 11 bits e é `0x7DF` ou um de `0x7E0` a `0x7E7`;
- é um quadro de dados (não remoto), com 8 bytes;
- o byte 0 é `0x02`, um quadro único com 2 bytes úteis, e o byte 1 é `0x01`, o modo 01;
- o transceptor está liberado, entre um `talk()` e o `listen()` seguinte.

O byte 2, o PID, é livre, porque todo PID do modo 01 só lê. Os bytes 3 a 7 são preenchimento, e o portão não os confere: o byte 0 já diz que só 2 bytes valem.

**O teste do portão,** em três partes, contra a mesma regra escrita à parte, dentro do teste:
1. **Quadros perigosos, um por um:** apagar os códigos de falha (modo 04), escrever na central (`0x2E`), acesso de segurança (`0x27`), rotina (`0x31`), sessão de diagnóstico (`0x10`), controle de fluxo (`0x30`), modo 09, IDs de 29 bits e quadros remotos.
2. **Todas as combinações:** os 2 048 IDs de 11 bits com os 65 536 valores dos bytes 0 e 1. São cerca de 134 milhões de casos, que rodam em cerca de 1 s.
3. **Fora da liberação:** um quadro válido enviado antes de `talk()` ou depois de `listen()` é barrado.

Cada quadro barrado precisa somar 1 em `bloqueadas`, sair num LOG de erro e levar o OBD a BLOQUEADO.

**Conferência externa.** A ECU simulada recebe todos os quadros que o ESP32 põe no barramento: no `vcan0` na fase 1 e no `can0` nas fases 2 e 4. Ela confere cada um com a mesma regra e conta os que estão fora da lista em `fora_da_lista`, que entra no resumo do simulador. **O valor deve ser 0 em todo ensaio.** É uma prova que não depende do contador do próprio firmware.

## 6. Ensaio de 2 h (D16)

| Fase | Ligação | Papel |
|---|---|---|
| 1 · notebook | pty | Ensaio geral: pega defeitos de lógica sem gastar nada |
| 2 · bancada | USB a 460800 | Condição para passar à fase 4: o ESP32 de verdade, com memória, watchdog e UART real |
| 4 · integração | Fios até a UART5 da Pi | **Oficial (T24.2):** a ligação que vai para o carro |

Cada ensaio tem duas partes:
- **2 h nas taxas padrão** (IMU 25/s, OBD 5/s), com a `volta-no-bairro.txt` repetida. É como o produto roda, com a média de 4 leituras.
- **30 min nas taxas máximas** (IMU 100/s, OBD 10/s), com a `volta-no-maximo.txt`. É o pior caso para a fila e para o cabo, ocupando cerca de 15% da linha.

**Critérios,** todos no resumo final de cada parte:

| Critério | Valor | Fases |
|---|---|---|
| Perdidas: `lost` + o aumento de `descartadas`, sobre o total ([`PROTOCOLO.md` §6.6](PROTOCOLO.md)) | Menos de 0,1% | 1, 2 e 4 |
| Boots vistos | 1, ou seja, nenhum reinício | 1, 2 e 4 |
| `bloqueadas` e `rejeitadas` | 0 | 1, 2 e 4 |
| `fora_da_lista` da ECU simulada (§5) | 0 | 1, 2 e 4 |
| Fora do esperado (abaixo) | 0 | 1 e 2 |
| `heap_min` | O do último STATUS igual ao do STATUS de 1 min | 2 e 4 |

O `corrupt` do decodificador é anotado, mas não reprova: uma mensagem corrompida que o CRC pegou é o protocolo funcionando. Algumas aparecem no início, com o texto da ROM do ESP32.

**Fora do esperado.** Nas fases 1 e 2, o simulador faz o papel da Pi e sabe o que enviou, então ele confere o conteúdo de cada amostra:
- **IMU:** a média e o pico de cada eixo precisam cair entre o menor e o maior valor da IMU_SIM enviados no intervalo da amostra, com uma folga para o trânsito pelo cabo (30 ms, a ajustar na implementação) e 1 unidade do cabo (0,01 m/s² ou 0,1 °/s).
- **OBD:** cada valor precisa ser a decodificação de uma resposta da ECU simulada para aquele PID, dada no instante que a idade informa, com a mesma folga.

Cada amostra fora disso soma 1 em `fora_do_esperado`. A conferência pega, de ponta a ponta, erros de média, de pico, de eixo, de fórmula e de idade, além de corrupção no cabo. Na fase 4, o simulador não vê as amostras, porque a Pi as lê; o conteúdo já foi provado na fase 2.

**Como rodar:**

| Fase | Comando |
|---|---|
| 1 | `python3 host/simulator.py --firmware .pio/build/native/program --roteiro host/roteiros/volta-no-bairro.txt --repetir --sem-tela --duracao 2h` |
| 2 | O mesmo, com `--porta /dev/vguard-esp32 --can can0` no lugar de `--firmware` |
| 4 | No notebook, `python3 host/simulator.py --so-carro --porta /dev/vguard-esp32 --can can0 --roteiro … --repetir --sem-tela --duracao 2h`; na Pi, `python3 host/link_test.py --porta /dev/ttyAMA5 --duracao 2h` |

**`host/link_test.py`** é o nosso papel da Pi na fase 4: abre a porta, usa o `Decoder` para enviar o PING e o TAXAS e, ao fim, imprime o mesmo resumo do simulador sem tela. Assim, o ensaio oficial não depende de o leitor da Isabela estar pronto. Durante o ensaio, o leitor dela fica parado, porque a porta tem um único leitor.

## 7. Horário (D12, T41)

A meta da D12 é um erro de no máximo 10 ms entre o horário entregue e o instante real da amostra, no relógio da Pi.

### 7.1 Pela conta

O ESP32 marca o PONG num instante entre o envio do PING (`t1`) e a chegada do PONG (`t2`). Por isso, o erro de um par de horários é de no máximo metade da ida e volta, mesmo com atrasos desiguais nos dois sentidos. O decodificador só aceita idas e voltas de até 20 ms, então o erro de cada par fica em até 10 ms.

- **Teste:** com o relógio falso, um teste do decodificador aplica atrasos desiguais e variáveis ao PING e ao PONG, e confere que o horário entregue fica dentro da metade da ida e volta.
- **Em todo ensaio:** o resumo anota o `rtt_ms` e o `slow_pongs`.

### 7.2 Medição com um fio (fase 4)

1. Grava-se o build de teste com o pulso ligado (§8). A cada segundo, o ESP32 muda um GPIO e manda um LOG com o `t_us` dessa mudança.
2. Um fio leva o GPIO18 do ESP32 ao GPIO16 da Pi, no pino 36 ([`BANCADA.md` §7.3](BANCADA.md)).
3. Na Pi, `python3 host/link_test.py --porta /dev/ttyAMA5 --pulso 16 --duracao 30min` marca cada borda com o relógio monotônico, o mesmo do decodificador, e converte o `t_us` do LOG com a conversão do próprio decodificador.
4. O resumo traz o erro máximo e o percentil 99 da diferença. O número vai para o T41.

**Critério:** erro máximo de até 10 ms (D12).

### 7.3 Quando as constantes mudam

As constantes do [`DECODIFICADOR.md` §3.8](DECODIFICADOR.md) não mudam por padrão. Um destes casos leva a uma análise antes do T41:
- erro máximo acima de 5 ms na medição com o fio, metade da D12;
- `slow_pongs` acima de 10% dos PINGs.

O `MAX_RTT_S` (20 ms) vem da D12 e não muda.

Exemplo: se a medição der 7 ms, o resultado ainda passa, mas o primeiro suspeito é a Pi ocupada com a visão, atrasando a leitura do PONG. O `slow_pongs` mostra se é isso.

## 8. Build de teste

Um ambiente à parte no PlatformIO, `vguard_test`, compila o firmware do ESP32 com ganchos de teste, ligados por constantes:

| Constante | O que faz | Para quê |
|---|---|---|
| `VGUARD_TEST_HANG_S` | Trava a tarefa do núcleo depois de N segundos | O ensaio do watchdog (§10.3) e a pane no osciloscópio (§10.4) |
| `VGUARD_TEST_PULSE_GPIO` | Muda o GPIO (o 18, [`BANCADA.md` §7.3](BANCADA.md)) a cada 1 s e manda um LOG com o `t_us` da mudança | A medição do horário (§7.2) |

- Os ganchos ficam no código da plataforma do ESP32 (`src/vguard/`), entre `#if`. O núcleo não os conhece.
- **O firmware entregue é sempre o ambiente `vguard`, que não tem nenhum gancho.** Assim, nenhum comando consegue derrubar o firmware entregue.
- O build de teste não é simulação, então não sai com ela (ADR-0003). Ele fica no projeto, como os testes.

## 9. Fase 1: notebook

**Passagem para a fase 2:**
- [ ] `./verifica.sh` passa (§3);
- [ ] os 4 roteiros rodam sem tela, com todos os `confere` certos (§4), inclusive com `--duas-centrais`;
- [ ] ensaio de 2 h e de 30 min nas taxas máximas, com os critérios do §6.

## 10. Fase 2: bancada

### 10.1 Montagem

São três placas ESP32 no mesmo barramento de bancada, com BAT43 e *pull-up* de 1 kΩ ([#2](https://github.com/V-guard-org/firmware_telemetria/issues/2)):

| Placa | Programa | Ligação ao notebook |
|---|---|---|
| Firmware | `vguard`, ou `vguard_test` nos ensaios do §8 | USB, `/dev/vguard-esp32` |
| Adaptador USB-CAN | Doggie ou a ponte própria ([#3](https://github.com/V-guard-org/firmware_telemetria/issues/3)) | USB, `/dev/vguard-adaptador`, que vira o `can0` pelo `slcand` |
| **Gerador de tráfego** | `tools/can-trafego/` | USB, `/dev/vguard-gerador`, só para alimentar e ler os contadores |

**O gerador de tráfego** é um programa pequeno em ESP-IDF, fora do firmware, como a ECU simulada:
- põe rajadas de quadros de ID prioritário, como `0x100`, no barramento; a carga é ajustável, e o ponto de partida é 50%;
- a velocidade (500 ou 250 kbit/s) é escolhida no código;
- imprime pela serial os seus contadores de erro (TEC e REC).

Ele serve para três coisas: o aceite do #2, com duas placas transmitindo ao mesmo tempo; a disputa perdida; e o carro com tráfego, porque o adaptador, limitado pela serial de 115200, passa no máximo uns 400 quadros/s, contra 2 000 a 4 000 num carro.

A fiação, os pinos e os nomes fixos das portas estão no [`BANCADA.md`](BANCADA.md) (ticket #11).

### 10.2 Aceite da bancada

Antes de testar o firmware, a bancada precisa passar nos aceites do [#2](https://github.com/V-guard-org/firmware_telemetria/issues/2) e do [#3](https://github.com/V-guard-org/firmware_telemetria/issues/3):

| Aceite | Critério |
|---|---|
| Linha parada | Cerca de 3,3 V, no multímetro |
| Nível baixo | Até 0,5 V |
| Subida até 2,5 V | Até 300 ns, com as três placas ligadas, no osciloscópio (§10.4) |
| 30 min a 500 kbit/s | Duas placas com o gerador de tráfego, inclusive IDs que disputam; TEC e REC em 0, sem alerta de erro |
| Adaptador (Doggie) | Os 4 critérios do #3: o `candump` mostra as requisições; um `cansend` chega ao ESP32; 10 min a 20 requisições/s sem perda; tirar e pôr o USB e repetir funciona. Até 2 h de tentativa; se falhar, a ponte própria |

### 10.3 O firmware na bancada

- **Roteiros:** os 4 roteiros com `confere`, pelo USB (§4).
- **Reinício pelo RTS:** "Reiniciar o ESP32" no simulador dá um boot novo, e abrir a porta não reinicia a placa.

**Falhas do CAN, provocadas à mão:**

| Caso | Como provocar | Resultado esperado |
|---|---|---|
| Sem ACK | Tirar da linha o adaptador e o gerador; o ESP32 fica sozinho, porque qualquer outro nó daria o ACK | DESCOBRINDO → SEM_RESPOSTA, sem FALHA_CAN; nova pergunta a cada 2 s |
| Velocidade errada, com tráfego | Gerador de tráfego a 250 kbit/s | VELOCIDADE_ERRADA a cada 10 s; `obd_enviadas` = 0 |
| Velocidade errada, barramento mudo | Gerador fora da linha e adaptador a 250 kbit/s | DESCOBRINDO → FALHA_CAN, três vezes → BLOQUEADO em cerca de 22 s; `obd_enviadas` = 3 |
| *Error passive* na leitura | Em LENDO, trocar o adaptador para 250 kbit/s | FALHA_CAN em cerca de 1 s, antes da regra dos 2 s; volta a ESCUTANDO depois de 10 s |
| Linha presa | Ligar a linha ao GND. É seguro: passam 3,3 mA pelo *pull-up*, e os diodos ficam reversos | LINHA_PRESA a cada 10 s; `obd_enviadas` não muda |
| Carro com tráfego e disputa perdida | Gerador a 500 kbit/s, com cerca de 50% de carga, durante 10 min de leitura | ESCUTANDO → DESCOBRINDO (quadros válidos); LENDO por 10 min; nenhum PID some da amostra; nenhum LOG de fila de eventos cheia |
| Pi ausente | `falhas.txt` (`pi ping desliga`) | ESPERANDO_PI depois de 3 s; GPIO27 em 1 (§10.4) |
| Watchdog | Build de teste com `VGUARD_TEST_HANG_S` | Reinício em cerca de 3 s; STATUS com `reinicio` = 5; OBD em ESPERANDO_PI; GPIO27 nunca em 0 (§10.4) |

- **Tirar uma placa da linha** é tirar o diodo dela da protoboard e o fio do RX, com a placa ainda no USB. Uma placa sem energia nunca fica na linha ([`BANCADA.md` §3.3](BANCADA.md)).
- Os dois casos de velocidade errada provam juntos que o PCNT e o TWAI leem o GPIO26 ao mesmo tempo: o PCNT conta as transições, e o TWAI, os quadros válidos.
- O *bus-off* não é provocado na bancada: na leitura, o *error passive* chega antes e já leva a FALHA_CAN. Ele se prova com a porta falsa ([`PLATAFORMA.md` §6](PLATAFORMA.md)).

### 10.4 Sessão de osciloscópio

O osciloscópio fica no laboratório, então todas as medidas são feitas numa única ida. Repete-se só a medida afetada, se o código do CAN mudar depois.

| Medida | Canais | Critério |
|---|---|---|
| Subida da linha | Linha do barramento | Até 300 ns até 2,5 V |
| `talk()` | GPIO27 e TX do TWAI | GPIO27 em 0 pelo menos 10 µs antes da 1ª borda do TX |
| `listen()` | GPIO27 e TX | GPIO27 em 1 antes de o nó voltar a escutar; nenhuma borda do TX depois |
| Escuta antes de falar | GPIO27 e TX | Nenhuma borda do TX antes de 1 s da 1ª escuta |
| Boot | GPIO27, disparo único na descida | Nenhum pulso em 0 do EN até a 1ª linha do `app_main` |
| `esp_restart()` e pane | GPIO27, disparo único na descida | Nenhum pulso em 0 durante o reinício; a pane vem do build de teste |
| Pi ausente | GPIO27 | Volta a 1 até 3 s depois do último PING |

Na bancada de diodos não há transceptor: mede-se o pino, que é o que o firmware controla. Um resistor de 10 kΩ na protoboard segura o GPIO27 em 1 quando o firmware o solta, como o resistor do transceptor faria; sem ele, o pino ficaria solto e a medida não valeria ([`BANCADA.md` §4](BANCADA.md)). Os pontos de prova estão no [`BANCADA.md` §10](BANCADA.md). O transceptor real é conferido na sessão com transceptor (§10.7).

### 10.5 Ensaio

Ensaio de 2 h e de 30 min nas taxas máximas, pelo USB (§6).

### 10.6 Passagem para a fase 4

- [ ] aceite da bancada (§10.2);
- [ ] roteiros, reinício pelo RTS e falhas do CAN (§10.3);
- [ ] sessão de osciloscópio (§10.4);
- [ ] ensaio de 2 h e de 30 min (§6).

### 10.7 Sessão com transceptor

Quando os TJA1051T/3 chegarem, uma sessão curta, de 1 a 2 h, troca os diodos por dois transceptores: um na placa do firmware e outro no adaptador ([`BANCADA.md` §9](BANCADA.md)). Ela prova o que a bancada de diodos não prova: o receptor funcionando com o transceptor silencioso, que é o que a escuta usa, e o jumper do GPIO27 cortando a transmissão de verdade.

- [ ] leitura normal, com um roteiro e o `confere`;
- [ ] VELOCIDADE_ERRADA com o adaptador a 250 kbit/s e tráfego, o que prova o receptor durante a escuta;
- [ ] com o jumper aberto, nenhuma requisição nossa no `candump`;
- [ ] sem a Pi, nenhum quadro do ESP32 no `candump` no boot, no reinício e na pane do build de teste.

A sessão não faz parte da passagem para a fase 4. É um passo obrigatório antes do carro (§12), porque completa o item de segurança do transceptor silencioso (§2).

## 11. Fase 3: a Pi

**Nosso:**
- [ ] `python3 -m unittest discover -s host` passa no Python da Pi, sem instalar nada;
- [ ] **laço da UART5:** um fio do pino 32 (TX) ao 33 (RX) e 1 MB a 460800 baud, com `python3 host/link_test.py --laco --porta /dev/ttyAMA5`, sem nenhum erro. Isso prova o `dtoverlay` e a velocidade antes de ligar o ESP32 ([`BANCADA.md` §6](BANCADA.md)).

**Sugestão para a Isabela,** porque o código é dela: o leitor dela com o ESP32 falso (`host/fake_esp32.py`) por 10 min, com amostras chegando ao `EventManager` e nenhuma exceção.

## 12. Fase 4: integração (T40, 05/11)

O ESP32 fica ligado à Pi pelos fios da UART2, com o fio de 5 V desligado enquanto o USB estiver ligado ([`BANCADA.md` §7](BANCADA.md)). O notebook roda o simulador com `--so-carro`: manda a IMU_SIM pelo USB e responde no `can0` ([ADR-0009](adr/0009-imu-sim-pelo-usb-na-fase-4.md)).

**Passagem para o carro:**
- [ ] sessão com transceptor (§10.7);
- [ ] ensaio de 2 h e de 30 min pelos fios, com o `link_test.py` na Pi (§6). É o T24.2;
- [ ] medição do horário com o fio, por 30 min (§7.2). É a parte do firmware no T41;
- [ ] todos os itens de segurança provados (§2).

## 13. Ensaio no carro

Fica para quando o carro for definido (#13 Q12), num ticket próprio. Ideias já levantadas:
- **Testemunha:** a placa do adaptador, com um transceptor próprio, só escutando (o comando `L` do Doggie), grava tudo o que passa no barramento. Ela precisa de um 2º TJA1051T/3.
- **Jumper do GPIO27 primeiro tirado:** o resistor trava o transceptor em silencioso, e o firmware só escuta. Depois, com o jumper posto, a descoberta e a leitura.
- **Quando parar:** um quadro nosso fora da lista na testemunha, uma luz nova no painel ou `bloqueadas` > 0.
- **Tempos do grupo 3** (§14), conferidos com o arquivo da testemunha.

## 14. Tempos: de onde vêm e quem os muda

Os tempos do [`MAQUINAS-DE-ESTADO.md` §2.7](MAQUINAS-DE-ESTADO.md) têm três origens. A bancada só confere que todos funcionam como estão escritos, porque nela o carro é a ECU simulada, que responde como a configurarmos.

| Grupo | Tempos | Quem muda |
|---|---|---|
| **1. Norma e datasheet** | Prazo de resposta de 50 ms (ISO 15765-4); liberação de 10 µs (datasheet); linha presa acima de 10 ms (um quadro válido nunca tem mais de 5 bits dominantes seguidos) | Ninguém. O osciloscópio confere os 10 µs |
| **2. Escolhas nossas** | Escuta de 1 s; esperas de 10 s; 3 falhas até o bloqueio; RPM e velocidade a cada 200 ms e os outros PIDs a cada 1 s; 20 requisições/s; Pi ausente em 3 s | Nós, só se um ensaio mostrar um problema visível |
| **3. Comportamento do carro** | 2 s sem resposta, que cobre as centrais mudas na partida; 5 faltas para tirar um PID da agenda | Só o ensaio no carro (§13) |

Um valor errado do grupo 3 não é risco de segurança. Por exemplo, se as centrais de um carro ficarem mudas por 2,5 s na partida, o OBD cai em SEM_RESPOSTA a cada partida e volta cerca de 4,5 s depois, perdendo dados nesse intervalo.

As constantes do horário seguem o §7.3.

## 15. Registro dos ensaios

Cada ensaio, de bancada ou de 2 h, deixa um arquivo em `docs/ensaios/AAAA-MM-DD-nome.md`:

```
# Ensaio de 2 h · fase 2 · 30/10/2026

- Firmware: 0.4.0-a58af37 (do STATUS) · decodificador 1.0.0
- Montagem: bancada de 3 placas, USB a 460800
- Comando: python3 host/simulator.py …

| Critério | Esperado | Visto | |
|---|---|---|---|
| Perdidas | < 0,1% | 0,000% | ✔ |
| …

Resumo JSON:
{"duracao_s": 7200, …}

Pendências: …
```

- As pendências de uma passagem de fase (§2) ficam nesse arquivo, com o prazo.
- As gravações do `--gravar` ficam fora do git: 2 h dão dezenas de MB.

## 16. Fora deste documento

- **[`BANCADA.md`](BANCADA.md)** (ticket #11): a fiação da bancada de três placas, os nomes fixos das portas, o fio do horário, o laço da UART5, os fios até a Pi e a montagem da sessão com transceptor.
- **Ensaio no carro:** um ticket próprio, quando o carro for definido (§13).
- **Leitor da Isabela:** os testes do código dela; aqui só há uma sugestão (§11).
- **T41 da equipe:** os horários da câmera e do GPS são de quem cuida deles. Daqui sai só o número do ESP32 (§7.2).
