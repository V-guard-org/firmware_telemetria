"""Fake ESP32: speaks like the ESP32 on the wire, to test the Pi's reader and the
decoder without a board or firmware (docs/DECODIFICADOR.md §4).

    python3 host/fake_esp32.py [--path /tmp/vguard-esp32]

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
from esp32_decoder import IMU, LOG, OBD, PING, PONG, STATUS, VERSION, cobs_decode, crc16, encode_message

DEFAULT_PATH = "/tmp/vguard-esp32"

PROTOCOL_VERSION = 1
DEFAULT_IMU_RATE = 25
DEFAULT_OBD_RATE = 5

# Times in µs of the fake's own clock, like the ESP32's t_us
STATUS_INTERVAL_US = 1_000_000
TICK_US = 10_000                # the IMU is read 100 times a second (PROTOCOLO.md §3.2)
READS_PER_S = 1_000_000 // TICK_US
PI_TIMEOUT_US = 3_000_000       # the Pi is present while its PINGs arrive within 3 s
LISTEN_US = 1_000_000           # MAQUINAS-DE-ESTADO.md §2.1
DISCOVERY_US = 50_000           # 01 00, 01 20 and 01 40, each answered in about 15 ms

# The 60 s course, in a loop (DECODIFICADOR.md §4): stopped, speeds up to 60 km/h, cruises and brakes
COURSE_S = 60
COURSE_GO_S = 8
COURSE_BRAKE_S = 42
COURSE_KMH = 60
COURSE_THROTTLE = 0.6           # the most it presses
COURSE_HOLD_THROTTLE = 0.13     # about what holds the car at 60 km/h
COURSE_BRAKE = 0.4
COURSE_STOPPED_BRAKE = 0.3

# The PIDs the fake car has, how often the firmware asks for each (MAQUINAS-DE-ESTADO.md §2.4),
# and the inverse of the formulas of PROTOCOLO.md §3.3, written apart from the decoder (ADR-0006)
_PIDS = {
    0x04: ("load", 1_000_000, lambda value: [round(value * 255 / 100)]),
    0x05: ("coolant", 1_000_000, lambda value: [round(value) + 40]),
    0x0B: ("map", 1_000_000, lambda value: [round(value)]),
    0x0C: ("rpm", 200_000, lambda value: divmod(round(value * 4), 256)),
    0x0D: ("speed", 200_000, lambda value: [round(value)]),
    0x0F: ("iat", 1_000_000, lambda value: [round(value) + 40]),
    0x11: ("throttle", 1_000_000, lambda value: [round(value * 255 / 100)]),
    0x42: ("voltage", 1_000_000, lambda value: divmod(round(value * 1000), 256)),
}
_PIDS_ANNOUNCED = (*_PIDS, 0x20, 0x40)  # 0x20 and 0x40 tell the firmware to ask 01 20 and 01 40

# The messages, laid out here apart from the decoder (PROTOCOLO.md §3)
_HEADER = struct.Struct("<BHH")     # type, boot, seq
_STATUS = struct.Struct("<BQBBBBBBB12s9II")
_IMU = struct.Struct("<Q12h")
_OBD = struct.Struct("<QB")
_OBD_VALUE = struct.Struct("<BHB")
_PONG = struct.Struct("<HQ")
_LOG = struct.Struct("<QB")         # t_us, level; the text follows
_PLATFORM_NOTEBOOK = 0
_SOURCE_IMU_SIMULATED = 0x01
_RESET_POWER_ON = 1
_IMU_OK = 0
_WAITING_PI, _LISTENING, _DISCOVERING, _READING = range(4)     # estado_obd
_ESP32_COUNTER_COUNT = 9
_OBD_SENT, _OBD_ANSWERED = 3, 4     # positions in the counters
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
    whenever it is called, so the caller's rhythm changes nothing on the wire.
    """

    def __init__(self, clock=time, rng=None):
        rng = rng or random.Random()
        self._clock = clock
        self.boot = rng.randint(1, 65535)
        self._start = clock.monotonic()
        self._car = CarModel(rng)
        self._seq = 0
        self._buffer = b""
        self._next_tick_us = 0
        self._next_status_us = 0
        self._last_ping_us = None
        self._obd_state = _WAITING_PI
        self._obd_until_us = 0
        self._pids = bytes(12)
        self._next_ask_us = {}          # PID → when the firmware asks for it again
        self._answers = {}              # PID → (raw bytes, t_us of the car's answer)
        self._next_obd_us = 0
        self._readings = []             # (t_us, reading) of the IMU interval so far
        self._counters = [0] * _ESP32_COUNTER_COUNT
        self._pongs = []
        self._samples = []              # (type, body) of IMU and OBD, in the order they were made
        self._logs = []

    def feed(self, data):
        """Bytes from the Pi. Each whole PING gets a PONG with the instant it arrived."""
        now_us = self._now_us()
        self._run_until(now_us)
        *frames, rest = (self._buffer + data).split(b"\x00")
        self._buffer = rest[-_MAX_FRAME:]
        for frame in frames:
            seq = _ping_seq(frame)
            if seq is not None:
                self._pongs.append(_PONG.pack(seq, now_us))
                self._last_ping_us = now_us

    def data_to_send(self):
        """Everything due up to now, in the ESP32's output order (PROTOCOLO.md §6.5)."""
        now_us = self._now_us()
        self._run_until(now_us)
        out = bytearray()
        for body in self._pongs:
            out += self._message(PONG, body)
        if now_us >= self._next_status_us:
            out += self._message(STATUS, self._status_body(now_us))
            self._next_status_us += STATUS_INTERVAL_US
            if self._next_status_us <= now_us:  # the program stalled; no burst to catch up
                self._next_status_us = now_us + STATUS_INTERVAL_US
        for msg_type, body in self._samples:
            out += self._message(msg_type, body)
        for t_us, text in self._logs:
            out += self._message(LOG, _LOG.pack(t_us, _LOG_WARNING) + text.encode())
        self._pongs.clear()
        self._samples.clear()
        self._logs.clear()
        return bytes(out)

    def _now_us(self):
        return round((self._clock.monotonic() - self._start) * 1e6)

    def _run_until(self, now_us):
        while self._next_tick_us <= now_us:
            t_us = self._next_tick_us
            _drive(self._car, t_us / 1e6)
            self._car.step(TICK_US / 1e6)
            self._read_imu(t_us)
            self._step_obd(t_us)
            self._next_tick_us += TICK_US

    def _read_imu(self, t_us):
        self._readings.append((t_us, self._car.imu))
        if len(self._readings) < READS_PER_S // DEFAULT_IMU_RATE:
            return
        instants = [at for at, _ in self._readings]
        axes = list(zip(*(reading for _, reading in self._readings)))
        means = [sum(axis) / len(axis) for axis in axes]
        peaks = [max(axis, key=abs) for axis in axes]
        self._samples.append((IMU, _IMU.pack(round(sum(instants) / len(instants)),
                                             *_imu_wire(means), *_imu_wire(peaks))))
        self._readings.clear()

    def _step_obd(self, t_us):
        """The firmware's OBD, cut short: the car always answers (MAQUINAS-DE-ESTADO.md §2)."""
        if self._last_ping_us is None or t_us - self._last_ping_us >= PI_TIMEOUT_US:
            self._obd_state = _WAITING_PI
            self._answers.clear()
            return
        if self._obd_state == _WAITING_PI:
            self._obd_state, self._obd_until_us = _LISTENING, t_us + LISTEN_US
            self._logs.append((t_us, _FAKE_WARNING))    # any reader that just opened the port sees it
        elif self._obd_state == _LISTENING and t_us >= self._obd_until_us:
            self._obd_state, self._obd_until_us = _DISCOVERING, t_us + DISCOVERY_US
            self._count_requests(3)                     # 01 00, 01 20 and 01 40
        elif self._obd_state == _DISCOVERING and t_us >= self._obd_until_us:
            self._obd_state = _READING
            self._pids = _pid_map(_PIDS_ANNOUNCED)
            # one PID per tick, as the firmware asks one at a time; all 8 before the 1st sample
            self._next_ask_us = {pid: t_us + i * TICK_US for i, pid in enumerate(_PIDS)}
            self._next_obd_us = t_us + 1_000_000 // DEFAULT_OBD_RATE
        if self._obd_state != _READING:
            return
        values = self._car.obd()
        for pid, (name, period_us, to_raw) in _PIDS.items():
            if t_us >= self._next_ask_us[pid]:
                self._answers[pid] = (bytes(to_raw(values[name])), t_us)
                self._next_ask_us[pid] += period_us
                self._count_requests(1)
        if t_us >= self._next_obd_us:
            body = _OBD.pack(t_us, len(self._answers))
            for pid, (raw, answered_us) in sorted(self._answers.items()):
                age_ms = min(65535, round((t_us - answered_us) / 1000))
                body += _OBD_VALUE.pack(pid, age_ms, len(raw)) + raw
            self._samples.append((OBD, body))
            self._next_obd_us += 1_000_000 // DEFAULT_OBD_RATE

    def _count_requests(self, count):
        self._counters[_OBD_SENT] += count
        self._counters[_OBD_ANSWERED] += count      # the fake car always answers

    def _status_body(self, t_us):
        return _STATUS.pack(
            PROTOCOL_VERSION, t_us, _PLATFORM_NOTEBOOK, _SOURCE_IMU_SIMULATED, _RESET_POWER_ON,
            DEFAULT_IMU_RATE, DEFAULT_OBD_RATE, _IMU_OK, self._obd_state, self._pids,
            *self._counters,
            0,                                          # heap_min: 0 off the ESP32
        ) + f"fake-{VERSION}".encode()

    def _message(self, msg_type, body):
        data = encode_message(msg_type, self.boot, self._seq, body)
        self._seq = (self._seq + 1) & 0xFFFF
        return data


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


def _ping_seq(frame):
    """The seq of a whole PING; None for anything else, which the fake ignores."""
    try:
        data = cobs_decode(frame)
    except ValueError:
        return None
    payload, crc = data[:-2], data[-2:]
    if len(payload) < _HEADER.size or int.from_bytes(crc, "little") != crc16(payload):
        return None
    msg_type, _boot, seq = _HEADER.unpack_from(payload)
    return seq if msg_type == PING else None


def main(argv=None):
    parser = argparse.ArgumentParser(description="Fake ESP32 on a pty, for testing the Pi's reader.")
    parser.add_argument("--path", default=DEFAULT_PATH,
                        help=f"link to the pty, opened by the reader (default {DEFAULT_PATH})")
    args = parser.parse_args(argv)

    master, slave = os.openpty()
    tty.setraw(slave)               # no echo and no byte translation, before the reader opens it
    os.set_blocking(master, False)
    pty_name = os.ttyname(slave)
    _link(pty_name, args.path)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    esp32 = FakeEsp32()
    print(f"fake ESP32 on {args.path} -> {pty_name}, boot 0x{esp32.boot:04X}", file=sys.stderr)
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
