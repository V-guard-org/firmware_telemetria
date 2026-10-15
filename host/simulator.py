"""The PC simulator: plays the car and the Pi for the firmware (docs/SIMULADOR.md).

    python3 host/simulator.py --firmware .pio/build/native/program --sem-tela
                              [--roteiro host/roteiros/ARQUIVO.txt] [--duracao 30min] [--porta CAMINHO]

One process, one thread: select() on the firmware's port with a 10 ms tick (§2). The port is
opened once, and the IMU_SIM and the Decoder share it, as it has a single reader. Headless
only for now: the screen comes with #27, and the ECU simulada with #25.
"""

import argparse
import collections
import json
import logging
import os
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import tty

from car_model import CarModel
from esp32_decoder import PING, Decoder, cobs_decode, encode_message

DEFAULT_PORT = "/tmp/vguard-esp32"
TICKS_PER_S = 100               # the IMU_SIM goes once per tick (PROTOCOLO.md §4.3)
TICK_S = 1 / TICKS_PER_S
MAX_LAG_S = 1.0                 # further behind than this (a stalled notebook), jump instead of catching up
END_AFTER_LAST_S = 5            # a roteiro with no fim ends this long after its last action (§6)
FIRMWARE_START_S = 2.0          # for the native firmware to create its link
FIRMWARE_STOP_S = 2.0           # before it gets a SIGKILL

IMU_SIM = 0x83
_IMU_SIM = struct.Struct("<6hB")    # ax ay az, gx gy gz, falha (PROTOCOLO.md §4.3)
_IMU_SCALES = (100, 100, 100, 10, 10, 10)   # 0.01 m/s² and 0.1 °/s

Action = collections.namedtuple("Action", "at_s line text command")
Roteiro = collections.namedtuple("Roteiro", "actions end_s")
Command = collections.namedtuple("Command", "name args")

_TIME_AND_COMMAND = re.compile(r"(\d+):([0-5]\d)\s+(.+)")

# The roteiro's names for the states in the STATUS, and the decoder's (TESTES.md §4)
OBD_STATES = {"esperando-pi": "waiting_pi", "escutando": "listening", "descobrindo": "discovering",
              "lendo": "reading", "sem-resposta": "no_response", "velocidade-errada": "wrong_bitrate",
              "linha-presa": "line_stuck", "falha-can": "can_failure", "bloqueado": "locked"}
IMU_STATES = {"ok": "ok", "falha": "failed"}
_ROTEIRO_NAMES = {english: name for states in (OBD_STATES, IMU_STATES) for name, english in states.items()}
SOURCES = ("imu", "obd")


def _percent(name, text, low=0):
    value = int(text)
    if not low <= value <= 100:
        raise ValueError(f"{name} {text}%: fora da faixa de {low}% a 100%")
    return value / 100


def _one_of(name, value, accepted):
    if value not in accepted:
        raise ValueError(f'{name} {value}: esperava {", ".join(accepted)}')
    return value


# Each command as the roteiro and the screen write it (SIMULADOR.md §6), and what it means
_COMMANDS = (
    (r"ignição (liga|desliga)", lambda on: Command("ignition", (on == "liga",))),
    (r"partida", lambda: Command("start", ())),
    (r"(acelerador|freio) ([+-]?\d+)%", lambda pedal, value: Command(
        "throttle" if pedal == "acelerador" else "brake", (_percent(pedal, value),))),
    (r"volante ([+-]?\d+)%", lambda value: Command("steering", (_percent("volante", value, -100),))),
    (r"solta tudo", lambda: Command("release", ())),
    (r"pi ping (liga|desliga)", lambda on: Command("ping", (on == "liga",))),
    (r"reinicia esp32", lambda: Command("restart", ())),
    (r"confere obd (\S+)", lambda state: Command(
        "check_obd", (_one_of("confere obd", state, OBD_STATES),))),
    (r"confere imu (\S+)", lambda state: Command(
        "check_imu", (_one_of("confere imu", state, IMU_STATES),))),
    (r"confere boots (\d+)", lambda count: Command("check_boots", (int(count),))),
    (r"confere vazias (\S+) (\d+)", lambda source, count: Command(
        "check_empty", (_one_of("confere vazias", source, SOURCES), int(count)))),
    (r"fim", lambda: Command("end", ())),
)


