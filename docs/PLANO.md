# Plano do firmware — V-GUARD (ESP32)

> **Rascunho superado.** Várias decisões deste arquivo foram revistas no planejamento: FW-05/06/07 mudaram, e a FW-06 virou o oposto. Valem `docs/adr/`, `CONTEXT.md`, `docs/notas-equipe.md` e o mapa de decisões no GitHub. Este arquivo fica só como histórico.

Versão inicial: 13/10/2026. Cobre as tarefas T17.1, T22, T23, T24 e a preparação para T40/T41/T43.3 do cronograma.

Referência de requisitos: `proposta_revisada.pdf` (Plano de Projeto de 23/09, em `V-guard-org/visao/docs/`). A numeração de RF/RNF usada aqui é a dessa versão. O `v_guard.pdf` desta pasta é o roteiro anterior.

---

## 1. Escopo

O ESP32 é o nó de aquisição. Ele lê a IMU e o OBD-II e envia a telemetria à Raspberry Pi pela UART. Não grava nada, não fala com o backend e não lê o GPS.

| Requisito | O que significa no firmware |
|---|---|
| RF01 | IMU amostrada continuamente em taxa fixa |
| RF03 | Descobrir os PIDs suportados (Mode 01, PIDs 0x00/0x20/0x40) |
| RF04 | RPM (0x0C) e velocidade (0x0D) |
| RF05 | Carga 0x04, temp. líquido 0x05, MAP 0x0B, temp. ar 0x0F, acelerador 0x11, tensão do módulo 0x42 |
| RF10 | A Pi sempre tem a amostra mais recente de IMU e OBD (fluxo contínuo) |
| RF29 | Intervalos de envio configuráveis pela Pi |
| RNF07 | Protocolo UART definido, com detecção de erro e perda |
| RNF08 | Transceptor CAN de 3,3 V |
| RNF10, AE02, AE04 | Só requisições de diagnóstico padronizadas saem no CAN, garantidas por um filtro no código |
| RNF19, RNF20 | PID ausente, carro desligado ou CAN em falha não param a IMU |
| RNF24, AE15 | A IMU é agregada no ESP32; a Pi recebe dado consolidado |
| RF34, RF35 | Frenagem brusca e impacto: extensão, só depois do MVP |

Fora do escopo do firmware: GPS e PPS (ficam na Pi), visão, eventos, armazenamento, sincronização com o backend.

## 2. Decisões

| ID | Decisão | Motivo |
|---|---|---|
| FW-01 | PlatformIO + ESP-IDF, em C++ | Escolha da equipe. FreeRTOS, TWAI, watchdog e NVS nativos, e a lógica portátil roda nos testes no PC. |
| FW-02 | GPS e PPS ligados na Pi | Arquitetura da Fig. 3. O relógio da Pi fica preso ao GPS (gpsd + chrony com PPS). |
| FW-03 | O ESP32 marca cada amostra com µs desde o boot; a Pi converte para horário real | Troca PING/PONG pela UART, ficando com a medida de menor tempo de ida e volta. Erro esperado ≤ 2 ms, contra ~33 ms entre frames da câmera. |
| FW-04 | Protocolo binário: quadros COBS com CRC16 e número de sequência | Compacto, se recupera de lixo na linha (inclusive as mensagens do boot do ESP32) e detecta perda. A Pi só enxerga dicionários Python. |
| FW-05 | Os PIDs são decodificados no ESP32, que envia um *snapshot* OBD completo em intervalo fixo | As fórmulas ficam num lugar só e testado. Além disso, `EventManager.update()` substitui a última amostra da fonte inteira, então mandar um PID por vez apagaria os outros. |
| FW-06 | Listen-only só para detectar a velocidade do barramento; depois, modo normal com filtro de transmissão | Em listen-only não dá para pedir PIDs. O filtro torna RNF10/AE02 verificáveis por teste. |
| FW-07 | Cada fonte tem uma versão real e uma simulada, escolhidas em tempo de execução (salvas na NVS) | Permite combinações na bancada, como IMU real com OBD simulado. |
| FW-08 | A UART do enlace é configurável: UART0/USB no desenvolvimento, UART2/GPIO ou USB na Pi | Os logs do ESP-IDF viajam dentro do protocolo (mensagem LOG), então um único cabo USB basta. |
| FW-09 | Um segundo ESP32 faz o papel de ECU simulada num barramento CAN real de bancada | A equipe tem 2+ ESP32 clássicos. Sem transceptor, o barramento é montado com diodos (§6). |
| FW-10 | Código no repositório `firmware_telemetria` da org `V-guard-org` | Um repo por módulo, como já fazem `visao` e `gerenciador_eventos`. |

