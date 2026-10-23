"""Fake ESP32: speaks like the ESP32 on the wire, to test the Pi's reader and the
decoder without a board or firmware (docs/DECODIFICADOR.md §4).

    python3 host/fake_esp32.py [--path /tmp/vguard-esp32] [--car-off-after S] [--reboot-every S]
                               [--garbage] [--drift-ppm N]

The logic (FakeEsp32) never opens a port: it takes bytes and instants and returns
bytes, like the Decoder. Only main() creates the pty and the link.
"""

import argparse
import fcntl
import os
import random
import select
import signal
import struct
import sys
import termios
import time
import tty

from car_model import CarModel
from ecu_sim import PIDS
from esp32_decoder import (IMU, LOG, OBD, PING, PONG, STATUS, TAXAS, VERSION, cobs_decode, crc16,
                           encode_message)

DEFAULT_PATH = "/tmp/vguard-esp32"

PROTOCOL_VERSION = 1
DEFAULT_IMU_RATE = 25
DEFAULT_OBD_RATE = 5
IMU_RATES = (100, 50, 25, 20, 10)   # the rates a TAXAS may ask for (PROTOCOLO.md §4.2)
OBD_RATES = (1, 2, 5, 10)

# Times in µs of the fake's own clock, like the ESP32's t_us
STATUS_INTERVAL_US = 1_000_000
TICK_US = 10_000                # the IMU is read 100 times a second (PROTOCOLO.md §3.2)
READS_PER_S = 1_000_000 // TICK_US
PI_TIMEOUT_US = 3_000_000       # the Pi is present while its PINGs arrive within 3 s
LISTEN_US = 1_000_000           # MAQUINAS-DE-ESTADO.md §2.1
DISCOVERY_US = 50_000           # 01 00, 01 20 and 01 40, each answered in about 15 ms
NO_ANSWER_US = 2_000_000        # with no answer for this long, SEM_RESPOSTA (MAQUINAS-DE-ESTADO.md §2.4)
GARBAGE_INTERVAL_US = 1_000_000
GARBAGE_BYTES = 20

# The 60 s course, in a loop (DECODIFICADOR.md §4): stopped, speeds up to 60 km/h, cruises and brakes
COURSE_S = 60
COURSE_GO_S = 8
COURSE_BRAKE_S = 42
COURSE_KMH = 60
COURSE_THROTTLE = 0.6           # the most it presses
COURSE_HOLD_THROTTLE = 0.13     # about what holds the car at 60 km/h
COURSE_BRAKE = 0.4
COURSE_STOPPED_BRAKE = 0.3

# The PIDs the fake car has, with the ECU simulada's inverse formulas (ADR-0006), and how
# often the firmware asks for each (MAQUINAS-DE-ESTADO.md §2.4)
_PIDS = {pid: (name, 200_000 if name in ("rpm", "speed") else 1_000_000, to_raw)
         for pid, (name, to_raw) in PIDS.items()}
_PIDS_ANNOUNCED = (*_PIDS, 0x20, 0x40)  # 0x20 and 0x40 tell the firmware to ask 01 20 and 01 40

# The messages, laid out here apart from the decoder (PROTOCOLO.md §3)
_HEADER = struct.Struct("<BHH")     # type, boot, seq
_STATUS = struct.Struct("<BQBBBBBBB12s9II")
_IMU = struct.Struct("<Q12h")
_OBD = struct.Struct("<QB")
_OBD_VALUE = struct.Struct("<BHB")
_PONG = struct.Struct("<HQ")
_TAXAS = struct.Struct("<BB")       # imu, obd
_LOG = struct.Struct("<QB")         # t_us, level; the text follows
_PLATFORM_NOTEBOOK = 0
_SOURCE_IMU_SIMULATED = 0x01
_RESET_POWER_ON = 1
_RESET_WATCHDOG = 5                 # --reboot-every fakes the restarts the field would see
_IMU_OK = 0
_WAITING_PI, _LISTENING, _DISCOVERING, _READING, _NO_RESPONSE = range(5)      # estado_obd
_ESP32_COUNTER_COUNT = 9
_REJECTED, _OBD_SENT, _OBD_ANSWERED, _OBD_TIMEOUTS = 1, 3, 4, 5     # positions in the counters
_LOG_WARNING = 2
_IMU_SCALES = (100, 100, 100, 10, 10, 10)   # 0.01 m/s² and 0.1 °/s

_FAKE_WARNING = "fake ESP32: no board, the values are made up"

_POLL_S = 0.005
# A reader takes what arrives at least every 100 ms (DECODIFICADOR.md §2.2), so
# more than this left unread means nobody has the port open.
_UNREAD_LIMIT = 2048
_MAX_FRAME = 258                    # the longest message on the wire, with its 0x00 (PROTOCOLO.md §2)


