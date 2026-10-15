# A versão do firmware que sai no STATUS (PROTOCOLO.md §3.1): a do platformio.ini, seguida
# do commit, por exemplo 0.1.0-a58af37, e de -dirty se há mudanças sem commit.
# Entra só nas fontes de src/, para um commit novo não recompilar a lib/ inteira.
import subprocess

Import("env", "projenv")  # noqa: F821 (injetados pelo SCons do PlatformIO)


def _git(*args):
    return subprocess.run(["git", *args], cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
                          capture_output=True, text=True, check=True).stdout.strip()


version = env.GetProjectConfig().get("firmware", "version")  # noqa: F821
try:
    version += "-" + _git("rev-parse", "--short=7", "HEAD")
    if _git("status", "--porcelain", "--untracked-files=no"):
        version += "-dirty"
except (OSError, subprocess.CalledProcessError):
    pass

projenv.Append(CPPDEFINES=[("VGUARD_FIRMWARE_VERSION", env.StringifyMacro(version))])  # noqa: F821
