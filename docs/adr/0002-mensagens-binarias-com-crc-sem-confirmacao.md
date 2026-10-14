# Mensagens binárias com CRC entre ESP32 e Pi, sem confirmação das amostras

O Plano de Projeto citava JSON ou CSV no cabo ESP32 → Pi. Escolhemos mensagens binárias delimitadas (COBS), com CRC16 e número de sequência:
- são 3 a 4 vezes menores;
- uma mensagem corrompida por ruído nunca passa como válida.

A Pi lê com um decodificador que a equipe do firmware entrega, feito só com a biblioteca padrão do Python, porque o `gerenciador_eventos` não tem dependências.

As amostras não são confirmadas nem reenviadas, e o ESP32 não guarda o que leu enquanto a Pi não está escutando: sem a Pi não há evento para contextualizar, e a sequência basta para contar perdas.

O ESP32 transmite desde o boot, sem esperar a Pi, e não existe mensagem de apresentação (HELLO). A Pi pode abrir a porta muito depois do boot, então tudo o que essa mensagem levaria vai no STATUS, que sai no boot e depois 1 vez por segundo.

Os comandos da Pi também não têm resposta própria (ACK). O STATUS mostra a configuração em uso, e o decodificador reenvia o comando enquanto ela for diferente da pedida. O mesmo mecanismo cobre o comando perdido e o ESP32 que reiniciou e voltou ao padrão.

A especificação completa está em [`docs/PROTOCOLO.md`](../PROTOCOLO.md).

## Opções consideradas

- **JSON por linha:** legível no monitor serial, porém maior e sem proteção contra corrupção.
- **Retransmissão com buffer no ESP32:** mais estado dos dois lados, sem uso para os dados recuperados.
- **ACK para cada comando da Pi:** exigiria prazo de espera, reenvio e um tratamento separado para o reinício do ESP32, casos que o STATUS já cobre.

## Interface do decodificador (confirmada pela Isabela em 13/10)

O decodificador não abre a porta serial: quem abre é o leitor da Pi.
- Ele recebe bytes e devolve dicionários.
- Ele também devolve os bytes que o leitor deve escrever na porta: comandos de taxa (RF29) e as perguntas de horário (ADR-0004).
- Na amostra OBD, um PID que o carro não suporta fica ausente, nunca `null`.
- Descartar valores velhos pelo `age_ms` é responsabilidade do leitor.