class RoteiroError(ValueError):
    """A roteiro that does not run: the message says which line."""


def parse_command(text):
    """One command, as in a roteiro line or a click on the screen. ValueError if unknown."""
    words = " ".join(text.split())
    for pattern, build in _COMMANDS:
        match = re.fullmatch(pattern, words)
        if match:
            return build(*match.groups())
    raise ValueError(f'comando desconhecido: "{words}"')


def parse_roteiro(text):
    """The roteiro's actions in order, and when it ends (§6)."""
    actions = []
    end_s = None
    for line, raw in enumerate(text.splitlines(), start=1):
        content = raw.split("#", 1)[0].strip()
        if not content:
            continue
        match = _TIME_AND_COMMAND.fullmatch(content)
        if not match:
            raise RoteiroError(f'linha {line}: esperava o tempo em m:ss e um comando: "{content}"')
        at_s = int(match[1]) * 60 + int(match[2])
        try:
            command = parse_command(match[3])
        except ValueError as error:
            raise RoteiroError(f"linha {line}: {error}") from None
        if end_s is not None:
            raise RoteiroError(f"linha {line}: nada pode vir depois do fim")
        if actions and at_s < actions[-1].at_s:
            raise RoteiroError(f"linha {line}: o tempo {match[1]}:{match[2]} vem antes do da linha "
                              f"{actions[-1].line}")
        if command.name == "end":
            end_s = at_s
        else:
            actions.append(Action(at_s, line, " ".join(match[3].split()), command))
    if not actions:
        raise RoteiroError("o roteiro não tem nenhuma ação")
    if end_s is None:
        end_s = actions[-1].at_s + END_AFTER_LAST_S
    return Roteiro(actions, end_s)


class SimulatorError(Exception):
    """The simulator cannot go on: the port or the firmware is gone."""


