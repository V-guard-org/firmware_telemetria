# Ferramenta: PlatformIO + ESP-IDF + testes no notebook (ticket #6)

Testado em 13–14/10/2026 no notebook de desenvolvimento: Ubuntu 26.04.1 LTS, kernel 7.0.0-38, g++ 15.2.0. O projeto de prova está em [`tools/toolchain-probe/`](../../tools/toolchain-probe/). Ele é descartável e não é o firmware.

## Pergunta

O toolchain da fase 1 (ADR-0005) funciona neste notebook sem sudo? Concretamente:

1. Dá para instalar o PlatformIO de forma isolada, no usuário?
2. Um mesmo projeto compila os dois apps do ESP32 (o firmware e o **Adaptador USB-CAN**) e também a biblioteca portátil, testada no notebook com Unity?
3. O CAN virtual (`vcan`) e a UART virtual (par de pseudo-terminais) estão disponíveis? O que precisa de sudo?
4. Existe algo que mude o plano?

## Resposta curta

- **Sim, funciona.** O PlatformIO Core 6.2.0 foi instalado com `uv`, sem sudo. Os três envs passaram: `vguard` (33 s do zero), `helper` (34 s) e `native` (4 testes Unity, menos de 1 s).
- **Dois apps cabem no mesmo projeto, mas não com `build_src_filter`.** Com ESP-IDF, o PlatformIO ignora essa opção e avisa disso. O que funciona é passar a pasta do app ao CMake por env, com `board_build.cmake_extra_args`. A pasta `lib/` é compartilhada sem esforço.
- **Muda o plano:** o PlatformIO atual (`espressif32` 7.1.3) traz o **ESP-IDF 6.1.0**. Nessa versão, `driver/twai.h` (o driver TWAI antigo) está obsoleto. O build falha logo de cara, porque o aviso de obsolescência vira erro. Ele volta a compilar com `CONFIG_TWAI_SUPPRESS_DEPRECATE_WARN=y`. O driver novo (`esp_twai.h`) também compila, mas a recepção dele é por callback em ISR. Recomendo escrever o CAN do ESP32 já no driver novo.
- **Outros pontos para a camada portátil:** o ESP-IDF 6.1 compila C++ com `-std=gnu++26`, sem exceções e sem RTTI. O env `native` aceita C++17 sem problemas. Configurei o `native` para também rejeitar exceções e RTTI.
- **O `vcan0` só pode ser criado com sudo.** O `sudo -n` pede senha, e os namespaces de usuário estão bloqueados pelo AppArmor. Depois que a interface existe, abrir o socket CAN não exige root. O `socat` já está instalado. O `os.openpty()` funciona. Falta instalar o can-utils (com apt).

## O que foi instalado

Tudo fica no usuário. Nada usou sudo, e o Python do sistema não foi alterado.

```sh
# 1ª tentativa: funcionou, mas o venv ficou preso ao Python "latest" do mise (veja abaixo)
uv tool install platformio

# Instalação definitiva: Python próprio, gerenciado pelo uv, fora do mise
uv tool install --force --managed-python --python 3.14 platformio
rm -rf ~/.platformio/penv/.espidf-6.1.0   # o venv do IDF é recriado no próximo build

# O primeiro build baixa a plataforma, o toolchain e o ESP-IDF, e cria o venv Python do IDF
cd tools/toolchain-probe
pio run -e vguard
```

**Por que reinstalar:** na 1ª instalação, o `uv` usou o Python 3.14.6 do mise. O venv apontava para `~/.local/share/mise/installs/python/latest/bin/python`. Esse link muda quando o mise atualiza o Python. Numa troca para o 3.15, o `pio` pararia de funcionar, porque os pacotes estão em `lib/python3.14`. O venv do ESP-IDF (`~/.platformio/penv/.espidf-6.1.0`) herdou o mesmo link. O Python 3.14 em si funcionou bem, porque todas as dependências têm wheel `cp314`. O PlatformIO exige Python 3.9 ou mais novo ([requisitos][pio-req]), e o ESP-IDF 6.1 exige 3.10 ou mais novo (`OLDEST_PYTHON_SUPPORTED = (3, 10)` em `tools/python_version_checker.py`).

