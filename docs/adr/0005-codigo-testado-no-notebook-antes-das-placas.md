# O código é escrito e testado no notebook antes de ir para as placas

O desenvolvimento segue quatro fases:
1. **Notebook:** tudo roda no PC, incluindo o firmware, a ECU simulada e a parte da Pi, com CAN e UART virtuais.
2. **ESP32:** o mesmo firmware vai para o ESP32, na bancada.
3. **Raspberry Pi:** a parte da Pi vai para a Pi.
4. **Integração** de ESP32 e Pi pelos fios, e depois o carro.

Assim dá para desenvolver e analisar sem hardware, sem carro e sem disputar a Pi com a equipe de visão.

Para o firmware rodar no notebook, a lógica (OBD, agregação, protocolo, máquinas de estado) é C++ portátil. Só uma camada fina de plataforma muda entre o notebook e o ESP32:

| | Notebook | ESP32 |
|---|---|---|
| Tarefas | threads | FreeRTOS |
| CAN | `vcan` do Linux (SocketCAN) | TWAI |
| UART | par de pseudo-terminais | UART |

## Opções consideradas

- **Emular o ESP32 (QEMU da Espressif, Wokwi):** roda o binário real, mas, até onde sabemos, nenhum dos dois emula o controlador CAN.
- **Alvo `linux` do ESP-IDF:** é experimental, não é suportado pelo PlatformIO e não tem drivers de CAN nem de UART.

## Consequências

- A ECU simulada é um programa no notebook. Na fase 1 ela fala no `vcan`; na fase 2 fala no barramento real através do segundo ESP32, usado como adaptador USB-CAN (protocolo slcan). Ela é escrita uma única vez.
- Na fase 2, o notebook conversa com o ESP32 pelo USB dele: manda a IMU simulada e lê as amostras. Os fios só entram na fase 4, trocando uma constante no firmware.
- O build para o notebook não entra no binário do ESP32, então não conta como "simulação no firmware" para a ADR-0003.

## Nota (ticket #10, 14/10/2026)

No notebook, as "threads" da tabela viraram um único laço com `poll()`: o código portátil é um núcleo passivo, chamado pela plataforma ([ADR-0008](0008-nucleo-passivo-chamado-pela-plataforma.md)). A camada de plataforma está descrita em [`docs/PLATAFORMA.md`](../PLATAFORMA.md).
