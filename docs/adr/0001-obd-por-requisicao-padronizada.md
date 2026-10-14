# OBD-II por requisição padronizada, e não só escuta

Cogitamos que o ESP32 apenas escutasse o barramento CAN, por segurança. A ideia foi descartada:
- o OBD-II funciona por pergunta e resposta;
- muitos carros deixam o conector mudo, por causa de um gateway;
- o tráfego espontâneo, quando existe, é proprietário e fica fora do escopo pelo AE04.

O firmware transmite somente requisições OBD padronizadas do modo 01, como permitem o AE02 e o RNF10. Ele escuta o barramento antes de transmitir, e toda transmissão passa por uma lista fechada no código.

## Consequências

- Os requisitos RF03–RF05 se mantêm.
- A segurança passa a depender da lista fechada, coberta por teste automático, e das demais salvaguardas do envio:
  - escutar antes de transmitir;
  - no máximo 20 requisições/s, com pausa quando o carro não responde;
  - velocidade do barramento fixa em 500 kbit/s com IDs de 11 bits, alterada só recompilando. O ESP32 nunca testa velocidades desconhecidas, ao contrário de um scanner. Opcionalmente, também de um transceptor com modo silencioso controlado pelo firmware.