## 3. Arquitetura

```
              ESP32 "vguard"                                  Raspberry Pi 4
 ┌────────────────────────────────────────────┐        ┌──────────────────────────────┐
 │ ImuSource ─100 Hz─► imu_task ─agrega 50 Hz─┐│        │ vguard_link (Python)         │
 │  [sim | mpu]                               ▼│  UART  │  CRC, sequência, tempo→epoch │
 │                                     fila TX ┼──────► │  └► EventManager.update(     │
 │ CanBus ◄──► obd_task ─snapshot 5 Hz────────┘│ ◄───── │       "imu" / "obd", ts=...) │
 │  [twai | virtual]   │                       │ PING,  │                              │
 │             filtro de TX (whitelist)        │ CONFIG └──────────────────────────────┘
 │ supervisor: estado, watchdog, STATUS 1 Hz  │
 └──────────────┬─────────────────────────────┘
                │ CAN  (bancada: 2 fios + diodos | carro: transceptor + tomada OBD-II)
       ┌────────┴─────────┐
       │ ESP32 "ecu_sim"  │  responde ao Mode 01 como 0x7E8 (e 0x7E9)
       └──────────────────┘
```

### 3.1 Tarefas FreeRTOS (app `vguard`)

| Tarefa | Função |
|---|---|
| `imu_task` | Lê a IMU a 100 Hz por timer, agrega (média e |a| máximo) e publica a 50 Hz |
| `obd_task` | Máquina de estados do OBD (§3.2), *polling* dos PIDs e snapshot periódico |
| `link_tx_task` | Tira mensagens da fila, serializa, aplica COBS e escreve na UART. Fila cheia descarta a mais velha e conta o descarte. |
| `link_rx_task` | Recebe comandos da Pi (PING, CONFIG_SET/GET) e responde |
| `supervisor` | Boot, configuração, STATUS a 1 Hz, watchdog de tarefas, presença da Pi |

Todas as tarefas ficam no watchdog do ESP-IDF. Uma tarefa travada reinicia o ESP32, e a Pi percebe pelo HELLO.

### 3.2 Máquina de estados do OBD

```
 OFF ─► DETECT ─► DISCOVER ─► POLL ──(timeouts seguidos)──► SILENT ─(a cada 5 s)─► DETECT
           ▲                                                                         │
           └─────────────── BUS_OFF ─► RECOVER ◄─(erro grave no barramento)───────────┘
```

- **DETECT:** escuta em listen-only a 500 e depois a 250 kbit/s. Se o barramento estiver em silêncio (comum quando há gateway), segue a sequência do ISO 15765-4: envia `01 00` em 500k/11 bits, 500k/29 bits, 250k/11 bits e 250k/29 bits.
- **DISCOVER:** pede os bitmaps `01 00/20/40` e guarda quais ECUs responderam e quais PIDs elas suportam.
- **POLL:** RPM e velocidade a cerca de 10 Hz; os demais PIDs suportados em rodízio, cerca de 1,5 Hz cada. No máximo cerca de 20 requisições/s. Só pede PIDs suportados (RNF20).
- **SILENT:** a ignição desligada é tratada como estado normal, não como erro. A IMU segue funcionando.

### 3.3 Fontes reais e simuladas

| Interface | Real | Simulada |
|---|---|---|
| `ImuSource` | Driver I²C (modelo da IMU a definir; o driver tem cerca de 1 dia de trabalho) | `SimImu`: perfil de direção (parado, aceleração, cruzeiro, curva, frenagem) com ruído |
| `CanBus` | `TwaiBus`: controlador TWAI do ESP32, usado também na bancada com 2 ESP32 | `VirtualBus`: ECU em memória, para testes no PC ou um único ESP32 |

O perfil de direção fica numa biblioteca portátil (`lib/sim`), compartilhada entre a `SimImu` e o app `ecu_sim`.

### 3.4 Estrutura do repositório

```
firmware_telemetria/
├── platformio.ini     ambientes: vguard, ecu_sim (esp32dev) e native (testes no PC)
├── lib/               código portátil, sem ESP-IDF, testado no PC
│   ├── proto/         COBS, CRC16, mensagens
│   ├── obd/           requisição, decodificação, bitmap, agendador, filtro de TX
│   ├── agg/           agregação da IMU
│   └── sim/           perfil de direção
├── src/
│   ├── vguard/        app principal: tarefas, drivers, máquinas de estado
│   └── ecu_sim/       app da ECU simulada
├── test/              testes Unity (pio test -e native)
├── host/
│   ├── vguard_link/   pacote Python: decodificador, tempo, leitor serial → EventManager
│   └── tools/         monitor.py (imita a Pi), ensaio de longa duração
└── docs/              PLANO.md, PROTOCOLO.md, BANCADA.md
```