| Componente | Versão |
|---|---|
| uv (já existia, snap) | 0.12.21 |
| PlatformIO Core | 6.2.0 |
| Python da ferramenta | CPython 3.14.7, gerenciado pelo uv (`~/.local/share/uv/python/`) |
| Plataforma `espressif32` | 7.1.3 |
| `framework-espidf` | 4.60100.0, ou seja, **ESP-IDF 6.1.0** |
| `toolchain-xtensa-esp-elf` | 15.2.0+20251204 (GCC 15.2) |
| `tool-cmake` / `tool-ninja` | 3.30.2 / 1.7.1 |
| `tool-esptoolpy` | esptool 4.11.0 |
| Venv Python do IDF | idf-component-manager 3.1.2, esp-idf-kconfig 3.9.0, cryptography 46.0.7 |
| Plataforma `native` / Unity | 1.2.1 / 2.6.1 |
| Compilador do notebook | g++ 15.2.0 (padrão gnu++17) |

**Espaço em disco:** `~/.platformio` ocupa 2,8 GB. O maior item é o toolchain Xtensa (1,3 GB). Depois vêm o ESP-IDF (555 MB) e `.cache` (520 MB), que guarda os arquivos baixados. Em seguida vêm `tool-cmake` (116 MB), os dois GDB (94 MB + 93 MB; o RISC-V não serve para o ESP32 clássico) e o venv do IDF (69 MB). O venv do PlatformIO soma mais 21 MB, e o Python do uv mais 114 MB. Cada env do ESP32 gera cerca de 146 MB em `.pio/build/<env>`.

## Resultados

| Env | Comando | Resultado | Tempo |
|---|---|---|---|
| `vguard` | `pio run -e vguard`, 1ª vez (nada instalado) | **Falhou.** O `#warning` do driver TWAI antigo virou erro (`-Werror=cpp`) | 2 min 02 s, quase tudo download e venv do IDF |
| `vguard` | `pio run -e vguard`, do zero (pacotes já baixados) | OK. RAM 13 644 B (4,2 %), flash 154 265 B (14,7 % da partição de 1 MB) | 33 s (39 s quando recria o venv do IDF) |
| `vguard` | build incremental (um `.cpp` alterado) | OK | 2,7 s |
| `helper` | `pio run -e helper`, do zero | OK. RAM 14 128 B (4,3 %), flash 171 757 B (16,4 %) | 34 s |
| `native` | `pio test -e native`, 1ª vez (baixa `native` e Unity) | 4/4 PASSED | 4,2 s |
| `native` | `pio test -e native`, de novo | 4/4 PASSED | 0,8 s |
| `native` | `pio run -e native` (executável Linux) | OK. Thread, `openpty()` e socket CAN funcionam | 0,9 s |

O notebook tem 8 núcleos. Os tempos "do zero" recompilam o ESP-IDF inteiro (cerca de 1000 arquivos).

**O que cada app prova:**
- **`vguard`** inclui `driver/twai.h`, instala o driver, manda uma **Requisição OBD** (`7DF#02010C…`) e cria uma tarefa FreeRTOS. Também usa a biblioteca de `lib/`.
- **`helper`** usa o driver TWAI novo (`esp_twai.h` + `esp_twai_onchip.h`). A recepção é um callback em ISR que põe o **Quadro CAN** numa fila do FreeRTOS, e uma tarefa lê dessa fila. É o esboço do **Adaptador USB-CAN**.
- **`native`** testa `lib/crc16` (CRC-16/CCITT-FALSE, tabela `constexpr`) com Unity. Também gera um executável Linux com `std::thread`, `openpty()` e um socket SocketCAN, ou seja, as três peças da camada de plataforma da ADR-0005.

**C++ nos dois lados:**
- O ESP-IDF 6.1 compila C++ com `-std=gnu++26`. Confirmei isso no `compile_commands.json` e na [documentação de C++][idf-cpp]. Exceções e RTTI vêm **desligadas**: `CONFIG_COMPILER_CXX_EXCEPTIONS` e `CONFIG_COMPILER_CXX_RTTI` não estão ativos.
- O `native` compilou a mesma biblioteca com `-std=gnu++17 -Wall -Wextra -Werror` e também com `gnu++26`.
- Para que o notebook recuse o que o ESP32 recusaria, o env `native` acrescenta `-fno-exceptions -fno-rtti` **só em CXXFLAGS**, pelo script `native_cxx.py`. Se esses flags ficarem em `build_flags`, o `-fno-rtti` também vai para os `.c` do Unity e, com `-Werror`, o build quebra (`'-fno-rtti' is valid for C++/D/ObjC++ but not for C`). Conferi que, com esses flags, um `throw` dá erro de compilação no notebook.