class FakeEsp32:
    """The fake's logic, without I/O: bytes and instants in, bytes out.

    It reads its car 100 times a second and works through every 10 ms tick up to now
    whenever it is called, so the caller's rhythm changes nothing on the wire. Its own
    instants count from when it started; the t_us on the wire count from each boot.
    """

    def __init__(self, clock=time, rng=None, car_off_after=None, reboot_every=None, garbage=False,
                 drift_ppm=0.0):
        """The options of main() (DECODIFICADOR.md §4): seconds, a flag and ppm."""
        self._rng = rng or random.Random()
        self._clock = clock
        self._start = clock.monotonic()
        self._rate = 1 + drift_ppm / 1e6       # everything the fake times runs on its own crystal
        self._next_garbage_us = GARBAGE_INTERVAL_US if garbage else None
        self._car_off_us = None if car_off_after is None else round(car_off_after * 1e6)
        self._reboot_us = None if reboot_every is None else round(reboot_every * 1e6)
        self.boot = None
        self._start_boot(0, _RESET_POWER_ON)
        self._car = CarModel(self._rng)
        self._next_tick_us = 0

    def _start_boot(self, at_us, reset_reason):
        """A new boot: all the ESP32 had in memory starts over, the car does not (PROTOCOLO.md §6.2)."""
        old = self.boot
        while self.boot == old:
            self.boot = self._rng.randint(1, 65535)
        self._boot_at_us = at_us
        self._reset_reason = reset_reason
        self._seq = 0
        self._buffer = b""
        self._next_status_us = at_us    # the STATUS goes right at the boot
        self._last_ping_us = None
        self._obd_state = _WAITING_PI
        self._obd_until_us = 0
        self._pids = bytes(12)
        self._next_ask_us = {}          # PID → when the firmware asks for it again
        self._answers = {}              # PID → (raw bytes, instant of the car's answer)
        self._last_answer_us = None
        self._next_obd_us = 0
        self._imu_rate = DEFAULT_IMU_RATE
        self._obd_rate = DEFAULT_OBD_RATE
        self._readings = []             # (instant, reading) of the IMU interval so far
        self._counters = [0] * _ESP32_COUNTER_COUNT
        self._pongs = []                # what is still to go out is lost with the reboot
        self._samples = []              # (type, body) of IMU and OBD, in the order they were made
        self._logs = []

    def feed(self, data):
        """Bytes from the Pi. Each whole PING gets a PONG with the instant it arrived."""
        now_us = self._now_us()
        self._run_until(now_us)
        *frames, rest = (self._buffer + data).split(b"\x00")
        self._buffer = rest[-_MAX_FRAME:]
        for frame in frames:
            msg_type, seq, body = _pi_message(frame)
            if msg_type == PING:
                self._pongs.append(_PONG.pack(seq, self._t_us(now_us)))
                self._last_ping_us = now_us
            elif msg_type == TAXAS:
                self._set_rates(body)

    def data_to_send(self):
        """Everything due up to now, in the ESP32's output order (PROTOCOLO.md §6.5)."""
        now_us = self._now_us()
        self._run_until(now_us)
        out = bytearray()
        for body in self._pongs:
            out += self._message(PONG, body)
        if now_us >= self._next_status_us:
            out += self._message(STATUS, self._status_body(self._t_us(now_us)))
            self._next_status_us = _next_due(self._next_status_us, STATUS_INTERVAL_US, now_us)
        for msg_type, body in self._samples:
            out += self._message(msg_type, body)
        for at_us, text in self._logs:
            out += self._message(LOG, _LOG.pack(self._t_us(at_us), _LOG_WARNING) + text.encode())
        if self._next_garbage_us is not None and now_us >= self._next_garbage_us:
            # Last, so the PONG that just went out is never what the noise spoils.
            out += self._rng.randbytes(GARBAGE_BYTES)
            self._next_garbage_us = _next_due(self._next_garbage_us, GARBAGE_INTERVAL_US, now_us)
        self._pongs.clear()
        self._samples.clear()
        self._logs.clear()
        return bytes(out)

    def _now_us(self):
        return round((self._clock.monotonic() - self._start) * 1e6 * self._rate)

    def _t_us(self, at_us):
        """The ESP32's t_us: µs since the boot."""
        return at_us - self._boot_at_us

    def _run_until(self, now_us):
        while self._next_tick_us <= now_us:
            at_us = self._next_tick_us
            if self._reboot_us is not None and at_us - self._boot_at_us >= self._reboot_us:
                self._start_boot(at_us, _RESET_WATCHDOG)
            _drive(self._car, at_us / 1e6)
            self._car.step(TICK_US / 1e6)
            self._read_imu(at_us)
            self._step_obd(at_us)
            self._next_tick_us += TICK_US

    def _read_imu(self, at_us):
        self._readings.append((at_us, self._car.imu))
        if len(self._readings) < READS_PER_S // self._imu_rate:     # a new rate counts from the next reading
            return
        instants = [at for at, _ in self._readings]
        axes = list(zip(*(reading for _, reading in self._readings)))
        means = [sum(axis) / len(axis) for axis in axes]
        peaks = [max(axis, key=abs) for axis in axes]
        self._samples.append((IMU, _IMU.pack(self._t_us(round(sum(instants) / len(instants))),
                                             *_imu_wire(means), *_imu_wire(peaks))))
        self._readings.clear()

    def _step_obd(self, at_us):
        """The firmware's OBD, cut short (MAQUINAS-DE-ESTADO.md §2).

        Once the car stops answering, it stays in SEM_RESPOSTA until the Pi leaves.
        """
        if self._last_ping_us is None or at_us - self._last_ping_us >= PI_TIMEOUT_US:
            self._obd_state = _WAITING_PI
            self._answers.clear()
            return
        car_answers = self._car_off_us is None or at_us < self._car_off_us
        if self._obd_state == _WAITING_PI:
            self._obd_state, self._obd_until_us = _LISTENING, at_us + LISTEN_US
            self._logs.append((at_us, _FAKE_WARNING))   # any reader that just opened the port sees it
        elif self._obd_state == _LISTENING and at_us >= self._obd_until_us:
            self._obd_state, self._obd_until_us = _DISCOVERING, at_us + DISCOVERY_US
        elif self._obd_state == _DISCOVERING and at_us >= self._obd_until_us:
            if not car_answers:
                self._obd_state = _NO_RESPONSE
                self._count_requests(1, answered=False)     # 01 00, and nothing more
                return
            self._obd_state = _READING
            self._count_requests(3)                     # 01 00, 01 20 and 01 40
            self._last_answer_us = at_us
            self._pids = _pid_map(_PIDS_ANNOUNCED)
            # one PID per tick, as the firmware asks one at a time; all 8 before the 1st sample
            self._next_ask_us = {pid: at_us + i * TICK_US for i, pid in enumerate(_PIDS)}
            self._next_obd_us = at_us + 1_000_000 // self._obd_rate
        if self._obd_state != _READING:
            return
        values = self._car.obd()
        for pid, (name, period_us, to_raw) in _PIDS.items():
            if at_us >= self._next_ask_us[pid]:
                if car_answers:
                    self._answers[pid] = (bytes(to_raw(values[name])), at_us)
                    self._last_answer_us = at_us
                self._next_ask_us[pid] += period_us
                self._count_requests(1, car_answers)
        if at_us - self._last_answer_us >= NO_ANSWER_US:
            self._obd_state = _NO_RESPONSE
            self._answers.clear()               # leaving LENDO forgets the values
            return
        if at_us >= self._next_obd_us:
            body = _OBD.pack(self._t_us(at_us), len(self._answers))
            for pid, (raw, answered_us) in sorted(self._answers.items()):
                age_ms = min(65535, round((at_us - answered_us) / 1000))
                body += _OBD_VALUE.pack(pid, age_ms, len(raw)) + raw
            self._samples.append((OBD, body))
            self._next_obd_us += 1_000_000 // self._obd_rate     # a new rate counts from the next sample

    def _set_rates(self, body):
        """Each field alone: a value outside the list counts 1 in rejeitadas (PROTOCOLO.md §4.2)."""
        if len(body) < _TAXAS.size:
            self._counters[_REJECTED] += 1
            return
        imu, obd = _TAXAS.unpack_from(body)
        if imu in IMU_RATES:
            self._imu_rate = imu
        else:
            self._counters[_REJECTED] += 1
        if obd in OBD_RATES:
            self._obd_rate = obd
        else:
            self._counters[_REJECTED] += 1

    def _count_requests(self, count, answered=True):
        self._counters[_OBD_SENT] += count
        self._counters[_OBD_ANSWERED if answered else _OBD_TIMEOUTS] += count

    def _status_body(self, t_us):
        return _STATUS.pack(
            PROTOCOL_VERSION, t_us, _PLATFORM_NOTEBOOK, _SOURCE_IMU_SIMULATED, self._reset_reason,
            self._imu_rate, self._obd_rate, _IMU_OK, self._obd_state, self._pids,
            *self._counters,
            0,                                          # heap_min: 0 off the ESP32
        ) + f"fake-{VERSION}".encode()

    def _message(self, msg_type, body):
        data = encode_message(msg_type, self.boot, self._seq, body)
        self._seq = (self._seq + 1) & 0xFFFF
        return data