## 4. Protocolo ESP32 ↔ Pi (rascunho)

Este é um rascunho; a especificação completa sai em `docs/PROTOCOLO.md` no M1.

- **Quadro:** `COBS(cabeçalho + corpo + CRC16-CCITT) + 0x00`, em little-endian, com até 256 bytes.
- **Cabeçalho:** `versão u8 | tipo u8 | seq u16`. As mensagens de dados levam `t_us u64` (µs desde o boot).
- **Velocidade da UART:** 460800 baud por padrão. O tráfego previsto é de cerca de 3 kB/s, então sobra margem.

| Tipo | Sentido | Conteúdo |
|---|---|---|
| HELLO | ESP→Pi | Versão do firmware, motivo do reset, `boot_id`, configuração ativa |
| STATUS | ESP→Pi | A 1 Hz: uptime, heap, contadores (enviados, descartados, CRC), estado CAN/OBD, fontes ativas |
| IMU | ESP→Pi | `t_us`, ax ay az (m/s²), gx gy gz (°/s), \|a\| máximo da janela |
| OBD | ESP→Pi | `t_us`, lista de (pid, valor, idade em ms) |
| OBD_SUPPORTED | ESP→Pi | ECU e bitmap dos PIDs 0x01–0x60 |
| LOG | ESP→Pi | Nível e texto (logs do ESP-IDF redirecionados) |
| PING / PONG | Pi→ESP / ESP→Pi | Horário da Pi ecoado e `t_us`, para a sincronização de tempo |
| CONFIG_SET / CONFIG_GET | Pi→ESP | Chave e valor, persistidos na NVS |
| ACK / NACK | ESP→Pi | Sequência do comando e código de resultado |

O que chega ao código da Isabela, com `ts` em epoch:

```python
manager.update("imu", {"ax": .., "ay": .., "az": .., "gx": .., "gy": .., "gz": .., "a_max": ..}, ts=...)
manager.update("obd", {"rpm": 2100.0, "speed": 60.0, "coolant": 88.0, ...}, ts=...)
```

## 5. OBD-II

| PID | Nome no dicionário | Fórmula | Unidade |
|---|---|---|---|
| 0x04 | `load` | A·100/255 | % |
| 0x05 | `coolant` | A − 40 | °C |
| 0x0B | `map` | A | kPa |
| 0x0C | `rpm` | (256A + B)/4 | rpm |
| 0x0D | `speed` | A | km/h |
| 0x0F | `iat` | A − 40 | °C |
| 0x11 | `throttle` | A·100/255 | % |
| 0x42 | `voltage` | (256A + B)/1000 | V |

**Filtro de transmissão (RNF10/AE02):** só passa um quadro com ID 0x7DF ou 0x7E0–0x7E7 (11 bits), ou 0x18DB33F1 / 0x18DAxxF1 (29 bits), cujo conteúdo seja uma requisição de modo 01 (ou 09, reservado). Qualquer outro quadro é descartado e contado. O teste no PC cobre todos os casos.

## 6. Simulação e bancada

| Nível | Ambiente | O que valida |
|---|---|---|
| 0 | PC: `pio test -e native` e pytest | Protocolo, decodificação OBD, filtro de TX, agregação, máquinas de estado com barramento e relógio falsos |
| 1 | ESP32 `vguard` + PC por USB | Firmware real, UART real, IMU simulada e `VirtualBus`. Já permite integrar com a Pi. |
| 2 | 2 ESP32 em CAN de bancada + PC | TWAI real, detecção de velocidade, PID ausente, ECU que para de responder, *bus-off* |
| 3 | Carro (25/11) | Transceptor, tomada OBD e lista de PIDs do veículo |

**Fiação provisória para 2 ESP32 sem transceptor** (a validar):

- Em cada ESP32, o TX do TWAI vai a um diodo, com o catodo no TX e o anodo na linha comum. De preferência um Schottky, como BAT43 ou 1N5817.
- O RX vai direto na linha comum.
- A linha tem um *pull-up* de 4,7 kΩ para 3,3 V, e os GNDs são comuns.
- Pinos provisórios: TWAI TX = GPIO25, RX = GPIO26, I²C da IMU em SDA = GPIO21 e SCL = GPIO22.
- Quando chegarem dois SN65HVD230, troca-se só a fiação (com terminação de 120 Ω). O código não muda.

## 7. Testes e critérios de aceite

