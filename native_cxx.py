# Faz o env native imitar os padrões do ESP-IDF em C++: sem exceções e sem RTTI.
# -fno-rtti só vale para C++; em build_flags ele também iria para os .c (Unity) e,
# com -Werror, quebraria o build. Por isso entra só em CXXFLAGS.
Import("env")  # noqa: F821 (injetado pelo SCons do PlatformIO)

env.Append(CXXFLAGS=["-fno-exceptions", "-fno-rtti"])  # noqa: F821

# pio test e pio run gravam no mesmo .pio/build/native/. O programa dos testes ganha
# outro nome, para não apagar o firmware nativo que o simulador inicia (program).
if "test" in env["BUILD_TYPE"]:  # noqa: F821
    env.Replace(PROGNAME="program_test")  # noqa: F821
