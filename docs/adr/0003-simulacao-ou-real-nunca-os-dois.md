# Simulação e fontes reais nunca convivem no projeto

A simulação existe só para desenvolver sem hardware. O ensaio no carro decide qual caminho fica:
- **se o real funcionar**, a simulação é apagada;
- **se não funcionar**, o projeto segue só com a simulação. Isso também cobre o risco R06 do Plano de Projeto.

Não há chave para alternar entre real e simulado. Por isso o código simulado do firmware precisa ficar isolado e ser removível inteiro sem mexer no resto.

O que roda fora do dispositivo fica: o simulador do notebook, a ECU simulada (programa no notebook), o build do firmware para o notebook (ADR-0005) e os testes automáticos. Só o que está no firmware do dispositivo é apagado.

## Opções consideradas

- **Real e simulado no mesmo firmware**, com chave de configuração e marca `sim` em cada amostra. Rejeitada porque o projeto entregue terá um único caminho.
- **Dois firmwares separados** (bancada e carro). Desnecessária pelo mesmo motivo.
