# Firmware de telemetria — V-GUARD

Firmware do ESP32 do V-GUARD: lê a IMU e o OBD-II do carro e entrega amostras à Raspberry Pi pela UART.

O planejamento foi concluído em 13/10/2026. O desenvolvimento começa pela fase 1, com tudo rodando no notebook.

## Onde está cada coisa

| O quê | Onde |
|---|---|
| Glossário (use estes termos em código, issues e docs) | [`CONTEXT.md`](CONTEXT.md) |
| Decisões de arquitetura | [`docs/adr/`](docs/adr/) |
| Protocolo ESP32 ↔ Pi (mensagens, bytes e comportamento) | [`docs/PROTOCOLO.md`](docs/PROTOCOLO.md) |
| Máquinas de estado (boot, OBD no CAN e IMU) | [`docs/MAQUINAS-DE-ESTADO.md`](docs/MAQUINAS-DE-ESTADO.md) |
| Núcleo portátil e camada de plataforma (portas, eventos, IMU simulada, estrutura do código) | [`docs/PLATAFORMA.md`](docs/PLATAFORMA.md) |
| Decodificador Python, guia do leitor da Pi e ESP32 falso | [`docs/DECODIFICADOR.md`](docs/DECODIFICADOR.md) |
| Simulador do PC (modelo do carro, controles, roteiros e tela) | [`docs/SIMULADOR.md`](docs/SIMULADOR.md) |
| Mapa de decisões em aberto | [issue #1](https://github.com/V-guard-org/firmware_telemetria/issues/1) (etiqueta `wayfinder:map`) |
| Resultados de pesquisas | ramos `research/*` |
| Protótipos descartáveis | ramo `prototype/simulador` (#9) |
| Histórico do planejamento (superado) | `docs/PLANO.md`, `docs/notas-equipe.md` |

## Fases

1. **Notebook:** firmware, ECU simulada e parte da Pi rodam no PC, com CAN e UART virtuais.
2. **ESP32:** o mesmo firmware vai para o ESP32, na bancada com dois ESP32.
3. **Raspberry Pi:** a parte da Pi vai para a Pi.
4. **Integração:** ESP32 e Pi pelos fios (T40, 05/11) e ensaio no carro (25/11).

## Ferramenta

As versões ficam fixas até o ensaio no carro (25/11), para que uma atualização não mude o driver nem o ESP-IDF no meio do caminho (ticket [#14](https://github.com/V-guard-org/firmware_telemetria/issues/14)).

| Peça | Versão |
|---|---|
| PlatformIO | 6.2.0, instalado com `uv tool install --managed-python --python 3.14 platformio` |
| Plataforma | `platformio/espressif32@7.1.3`, que traz o ESP-IDF 6.1.0 |
| Driver CAN | O TWAI novo (`esp_twai.h`). O antigo (`driver/twai.h`) fica como reserva: ainda existe no ESP-IDF 6.1, e trocar mexe só no arquivo de CAN da camada de plataforma. |

Para a fase 1, o notebook precisa do CAN virtual. Os comandos pedem sudo, e os três últimos precisam ser repetidos a cada reinício:

```
sudo apt install can-utils
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up
```

Projeto da disciplina Oficina de Integração 2 (UTFPR). Equipe: Nicolas Rossi Gariba, Pedro Eugênio Marin do Nascimento, Isabela Bella Bortoleto e Thiago Moreira Gomes de Lima.
