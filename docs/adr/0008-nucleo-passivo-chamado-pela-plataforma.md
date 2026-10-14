# O código portátil é um núcleo passivo, chamado pela camada de plataforma

O código portátil do firmware (OBD, IMU, protocolo, STATUS e presença da Pi) forma um núcleo que nunca espera, nunca cria tarefas e nunca pergunta a hora:
- a camada de plataforma chama o núcleo a cada evento (quadro CAN, bytes da Pi, aviso de log, prazo vencido), sempre com o instante do evento;
- o núcleo age chamando as portas (CAN, UART, IMU e sistema);
- ao final de cada chamada, o núcleo informa o seu próximo prazo, e a plataforma espera até ele ou até o próximo evento.

No ESP32, uma única tarefa lê uma fila de eventos. No notebook, uma thread faz `poll()` no `vcan0` e no pty. Nos testes, o núcleo é chamado direto, com um relógio falso.

A razão principal são os testes. Com o relógio falso saltando de um prazo para o seguinte, cada cenário do [`MAQUINAS-DE-ESTADO.md`](../MAQUINAS-DE-ESTADO.md) roda em milissegundos e sempre na mesma ordem, inclusive uma noite de 10 h. É também o formato do decodificador Python, que recebe bytes e devolve ações.

A especificação completa está em [`docs/PLATAFORMA.md`](../PLATAFORMA.md).

## Opções consideradas

- **Uma tarefa por fonte** (o `PLANO.md` §3.1): IMU, OBD, envio, recepção e supervisor, cada uma esperando dentro do código portátil e trocando dados por filas e mutexes. Os testes precisariam esperar de verdade ou falsificar o sono de várias threads, e a ordem entre elas mudaria a cada execução.
- **Um tique fixo de 1 ms**, em que o núcleo confere todos os prazos a cada chamada. Uma noite de 10 h no teste daria 36 milhões de tiques.
- **Uma porta de relógio** que o núcleo consulta quando quiser. O instante passado em cada chamada já basta, e assim o teste controla todos os instantes.

## Consequências

- Nenhuma operação do núcleo pode demorar. Hoje a mais lenta é a leitura da IMU, com menos de 1 ms. Algo lento no futuro, como gravar na flash, iria para a plataforma.
- O tique do FreeRTOS passa a 1000 Hz. Com o padrão de 100 Hz, uma espera só acorda a cada 10 ms, e as leituras da IMU, a cada 10 ms, ficariam irregulares.
- Os callbacks do TWAI e o gancho de log do ESP-IDF só põem eventos na fila. Nenhum adaptador chama o núcleo de dentro de uma chamada do próprio núcleo.
- No ESP32 ficam duas tarefas nossas, a do núcleo e a leitora da UART. São elas as "tarefas no watchdog" do `MAQUINAS-DE-ESTADO.md` §1.