## Dois apps no mesmo projeto

**`build_src_filter` não funciona com `framework = espidf`.** O builder da plataforma avisa e segue em frente:

```
Warning: the 'src_filter' option cannot be used with ESP-IDF. Select source files to build in the project CMakeLists.txt file.
```

No 1º build, o env `vguard` compilou tanto `src/vguard/main.cpp` quanto `src/helper/main.cpp`. Haveria dois `app_main` na hora de ligar. A documentação do PlatformIO lista essa limitação: "The `src_filter` option cannot be used" ([ESP-IDF no PlatformIO][pio-espidf], seção *Limitations*).

**Por quê:** com ESP-IDF, o PlatformIO roda o CMake do próprio IDF. A pasta `src/` vira um componente do IDF, registrado em `src/CMakeLists.txt`. O PlatformIO lê a lista de fontes do modelo de código que o CMake gera, então o filtro do SCons não tem efeito.

**`src_dir` por env também não existe.** Ele só vale na seção `[platformio]`, ou então pela variável de ambiente `PLATFORMIO_SRC_DIR`, que vale para a invocação inteira ([src_dir][pio-srcdir]).

**O que funciona (testado):** cada env passa a pasta do seu app ao CMake, e `src/CMakeLists.txt` só pega as fontes dessa pasta.

```ini
[env:vguard]
extends = esp32_base
board_build.cmake_extra_args = -DPROBE_APP=vguard

[env:helper]
extends = esp32_base
board_build.cmake_extra_args = -DPROBE_APP=helper
```

```cmake
# src/CMakeLists.txt
if(NOT DEFINED PROBE_APP)
    set(PROBE_APP vguard)
endif()
file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_LIST_DIR}/${PROBE_APP}/*.c ${CMAKE_CURRENT_LIST_DIR}/${PROBE_APP}/*.cpp)
idf_component_register(SRCS ${app_sources})
```

- Cada env tem a sua pasta de build e o seu `sdkconfig.<env>`.
- A pasta **`lib/` é compartilhada sem configuração extra.** O Library Dependency Finder do PlatformIO compilou `lib/crc16` nos dois envs do ESP32 e no `native`.
- No env `native` (que não é ESP-IDF), o `build_src_filter = +<native/>` funciona normalmente.

O projeto fica com quatro alvos: `vguard`, `helper`, o executável `native` e os testes. A alternativa de ter dois projetos compartilhando `lib/` (com `lib_extra_dirs`) não foi necessária e fica só como plano B.

**Fontes sobre `cmake_extra_args`:** a página da [plataforma Espressif 32][pio-esp32] cita `board_build.cmake_extra_args` só num exemplo, o de juntar vários `SDKCONFIG_DEFAULTS`. O comportamento foi confirmado no código do builder: `builder/frameworks/espidf.py` repassa `build.cmake_extra_args` ao CMake.

## CAN e UART virtuais

| Verificação | Resultado |
|---|---|
| `sudo -n true` | `sudo: interactive authentication is required` (exit 1). O usuário está no grupo `sudo`, mas precisa digitar a senha |
| Módulo `vcan` | Existe (`/lib/modules/7.0.0-38-generic/kernel/drivers/net/can/vcan.ko.zst`), mas não estava carregado |
| `ip link add dev vcan0 type vcan`, sem sudo | `RTNETLINK answers: Operation not permitted` |
| Namespace sem privilégio (`unshare -rn`) | Bloqueado: `kernel.apparmor_restrict_unprivileged_userns = 1`, erro `write failed /proc/self/uid_map: Operation not permitted` |
| Socket `socket(PF_CAN, SOCK_RAW, CAN_RAW)` sem root | **Funciona.** O kernel carregou `can` e `can_raw` sozinho. Isso foi efeito colateral do teste e some no próximo boot. O `bind` falha só porque o `vcan0` não existe |
| can-utils | Não instalado. Versão disponível no apt: 2023.03-1build1 |
| `socat` | 1.8.1.1 instalado. O par `pty,raw,echo=0,link=…` foi criado e os bytes atravessaram |
| `os.openpty()` | Funciona (Python 3.14.6 e 3.14.4). Em modo raw, os 256 valores de byte passaram intactos nos dois sentidos |
| `openpty()` em C++ (env `native`) | Funciona, sem `-lutil` |

