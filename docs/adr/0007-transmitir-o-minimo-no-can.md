# O ESP32 transmite o mínimo no CAN do carro

Além da lista fechada (ADR-0001), o firmware reduz ao mínimo o que transmite no barramento do carro:
- **Tentativa única por quadro.** A retransmissão automática do controlador fica desligada. Sem ACK, ela repetiria o mesmo quadro para sempre, porque no *error passive* o contador de erros para de subir. Num carro estacionado, isso inundaria um barramento adormecido e poderia acordar módulos.
- **Só com a Pi presente.** Sem um PING nos últimos 3 s, o ESP32 não pergunta nada ao carro, porque ninguém usaria as respostas (D11). A IMU continua.
- **Transceptor silencioso fora da leitura.** O pino de modo silencioso só fica liberado durante a descoberta e a leitura. Com o carro sem resposta, cada nova pergunta repete a escuta de 1 s antes de falar.
- **Um PID por requisição.** Respostas em vários quadros exigiriam transmitir controle de fluxo, que não é uma requisição.
- **Bloqueio.** Na 3ª falha de barramento do mesmo boot, ou em qualquer violação da lista fechada, o ESP32 fica silencioso até reiniciar.

O comportamento completo está em [`docs/MAQUINAS-DE-ESTADO.md`](../MAQUINAS-DE-ESTADO.md).

## Opções consideradas

- **Retransmissão automática**, o padrão do controlador: mais simples, mas inunda o barramento quando ninguém responde.
- **Perguntar ao carro sempre**, com ou sem a Pi: dados prontos alguns segundos antes, em troca de cerca de 16 requisições/s sem uso sempre que a Pi está fora.
- **Transceptor sempre liberado depois da 1ª escuta**, confiando só na lista fechada: deixaria o barramento sem a proteção do hardware nas horas com o carro desligado.

## Consequências

- Uma requisição que perde a disputa com o tráfego do carro não é repetida pelo hardware. O firmware a tenta de novo no próximo horário de envio.
- Os valores OBD chegam cerca de 1,2 s depois de a Pi aparecer, e até cerca de 11 s depois da partida, quando o carro ficou muito tempo desligado.