| Item | Critério |
|---|---|
| Enlace (T24.2) | Ensaio de 2 h a 50 Hz de IMU e 5 Hz de OBD com menos de 0,1% de lacunas de sequência e nenhum quadro com CRC válido mal decodificado |
| Tempo (apoio ao T41) | Erro do mapeamento de `t_us` para epoch ≤ 2 ms, medido com eco |
| OBD (T23.2) | Snapshot com todos os PIDs suportados pela `ecu_sim`; PID removido da `ecu_sim` some sem afetar a IMU; ECU desligada leva a SILENT e, ao religar, volta a POLL sozinha |
| Filtro CAN | 100% dos casos de teste fora da whitelist bloqueados |
| Robustez (T22.2) | Tarefa travada leva a reboot pelo watchdog, HELLO e retomada do fluxo; cabo USB removido e recolocado faz o leitor reconectar sozinho |

O CI no GitHub Actions roda os testes no PC, a compilação dos dois apps e o pytest do `host/`.

## 8. Cronograma

| Marco | Período | Entregável | Tarefas |
|---|---|---|---|
| M0 Fundação | 13–15/10 | Repo, PlatformIO + ESP-IDF compilando `vguard` e `ecu_sim`, testes no PC rodando, CI, HELLO saindo do ESP32 | T22.1 |
| M1 Enlace + IMU sim | 16–21/10 | `PROTOCOLO.md`, `lib/proto`, decodificador Python, `monitor.py`, sincronização de tempo, STATUS/LOG, `SimImu` agregada; `vguard_link` alimentando um `EventManager` real no PC | T22.2, T23.1, T24.1 |
| M2 OBD-II | 22–28/10 | `lib/obd` com testes, filtro de TX, máquina de estados OBD, `VirtualBus`, app `ecu_sim`, bancada com 2 ESP32 | T17.1, T23.2 |
| M3 Robustez | 29/10–03/11 | Watchdog, recuperação de *bus-off*, CONFIG/NVS (RF29), reconexão do leitor, ensaio de 2 h, injeção de falhas | T22.2, T23.3, T24.2 |
| Folga | 04/11 | Documentação e ajustes | — |
| Integração | 05/11 | Leitor rodando na Pi com o `gerenciador_eventos` | T40.1 |
| M4 Hardware real | até 18/11, conforme a chegada | Driver da IMU, transceptor, lista de PIDs do carro, verificação de timestamps | T15, T17.3, T41 |
| Carro | 25/11 | Ensaio controlado | T43.3 |

Ficam para depois do MVP: RF34/RF35 (frenagem e impacto, com os dados de 100 Hz no ESP32), Mode 09 (VIN) e RF33 (comparação de velocidade, que é do lado da Pi).

## 9. Riscos

| Risco | Mitigação |
|---|---|
| Problemas de versão ou de múltiplos apps no PlatformIO + ESP-IDF | Validado logo no M0. Se não der, dois projetos PlatformIO compartilhando `lib/`. |
| Barramento com diodos instável | Reduzir para 125 kbit/s na bancada. Comprar 2 × SN65HVD230 (cerca de R$ 10 cada) de qualquer forma. |
| Carro de teste com OBD silencioso, atrás de gateway ou sem CAN (K-line) | Definir o carro até 27/10. Se falhar, a demonstração usa a `ecu_sim`. |
| Modelo da IMU indefinido | A interface `ImuSource` isola o driver. |
| Prazo de 3 semanas | Os cortes já estão listados no §8. Se apertar, o suporte a 29 bits sai antes de qualquer coisa do núcleo. |

## 10. Pendências

1. **Leitor da Pi (`vguard_link`):** a proposta é que fique com a equipe do firmware, no `host/` deste repo, importado pela Pi. Falta confirmar com a Isabela.
2. **Modelos da IMU e do transceptor** (T14.1, T10).
3. **Carro do ensaio:** modelo e ano até 27/10, para a lista de PIDs (T17.3).
4. **Ligação física Pi ↔ ESP32:** USB ou UART por GPIO. O firmware suporta as duas; isso só afeta a fiação do T19.1.
5. **Criar o repo em `V-guard-org` e enviar o código:** aguardando autorização.

### Para combinar com a Isabela (`gerenciador_eventos`)

- `update()` substitui a última amostra da fonte inteira. Por isso o firmware manda o snapshot OBD completo, e não precisa mudar nada no código dela.
- O `ts` que o leitor entrega vem do relógio da Pi. Para ser horário GPS, a Pi precisa de chrony com PPS (parte do T16).
- Taxa prevista: cerca de 55 amostras/s (IMU a 50 Hz e OBD a 5 Hz), abaixo das ~100/s para as quais o buffer foi dimensionado.