class Simulator:
    """The car and the Pi on the firmware's port, a 10 ms tick at a time, from one thread.

    With a firmware path, it starts the native firmware as its child (phase 1), so that
    "reinicia esp32" can stop it and start it again.
    """

    def __init__(self, port=DEFAULT_PORT, firmware=None, roteiro=None, duration_s=None):
        self.car = CarModel()
        self.decoder = Decoder()
        self.samples = dict.fromkeys(SOURCES, 0)
        self.empty = dict.fromkeys(SOURCES, 0)     # the {} of each source since the start
        self.checks_passed = 0
        self.failed_checks = []
        self.error = None
        self._port = port
        self._firmware = firmware
        self._child = None
        self._fd = None
        self._actions = list(roteiro.actions) if roteiro else []
        ends = [end_s for end_s in (roteiro and roteiro.end_s, duration_s) if end_s is not None]
        self._end_tick = round(min(ends) * TICKS_PER_S) if ends else None
        self._tick_count = 0
        self._pinging = True
        self._imu_seq = 0
        self._ended = False
        self._stopping = False
        self._last_status = None

    def run(self):
        """Until the roteiro or the duration ends, or stop(). True if every confere passed."""
        handler = _RecordHandler(self._record)
        for name in ("esp32", "esp32_decoder"):
            logging.getLogger(name).addHandler(handler)
        try:
            if self._firmware:
                self._start_firmware()
            self._open_port()
            self._loop()
            if not self._ended and self._end_tick is not None:
                # A phase gate cut short must not pass: its last confere never ran.
                raise SimulatorError("interrompido antes do fim do roteiro ou da --duracao")
            if not self._ended:
                self._record("interrompido")
        except SimulatorError as error:
            self.error = str(error)
            self._record(f"ERRO: {error}")
        finally:
            self._close_port()
            self._stop_firmware()
            for name in ("esp32", "esp32_decoder"):
                logging.getLogger(name).removeHandler(handler)
        return not self.failed_checks and self.error is None

    def stop(self):
        """Ends the run within a tick, as a Ctrl+C does. Safe from a signal handler."""
        self._stopping = True

    def execute(self, command, line=None, text=None):
        """One command, from a roteiro line or, with #27, a click on the screen."""
        name, args = command
        where = f"linha {line}: {text}" if line else text
        if name.startswith("check_"):
            expected, seen = self._seen(name, args)
            if seen == expected:
                self.checks_passed += 1
                self._record(f"{where}: certo")
            else:
                self.failed_checks.append({"line": line, "command": text, "expected": expected,
                                           "seen": seen, "at_s": self._tick_count / TICKS_PER_S})
                self._record(f"{where}: FALHOU, esperado {expected}, visto {seen}")
            return
        self._record(where)
        if name == "ignition":
            self.car.ignition = args[0]
        elif name == "start":
            self.car.start()
        elif name == "throttle":
            self.car.throttle = args[0]
        elif name == "brake":
            self.car.brake = args[0]
        elif name == "steering":
            self.car.steering = args[0]
        elif name == "release":
            self.car.throttle = self.car.brake = self.car.steering = 0.0
        elif name == "ping":
            self._pinging = args[0]
        elif name == "restart":
            self._restart_firmware()
        elif name == "end":
            self._ended = True

    def summary(self):
        """What the run ends with (SIMULADOR.md §7); the ensaio's criteria come with #28."""
        stats = self.decoder.stats()
        esp32 = stats.pop("esp32")
        return {
            "duration_s": self._tick_count / TICKS_PER_S,
            "samples": dict(self.samples),
            "empty": dict(self.empty),
            "boots": stats["counters"]["boots"],
            "checks": {"passed": self.checks_passed, "failed": list(self.failed_checks)},
            "error": self.error,
            "decoder": stats,
            "esp32": esp32,
        }

    def _loop(self):
        start = time.monotonic()
        while not self._ended and not self._stopping:
            due = start + self._tick_count * TICK_S
            ready, _, _ = select.select([self._fd], [], [], max(0.0, due - time.monotonic()))
            if ready:
                # Fed as soon as read: the PONG's arrival is the read, not the next tick.
                self._exchange(self._read())
            now = time.monotonic()
            if now >= due:
                if now - due > MAX_LAG_S:
                    self._tick_count = int((now - start) * TICKS_PER_S)
                self._tick()
                if not self._ended:
                    self._tick_count += 1

    def _tick(self):
        """Actions, car, IMU_SIM, feed() and data_to_send(), in the order of SIMULADOR.md §2."""
        if self._child is not None and self._child.poll() is not None:
            raise SimulatorError(self._why_closed())
        while self._actions and self._actions[0].at_s * TICKS_PER_S <= self._tick_count:
            action = self._actions.pop(0)
            self.execute(action.command, action.line, action.text)
        if self._end_tick is not None and self._tick_count >= self._end_tick:
            self._ended = True
        if self._ended:
            return
        self.car.step(TICK_S)
        reading = [max(-32767, min(32767, round(value * scale)))
                   for value, scale in zip(self.car.imu, _IMU_SCALES)]
        self._write(encode_message(IMU_SIM, 0, self._imu_seq, _IMU_SIM.pack(*reading, 0)))
        self._imu_seq = (self._imu_seq + 1) & 0xFFFF
        self._exchange(b"")

    def _exchange(self, data):
        """The Pi's part: the bytes read go to feed(), and what data_to_send() returns goes out."""
        for source, _ts, values in self.decoder.feed(data):
            if values:
                self.samples[source] += 1
            else:
                self.empty[source] += 1
                self._record(f"amostra vazia: {source}")
        self._note_status()
        out = self.decoder.data_to_send()
        self._write(out if self._pinging else _without_pings(out))

    def _note_status(self):
        stats = self.decoder.stats()
        esp32 = stats["esp32"]
        if esp32 is None:
            return
        status = (stats["boot"], esp32["obd_state"], esp32["imu_state"])
        if status != self._last_status:
            self._last_status = status
            self._record(f"STATUS: boot 0x{status[0]:04X}, obd {_roteiro_name(status[1])}, "
                         f"imu {_roteiro_name(status[2])}")

    def _seen(self, name, args):
        """A confere's expected and seen values, from what the simulator knows now (TESTES.md §4)."""
        stats = self.decoder.stats()
        if name == "check_boots":
            return str(args[0]), str(stats["counters"]["boots"])
        if name == "check_empty":
            source, count = args
            return str(count), str(self.empty[source])
        esp32 = stats["esp32"]
        if esp32 is None:
            return args[0], "nenhum STATUS"
        return args[0], _roteiro_name(esp32["obd_state" if name == "check_obd" else "imu_state"])

    def _start_firmware(self):
        if os.path.islink(self._port):
            os.unlink(self._port)       # left by a firmware that did not stop cleanly
        # A session of its own: a Ctrl+C stops the simulator, and the simulator stops the firmware.
        self._child = subprocess.Popen([self._firmware, "--path", self._port], start_new_session=True)
        deadline = time.monotonic() + FIRMWARE_START_S
        while not os.path.exists(self._port):
            if self._child.poll() is not None or time.monotonic() > deadline:
                raise SimulatorError(f"o firmware não criou {self._port}")
            time.sleep(0.01)

    def _stop_firmware(self):
        if self._child is None or self._child.poll() is not None:
            return
        self._child.terminate()
        try:
            self._child.wait(FIRMWARE_STOP_S)
        except subprocess.TimeoutExpired:
            self._child.kill()
            self._child.wait()

    def _restart_firmware(self):
        """Phase 1: stop the native firmware and start it again, on a new pty (SIMULADOR.md §4)."""
        if self._firmware is None:
            self._record("reinicia esp32 precisa de --firmware; o pulso no RTS da fase 2 ainda não existe")
            return
        self._close_port()
        self._stop_firmware()
        self._start_firmware()
        self._open_port()

    def _open_port(self):
        """Like the reader in DECODIFICADOR.md §2.2, but non-blocking: a full port loses bytes, never waits."""
        try:
            fd = os.open(self._port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as error:
            raise SimulatorError(f"não abriu a porta {self._port}: {error.strerror}") from None
        tty.setraw(fd, termios.TCSANOW)     # TCSANOW, not TCSAFLUSH: keeps the boot's STATUS
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        attrs[2] &= ~(termios.CSTOPB | termios.CRTSCTS)
        attrs[4] = attrs[5] = termios.B460800
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        self._fd = fd

    def _close_port(self):
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def _read(self):
        try:
            data = os.read(self._fd, 4096)
        except BlockingIOError:
            return b""
        except OSError:
            data = b""                  # EIO: the other side of the pty closed
        if not data:
            raise SimulatorError(self._why_closed())
        return data

    def _why_closed(self):
        if self._child is not None:
            try:
                return f"o firmware terminou sozinho (código {self._child.wait(FIRMWARE_STOP_S)})"
            except subprocess.TimeoutExpired:
                pass
        return f"a porta {self._port} fechou"

    def _write(self, data):
        # A full port loses the bytes, as on the wire, and a partial write cuts a message, which the
        # firmware drops with the next. Neither happens while the firmware reads: 100 IMU_SIM a
        # second are about 2 KB, and the pty holds about 17 KB.
        try:
            os.write(self._fd, data)
        except BlockingIOError:
            pass
        except OSError:
            raise SimulatorError(self._why_closed()) from None

    def _record(self, text):
        """One line of the registro, at the simulator's time (SIMULADOR.md §5)."""
        minutes, seconds = divmod(self._tick_count / TICKS_PER_S, 60)
        print(f"{int(minutes)}:{seconds:05.2f} {text}", flush=True)


class _RecordHandler(logging.Handler):
    """The ESP32's LOGs and the decoder's own errors go to the registro."""

    _NAMES = {"esp32": "LOG do ESP32", "esp32_decoder": "decodificador"}

    def __init__(self, record):
        super().__init__()
        self._to_record = record

    def emit(self, entry):
        self._to_record(f"{self._NAMES.get(entry.name, entry.name)}: {entry.getMessage()}")


def _without_pings(data):
    """data_to_send() less its PINGs: with "pi ping desliga" the Pi still reads and sends the rest."""
    frames = data.split(b"\x00")[:-1]
    return b"".join(frame + b"\x00" for frame in frames if cobs_decode(frame)[0] != PING)


def _roteiro_name(state):
    """The roteiro's name for a state the decoder names in English: waiting_pi is esperando-pi."""
    return _ROTEIRO_NAMES.get(state, state)


def parse_duration(text):
    """--duracao, as 2h, 30min, 90s or 1h30min."""
    match = re.fullmatch(r"(?:(\d+)h)?(?:(\d+)min)?(?:(\d+)s)?", text)
    seconds = match and sum(int(value or 0) * unit for value, unit in zip(match.groups(), (3600, 60, 1)))
    if not seconds:
        raise argparse.ArgumentTypeError(f'"{text}": use, por exemplo, 2h, 30min, 90s ou 1h30min')
    return seconds


def print_summary(summary):
    """The summary for people, then the same as one JSON line, the last line of the output."""
    minutes, seconds = divmod(round(summary["duration_s"]), 60)
    checks = summary["checks"]
    lines = ["--- resumo ---",
             f"duração: {minutes}:{seconds:02d}",
             f"amostras: imu {summary['samples']['imu']}, obd {summary['samples']['obd']}; "
             f"vazias: imu {summary['empty']['imu']}, obd {summary['empty']['obd']}",
             f"boots vistos: {summary['boots']}",
             f"confere: {checks['passed']} certos, {len(checks['failed'])} falharam"]
    lines += [f"  linha {failed['line']}: {failed['command']}: esperado {failed['expected']}, "
              f"visto {failed['seen']}" for failed in checks["failed"]]
    if summary["error"]:
        lines.append(f"erro: {summary['error']}")
    lines.append("decodificador: " + _counters(summary["decoder"]["counters"]))
    esp32 = summary["esp32"]
    if esp32:
        lines.append(f"último STATUS: firmware {esp32['firmware']}, obd {_roteiro_name(esp32['obd_state'])}, "
                     f"imu {_roteiro_name(esp32['imu_state'])}, taxas imu {esp32['imu_rate']} "
                     f"obd {esp32['obd_rate']}; {_counters(esp32['counters'])}")
    else:
        lines.append("último STATUS: nenhum")
    print("\n".join(lines))
    print(json.dumps(summary, ensure_ascii=False), flush=True)


def _counters(counters):
    return ", ".join(f"{name} {value}" for name, value in counters.items())


def main(argv=None):
    parser = argparse.ArgumentParser(description="Simulador do PC: faz o carro e a Pi para o firmware "
                                                 "(docs/SIMULADOR.md).")
    parser.add_argument("--porta", default=DEFAULT_PORT, metavar="CAMINHO",
                        help=f"a porta do firmware (padrão {DEFAULT_PORT})")
    parser.add_argument("--firmware", metavar="CAMINHO",
                        help="fase 1: inicia o firmware nativo como processo filho, o que permite "
                             "reinicia esp32")
    parser.add_argument("--roteiro", metavar="ARQUIVO", help="roda o roteiro ao iniciar")
    parser.add_argument("--sem-tela", action="store_true", help="sem tela; imprime o resumo ao fim")
    parser.add_argument("--duracao", type=parse_duration, metavar="TEMPO",
                        help="tempo total, como 2h ou 30min")
    args = parser.parse_args(argv)
    if not args.sem_tela:
        parser.error("a tela chega no #27; por enquanto, use --sem-tela")

    roteiro = None
    if args.roteiro:
        try:
            with open(args.roteiro, encoding="utf-8") as file:
                roteiro = parse_roteiro(file.read())
        except (OSError, RoteiroError) as error:
            parser.exit(2, f"{args.roteiro}: {error}\n")
        if args.firmware is None and any(action.command.name == "restart" for action in roteiro.actions):
            parser.error("o roteiro usa reinicia esp32, que precisa de --firmware")

    simulator = Simulator(args.porta, args.firmware, roteiro, args.duracao)
    # A flag, not an exception: a 2nd Ctrl+C never cuts the firmware's stop short.
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: simulator.stop())
    passed = simulator.run()
    print_summary(simulator.summary())
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
