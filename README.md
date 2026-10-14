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
| Mapa de decisões em aberto | [issue #1](https://github.com/V-guard-org/firmware_telemetria/issues/1) (etiqueta `wayfinder:map`) |
| Resultados de pesquisas | ramos `research/*` |
| Histórico do planejamento (superado) | `docs/PLANO.md`, `docs/notas-equipe.md` |

## Fases

1. **Notebook:** firmware, ECU simulada e parte da Pi rodam no PC, com CAN e UART virtuais.
2. **ESP32:** o mesmo firmware vai para o ESP32, na bancada com dois ESP32.
3. **Raspberry Pi:** a parte da Pi vai para a Pi.
4. **Integração:** ESP32 e Pi pelos fios (T40, 05/11) e ensaio no carro (25/11).

Projeto da disciplina Oficina de Integração 2 (UTFPR). Equipe: Nicolas Rossi Gariba, Pedro Eugênio Marin do Nascimento, Isabela Bella Bortoleto e Thiago Moreira Gomes de Lima.
