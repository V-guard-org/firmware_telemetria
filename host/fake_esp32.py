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

from esp32_decoder import LOG, STATUS, VERSION, encode_message

DEFAULT_PATH = "/tmp/vguard-esp32"

STATUS_INTERVAL_S = 1.0
PROTOCOL_VERSION = 1
DEFAULT_IMU_RATE = 25
DEFAULT_OBD_RATE = 5

# STATUS up to heap_min, laid out here apart from the decoder (PROTOCOLO.md §3.1)
_STATUS = struct.Struct("<BQBBBBBBB12s9II")
_PLATFORM_NOTEBOOK = 0
_SOURCE_IMU_SIMULATED = 0x01
_RESET_POWER_ON = 1
_IMU_OK = 0
_OBD_WAITING_PI = 0
_ESP32_COUNTER_COUNT = 9

_LOG = struct.Struct("<QB")         # t_us, level; the text follows
_LOG_WARNING = 2

_POLL_S = 0.005
# A reader takes what arrives at least every 100 ms (DECODIFICADOR.md §2.2), so
# more than this left unread means nobody has the port open.
_UNREAD_LIMIT = 2048


class FakeEsp32:
    """The fake's logic, without I/O: bytes and instants in, bytes out."""

    def __init__(self, clock=time, rng=None):
        self._clock = clock
        self.boot = (rng or random.Random()).randint(1, 65535)
        self._start = clock.monotonic()
        self._seq = 0
        self._next_status_at = self._start
        self._logs = ["fake ESP32 started: no board, the values are made up"]

    def feed(self, data):
        """Bytes from the Pi. For now nothing in them changes what the fake sends."""

    def data_to_send(self):
        """Everything due up to now, in the ESP32's output order (PROTOCOLO.md §6.5)."""
        now = self._clock.monotonic()
        t_us = round((now - self._start) * 1e6)
        out = bytearray()
        if now >= self._next_status_at:
            out += self._message(STATUS, self._status_body(t_us))
            self._next_status_at += STATUS_INTERVAL_S
            if self._next_status_at <= now:     # the program stalled; no burst to catch up
                self._next_status_at = now + STATUS_INTERVAL_S
        for text in self._logs:
            out += self._message(LOG, _LOG.pack(t_us, _LOG_WARNING) + text.encode())
        self._logs.clear()
        return bytes(out)

    def _status_body(self, t_us):
        return _STATUS.pack(
            PROTOCOL_VERSION, t_us, _PLATFORM_NOTEBOOK, _SOURCE_IMU_SIMULATED, _RESET_POWER_ON,
            DEFAULT_IMU_RATE, DEFAULT_OBD_RATE, _IMU_OK, _OBD_WAITING_PI,
            bytes(12),                                  # no PID discovered
            *[0] * _ESP32_COUNTER_COUNT,
            0,                                          # heap_min: 0 off the ESP32
        ) + f"fake-{VERSION}".encode()

    def _message(self, msg_type, body):
        data = encode_message(msg_type, self.boot, self._seq, body)
        self._seq = (self._seq + 1) & 0xFFFF
        return data


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
