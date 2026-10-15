"""Decoder for the messages the V-GUARD ESP32 sends to the Raspberry Pi.

One file, Python standard library only (3.10+). The protocol is in
docs/PROTOCOLO.md and this API in docs/DECODIFICADOR.md.
"""

import binascii
import copy
import logging
import struct
import time

VERSION = "0.1.0"

PING_INTERVAL_S = 1.0
LINK_TIMEOUT_S = 3.0

STATUS = 0x01
IMU = 0x02
OBD = 0x03
PONG = 0x04
LOG = 0x05
PING = 0x81

_HEADER = struct.Struct("<BHH")     # type, boot, seq
_CRC_SIZE = 2
_MAX_MESSAGE = 255                  # header, body and CRC, before COBS
_MAX_BUFFER = 258                   # 255 bytes become at most 257 after COBS, plus the 0x00

# STATUS up to heap_min; the firmware text takes the rest (PROTOCOLO.md §3.1)
_STATUS = struct.Struct("<BQBBBBBBB12s9II")

_LOG = struct.Struct("<QB")        # t_us, level; the UTF-8 text takes the rest (§3.5)
_LOG_ERROR = 1

_MIN_BODY = {STATUS: _STATUS.size, LOG: _LOG.size}

_esp32_logger = logging.getLogger("esp32")

_PLATFORMS = ("notebook", "esp32")
_RESET_REASONS = ("unknown", "power_on", "external_pin", "software", "panic", "watchdog", "brownout")
_IMU_STATES = ("ok", "failed")
_OBD_STATES = ("waiting_pi", "listening", "discovering", "reading", "no_response",
               "wrong_bitrate", "line_stuck", "can_failure", "locked")
_ESP32_COUNTERS = ("dropped", "rejected", "imu_failures", "obd_sent", "obd_answered",
                   "obd_timeouts", "blocked", "can_errors", "bus_off")


def crc16(data):
    """CRC-16/CCITT-FALSE: polynomial 0x1021, start 0xFFFF, no reflection, no final XOR."""
    return binascii.crc_hqx(data, 0xFFFF)


def cobs_encode(data):
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data):
    """The inverse of cobs_encode, without the final 0x00. ValueError if the bytes are not COBS."""
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        end = i + code
        if code == 0 or end > len(data):
            raise ValueError("invalid COBS")
        out += data[i + 1:end]
        i = end
        if code < 255 and i < len(data):
            out.append(0)
    return bytes(out)


def encode_message(msg_type, boot, seq, body=b""):
    """The bytes on the wire: COBS(header | body | CRC16) followed by 0x00."""
    data = _HEADER.pack(msg_type, boot, seq) + body
    return cobs_encode(data + struct.pack("<H", crc16(data))) + b"\x00"