**Conclusão:**
- Não há como criar o `vcan0` sem root. Os grupos `docker` e `lxd`, dos quais o usuário faz parte, dariam acesso equivalente ao de root, mas esse caminho não foi usado nem é recomendado.
- Depois que o `vcan0` existe, o firmware no notebook e a **ECU simulada** rodam como usuário comum.
- Para a UART virtual, `os.openpty()` serve quando um mesmo processo cria o par e entrega o caminho do escravo ao outro programa. O `socat` serve quando dois programas independentes precisam de dois caminhos fixos, por exemplo `/tmp/ttyESP` e `/tmp/ttyPI`.
- A [documentação do SocketCAN][kernel-can] mostra a criação com `ip link add dev vcan42 type vcan`.

## Passos manuais para o Pedro

Comandos com sudo, para rodar à mão. Nada do PlatformIO precisa de sudo.

**Fase 1 (notebook):**

- [ ] `sudo apt install can-utils`
- [ ] `sudo modprobe vcan`
- [ ] `sudo ip link add dev vcan0 type vcan`
- [ ] `sudo ip link set vcan0 up`
- [ ] Conferir com `ip -details link show vcan0`. A saída deve mostrar `vcan` e `UP`.
- [ ] Testar. Num terminal, rode `candump vcan0`. Noutro, rode `cansend vcan0 7DF#02010C5555555555`. O quadro deve aparecer no primeiro terminal.
- [ ] Opcional: rodar `tools/toolchain-probe/.pio/build/native/program vcan0` depois de `pio run -e native`. A saída deve ter `write(vcan0, …): ok`, e o quadro deve aparecer no `candump`.

O `vcan0` some quando o notebook reinicia. Depois de cada boot, repita `modprobe`, `ip link add` e `ip link set up`. Se quiser que o módulo carregue sozinho no boot, rode `echo vcan | sudo tee /etc/modules-load.d/vcan.conf` (a interface ainda precisa do `ip link`).

**Fase 2 (ESP32 pelo USB), quando chegar a hora:**

- [ ] `sudo usermod -aG dialout $USER`, e depois sair e entrar de novo na sessão. Hoje o usuário não está no grupo `dialout`, então o upload e o monitor serial falhariam. A outra opção são as [regras udev do PlatformIO][pio-udev].
- [ ] Nada a fazer quanto ao `brltty`: ele está instalado, mas as regras dele para CP210x (`10c4:ea60`) e CH340 (`1a86:7523`) estão comentadas em `85-brltty.rules`. Se a `/dev/ttyUSB0` sumir ao conectar a placa, olhe `journalctl -k`.

## Riscos e incertezas

1. **O driver TWAI antigo está obsoleto no ESP-IDF 6.x.** O [guia de migração do 6.0][idf-mig6] diz: "The legacy driver is not recommended to be used anymore". O mesmo guia removeu de vez, no 6.0, os drivers antigos de ADC, MCPWM, timer, I2S, PCNT, RMT, DAC e sigma-delta. O I2C antigo já tem remoção marcada para o 7.0. Para o TWAI ainda não há data, mas o padrão sugere que ele também sai.
2. **O driver novo muda o formato do código.** A recepção só acontece no callback `on_rx_done`, chamado em contexto de ISR, com `twai_node_receive_from_isr()` ([TWAI no IDF 6.1][idf-twai]). A camada de plataforma do ESP32 precisa de uma fila ISR → tarefa. A maioria dos exemplos e tutoriais na internet usa a API antiga (`twai_driver_install`, `twai_receive`). Ponto positivo: o driver novo tem a flag `enable_listen_only`, que interessa à pesquisa do transceptor silencioso.
3. **A plataforma 7.x e o ESP-IDF 6.1 são recentes.** A 7.0.0 saiu em 04/2026 e a 7.1.3 em 09/2026. Há poucos relatos da comunidade. Se a equipe preferir a API antiga sem aviso, a `espressif32@6.13.0` usa o ESP-IDF 5.5.3 com GCC 14.2 (conforme o [platform.json da v6.13.0][pio-6130]). Isso **não foi compilado aqui**, e baixaria mais uns 2 GB. Também não verifiquei se o ESP-IDF 5.5 avisa sobre o driver antigo.
4. **Nada foi gravado numa placa.** A prova é só de build. O TWAI em funcionamento fica para a fase 2.
5. **`board_build.cmake_extra_args` quase não tem documentação.** Ele só aparece como exemplo. Pode mudar numa versão futura da plataforma, por isso a versão fica fixada.
6. **Caminhos com espaço quebram o build do ESP-IDF.** O builder recusa com `Error: Detected a whitespace character in project paths.` Isso pode pegar quem usa Windows com espaço no nome do usuário.
7. **O padrão de C++ é diferente nos dois lados:** `gnu++26` no ESP32 e `gnu++17` no notebook. Código que compila em C++17 quase sempre compila em C++26, mas há pequenas diferenças desde o C++20 (literais `u8`, operações compostas em `volatile`).
8. **O `sdkconfig.<env>` guarda estado.** Acrescentar ou mudar uma opção em `sdkconfig.defaults` vale no próximo build. Remover uma opção **não** desfaz o valor: é preciso apagar o `sdkconfig.<env>` (testei com `CONFIG_LOG_DEFAULT_LEVEL_WARN`). O [PlatformIO][pio-esp32] trata o `sdkconfig.<env>` como artefato de build, por isso ele está no `.gitignore` da prova.
9. **Editar o `platformio.ini` apaga as pastas de build de todos os envs.** O próximo build de cada env do ESP32 leva de 30 a 35 s.
10. **Detalhes menores:**
    - O Unity também é baixado nos envs do ESP32, mas não é compilado.
    - O `.cache` (520 MB) pode ser limpo com `pio system prune`.
    - O `uv` deste notebook vem do snap. Nos outros notebooks da equipe a instalação não foi verificada; lá o caminho pode ser `pipx` ou o instalador oficial ([métodos][pio-install]).

