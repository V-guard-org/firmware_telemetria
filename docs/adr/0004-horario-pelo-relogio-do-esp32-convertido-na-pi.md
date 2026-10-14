# O ESP32 carimba as amostras com o próprio relógio, e a Pi converte para a hora real

A meta é intervalo exato entre amostras e erro absoluto de até 10 ms em relação ao horário GPS.

- O ESP32 marca cada amostra com os microssegundos desde que ligou.
- A Pi descobre a diferença para o relógio dela, que é preso ao GPS por chrony com PPS, por pergunta e resposta, descontando o tempo de ida e volta.
- A conversão fica no decodificador que a equipe do firmware entrega.

## Opções consideradas

- **A Pi carimbar na chegada:** intervalos irregulares por causa do atraso variável do USB.
- **PPS ligado também no ESP32:** mais fio e mais lógica, sem necessidade para uma meta de 10 ms.
