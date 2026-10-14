# As fórmulas do OBD ficam no decodificador Python, e o ESP32 envia os bytes crus

Na amostra OBD, o ESP32 envia os bytes que o carro respondeu para cada PID, sem converter. O decodificador Python da Pi aplica as fórmulas, como `(256A + B)/4` para o RPM.

A razão principal é que uma fórmula errada se corrige atualizando o Python na Pi. Regravar o ESP32 dentro do carro é difícil, porque na fase 4 não há cabo USB. Além disso, a ECU simulada também é Python e usa a mesma tabela de fórmulas, testada nos dois sentidos, e o firmware fica menor.

## Opções consideradas

- **Fórmulas no ESP32, com o valor físico no cabo** (a FW-05 do rascunho antigo): as fórmulas ficariam em C++, separadas da ECU simulada, e cada correção exigiria regravar o firmware.

## Consequências

- O ESP32 não conhece os valores físicos do OBD. Uma detecção futura feita no próprio ESP32 com dados do OBD precisaria trazer as fórmulas para o C++. Hoje nenhuma está no escopo: as detecções previstas (RF34, RF35) usam a IMU.
- Cada valor no cabo leva o próprio tamanho em bytes, para que o decodificador consiga pular um PID que ele não conhece.