## Recomendação para o ticket

1. **Fechar o #6 como feito.** O toolchain funciona, e os únicos passos com sudo são os da lista acima.
2. **Instalação padrão da equipe:** `uv tool install --managed-python --python 3.14 platformio`. Qualquer Python de 3.10 em diante serve. O importante é não amarrar o venv a um Python que muda sozinho.
3. **Fixar a plataforma no firmware:** `platform = platformio/espressif32@7.1.3`, que traz o ESP-IDF 6.1.0.
4. **Escrever o CAN do ESP32 no driver TWAI novo** (`esp_twai.h` / `esp_twai_onchip.h`), tanto no firmware quanto no Adaptador USB-CAN. Ainda não há código para migrar, e o driver antigo está a caminho da remoção. Usar `CONFIG_TWAI_SUPPRESS_DEPRECATE_WARN` só como quebra-galho. Vale registrar essa escolha como ADR-0006.
5. **Estrutura do repositório do firmware,** copiada da prova:
   - `src/vguard/`, `src/helper/` e `src/native/` para os apps;
   - `lib/` para a lógica portátil e `test/` para o Unity;
   - envs do ESP32 separados por `board_build.cmake_extra_args`;
   - env `native` separado por `build_src_filter`.
   Um projeto só basta.
6. **Env `native`:** `-std=gnu++17 -Wall -Wextra -Werror`, mais `-fno-exceptions -fno-rtti` só em CXXFLAGS (como em `native_cxx.py`). Assim o notebook recusa o que o ESP32 recusaria.
7. **Versionar `sdkconfig.defaults`** (com `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`; sem isso o build avisa que a flash é de 2 MB) e ignorar `sdkconfig.<env>`.
8. **UART virtual:** `socat` para ligar dois programas, ou o firmware no notebook cria o pty com `openpty()` e imprime o caminho. Não precisa de sudo.

<!-- Fontes -->
[pio-req]: https://docs.platformio.org/en/latest/core/installation/requirements.html
[pio-install]: https://docs.platformio.org/en/latest/core/installation/methods/index.html
[pio-espidf]: https://docs.platformio.org/en/latest/frameworks/espidf.html
[pio-esp32]: https://docs.platformio.org/en/latest/platforms/espressif32.html
[pio-srcdir]: https://docs.platformio.org/en/latest/projectconf/sections/platformio/options/directory/src_dir.html
[pio-udev]: https://docs.platformio.org/en/latest/core/installation/udev-rules.html
[pio-6130]: https://github.com/platformio/platform-espressif32/blob/v6.13.0/platform.json
[idf-twai]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/peripherals/twai.html
[idf-mig6]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/migration-guides/release-6.x/6.0/peripherals.html
[idf-cpp]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/cplusplus.html
[kernel-can]: https://docs.kernel.org/networking/can.html