class Decoder:
    """Turns the ESP32's bytes into samples and prepares the bytes the Pi sends.

    It never opens the port: the reader passes in what it read and writes out
    what data_to_send() returns (DECODIFICADOR.md §2.2).
    """

    def __init__(self, clock=time):
        self._clock = clock
        self._seq = 0                   # of the messages the Pi sends
        self._next_ping_at = None
        self._buffer = b""
        self._skipping = False          # dropped an overlong buffer; wait for the next 0x00
        self._counters = {"lost": 0, "corrupt": 0, "unknown": 0, "boots": 0}
        self._boot = None
        self._last_seq = None
        self._last_valid_at = None
        self._esp32 = None

    def feed(self, data):
        """Bytes read from the port. Returns the samples ready for update()."""
        now = self._clock.monotonic()
        *frames, rest = (self._buffer + data).split(b"\x00")
        if self._skipping:
            if frames:
                frames.pop(0)
                self._skipping = False
            else:
                rest = b""
        if len(rest) > _MAX_BUFFER:
            self._counters["corrupt"] += 1
            self._skipping = True
            rest = b""
        self._buffer = rest
        for frame in frames:
            if frame:                   # back-to-back zeros carry no message
                self._handle_frame(frame, now)
        return []

    def data_to_send(self):
        """Everything pending for the ESP32, to be written right away."""
        now = self._clock.monotonic()
        out = bytearray()
        if self._next_ping_at is None or now >= self._next_ping_at:
            out += self._message(PING)
            self._next_ping_at = (self._next_ping_at or now) + PING_INTERVAL_S
            if self._next_ping_at <= now:   # the reader stalled; no burst to catch up
                self._next_ping_at = now + PING_INTERVAL_S
        return bytes(out)

    def stats(self):
        """The link state, the decoder's counters and the ESP32's last STATUS."""
        now = self._clock.monotonic()
        return {
            "connected": self._last_valid_at is not None
                         and now - self._last_valid_at < LINK_TIMEOUT_S,
            "boot": self._boot,
            "counters": dict(self._counters),
            "esp32": copy.deepcopy(self._esp32),
        }

    def _handle_frame(self, frame, now):
        try:
            data = cobs_decode(frame)
        except ValueError:
            data = b""
        payload, crc = data[:-_CRC_SIZE], data[-_CRC_SIZE:]
        if (len(payload) < _HEADER.size or len(data) > _MAX_MESSAGE
                or int.from_bytes(crc, "little") != crc16(payload)):
            self._counters["corrupt"] += 1
            return
        msg_type, boot, seq = _HEADER.unpack_from(payload)
        body = payload[_HEADER.size:]
        silent = self._last_valid_at is None or now - self._last_valid_at >= LINK_TIMEOUT_S
        step = None if self._last_seq is None else (seq - self._last_seq) & 0xFFFF
        if boot != self._boot:
            self._boot = boot
            self._counters["boots"] += 1
            self._esp32 = None
        elif not silent and 0 < step < 0x8000:
            # A repeated or earlier seq is a restart, not 65535 lost messages.
            self._counters["lost"] += step - 1
        self._last_seq = seq
        self._last_valid_at = now

        if msg_type in _MIN_BODY and len(body) < _MIN_BODY[msg_type]:
            # The CRC matched, so the header counted above; only the body is unreadable.
            self._counters["corrupt"] += 1
        elif msg_type == STATUS:
            self._esp32 = _parse_status(body)
        elif msg_type == LOG:
            _t_us, level = _LOG.unpack_from(body)
            _esp32_logger.log(logging.ERROR if level == _LOG_ERROR else logging.WARNING,
                              "%s", body[_LOG.size:].decode("utf-8", "replace"))
        elif msg_type not in (IMU, OBD, PONG):
            self._counters["unknown"] += 1

    def _message(self, msg_type, body=b""):
        data = encode_message(msg_type, 0, self._seq, body)
        self._seq = (self._seq + 1) & 0xFFFF
        return data


def _parse_status(body):
    (protocol, t_us, platform, sources, reset, imu_rate, obd_rate, imu_state, obd_state,
     pids, *rest) = _STATUS.unpack_from(body)
    counters, heap_min = rest[:-1], rest[-1]
    return {
        "protocol": protocol,
        "uptime_s": t_us / 1e6,
        "platform": _code_name(_PLATFORMS, platform),
        "imu_simulated": bool(sources & 0x01),
        "reset_reason": _code_name(_RESET_REASONS, reset),
        "imu_rate": imu_rate,
        "obd_rate": obd_rate,
        "imu_state": _code_name(_IMU_STATES, imu_state),
        "obd_state": _code_name(_OBD_STATES, obd_state),
        "supported_pids": [pid for pid in range(0x01, 0x61)
                           if pids[(pid - 1) // 8] & (0x80 >> ((pid - 1) % 8))],
        "counters": dict(zip(_ESP32_COUNTERS, counters)),
        "heap_min": heap_min,
        "firmware": body[_STATUS.size:].decode("ascii", "replace"),
    }


def _code_name(names, code):
    return names[code] if code < len(names) else "unknown"
