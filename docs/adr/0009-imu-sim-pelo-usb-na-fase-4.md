# Na fase 4, a IMU simulada recebe a IMU_SIM pelo USB

Na fase 4, os fios até a Pi ficam numa UART do ESP32 que não é a UART0. A UART0, que é a porta USB da DevKit, fica com o notebook: enquanto o firmware tiver a IMU simulada, o simulador do PC continua mandando a IMU_SIM (`0x83`) por ela. Por essa porta, o firmware aceita só mensagens de desenvolvimento.

A Pi nunca envia a IMU_SIM ([`PROTOCOLO.md` §4.3](../PROTOCOLO.md)). Sem este caminho, um ESP32 com a IMU simulada e ligado à Pi ficaria sem IMU: depois de 1 s sem IMU_SIM, as leituras falham e a IMU entra em FALHA. Isso aconteceria em dois casos:
- na integração do T40 (05/11), se a IMU real (T14.1) ainda não tiver chegado;
- na demonstração, se o ensaio no carro falhar e o projeto seguir só com a simulação (ADR-0003).

Nos dois casos, o OBD vem da ECU simulada, no mesmo notebook. Assim, um único modelo do carro gera a IMU e o OBD, coerentes entre si, e a Pi de verdade faz o resto da cadeia ([`SIMULADOR.md`](../SIMULADOR.md), modo `--so-carro`).

## Opções consideradas

- **Percurso fixo no firmware:** sem IMU_SIM, a IMU simulada tocaria um percurso gravado. A IMU ficaria incoerente com o OBD da ECU simulada, por exemplo freando enquanto a velocidade sobe, e entraria mais simulação no dispositivo.
- **A Pi enviar a IMU_SIM:** mudaria o leitor da Isabela, e a Pi precisaria do modelo do carro.
- **Nada:** só serviria se a IMU real chegasse até o T40 e funcionasse.

## Consequências

- O #11 escolhe para os fios uma UART que não seja a UART0. Ele também confere a alimentação com o USB e os fios ligados ao mesmo tempo.
- A plataforma do ESP32 também lê a UART0 quando a ligação com a Pi está em outra UART e a IMU é simulada. O núcleo ganha uma segunda entrada de bytes, que só entrega mensagens de desenvolvimento ([`PLATAFORMA.md` §5](../PLATAFORMA.md)).
- Com a IMU real, a UART0 não é lida. Quando a simulação for apagada, essa leitura sai junto.
- O USB fica livre na fase 4: dá para regravar o firmware por ele, com um notebook. Os fios da Pi também deixam de receber o texto que a ROM imprime na UART0 no boot.

## Nota (ticket #11, 15/10/2026)

Os fios da Pi ficam na UART2 do ESP32 (TX = GPIO17, RX = GPIO16). Com o USB ligado, o fio de 5 V da Pi fica desligado, porque a placa aceita uma fonte só; os fios de sinal levam um resistor de 1 kΩ, porque o ESP32 e a Pi deixam de ligar e desligar juntos ([`BANCADA.md` §7](../BANCADA.md)).