def _next_due(due_us, interval_us, now_us):
    """The next instant of something done every interval; if the program stalled, no burst to catch up."""
    due_us += interval_us
    return due_us if due_us > now_us else now_us + interval_us


def _drive(car, t):
    """The driver of the 60 s course; t in seconds since the fake started."""
    t %= COURSE_S
    if t < COURSE_GO_S:
        car.throttle, car.brake = 0.0, COURSE_STOPPED_BRAKE
    elif t < COURSE_BRAKE_S:
        # Pressing less as it nears 60 km/h, so the speed settles there without bouncing.
        hold = COURSE_HOLD_THROTTLE + 0.05 * (COURSE_KMH - car.speed_kmh)
        car.throttle, car.brake = min(COURSE_THROTTLE, max(0.0, hold)), 0.0
    else:
        car.throttle, car.brake = 0.0, COURSE_BRAKE


def _imu_wire(values):
    return [max(-32767, min(32767, round(value * scale))) for value, scale in zip(values, _IMU_SCALES)]


def _pid_map(pids):
    """The 12 bytes of the STATUS: bit 7 of the 1st byte is PID 0x01 (PROTOCOLO.md §3.1)."""
    bits = bytearray(12)
    for pid in pids:
        bits[(pid - 1) // 8] |= 0x80 >> ((pid - 1) % 8)
    return bytes(bits)


def _pi_message(frame):
    """(type, seq, body) of a whole message from the Pi; all None for anything else, which it ignores."""
    try:
        data = cobs_decode(frame)
    except ValueError:
        return None, None, None
    payload, crc = data[:-2], data[-2:]
    if len(payload) < _HEADER.size or int.from_bytes(crc, "little") != crc16(payload):
        return None, None, None
    msg_type, _boot, seq = _HEADER.unpack_from(payload)
    return msg_type, seq, payload[_HEADER.size:]


def main(argv=None):
    parser = argparse.ArgumentParser(description="Fake ESP32 on a pty, for testing the Pi's reader.")
    parser.add_argument("--path", default=DEFAULT_PATH,
                        help=f"link to the pty, opened by the reader (default {DEFAULT_PATH})")
    parser.add_argument("--car-off-after", type=float, metavar="S",
                        help="the car stops answering after S seconds: the OBD stops, SEM_RESPOSTA")
    parser.add_argument("--reboot-every", type=float, metavar="S",
                        help="reboot every S seconds, with a new boot and the default rates")
    parser.add_argument("--garbage", action="store_true", help="20 random bytes on the line every second")
    parser.add_argument("--drift-ppm", type=float, default=0.0, metavar="N",
                        help="the fake's clock runs N ppm fast, to test the time sync")
    args = parser.parse_args(argv)

    master, slave = os.openpty()
    tty.setraw(slave)               # no echo and no byte translation, before the reader opens it
    os.set_blocking(master, False)
    pty_name = os.ttyname(slave)
    _link(pty_name, args.path)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    esp32 = FakeEsp32(car_off_after=args.car_off_after, reboot_every=args.reboot_every,
                      garbage=args.garbage, drift_ppm=args.drift_ppm)
    print(f"fake ESP32 on {args.path} -> {pty_name}, boot 0x{esp32.boot:04X}", file=sys.stderr)
    boot = esp32.boot
    try:
        while True:
            ready, _, _ = select.select([master], [], [], _POLL_S)
            if ready:
                esp32.feed(os.read(master, 4096))
            # Like the ESP32 with no one reading, what nobody takes is lost (D11).
            # The fake keeps its own end of the pty open, so the kernel would hold it instead.
            if _unread(slave) > _UNREAD_LIMIT:
                termios.tcflush(slave, termios.TCIFLUSH)
            _write(master, esp32.data_to_send())
            if esp32.boot != boot:
                boot = esp32.boot
                print(f"fake ESP32 rebooted, boot 0x{boot:04X}", file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        if os.path.islink(args.path) and os.readlink(args.path) == pty_name:
            os.unlink(args.path)


def _link(target, path):
    if os.path.lexists(path) and not os.path.islink(path):
        sys.exit(f"{path} exists and is not a link; choose another --path")
    temporary = f"{path}.{os.getpid()}"
    os.symlink(target, temporary)
    os.replace(temporary, path)     # replaces a link left by an earlier run


def _unread(fd):
    return struct.unpack("i", fcntl.ioctl(fd, termios.FIONREAD, b"\0\0\0\0"))[0]


def _write(fd, data):
    try:
        os.write(fd, data)
    except BlockingIOError:
        pass                        # the pty is full: the bytes are lost, as on the wire


if __name__ == "__main__":
    main()
