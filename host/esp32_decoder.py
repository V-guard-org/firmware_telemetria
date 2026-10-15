"""Decoder for the messages the V-GUARD ESP32 sends to the Raspberry Pi.

One file, Python standard library only (3.10+). The protocol is in
docs/PROTOCOLO.md and this API in docs/DECODIFICADOR.md.
"""

import binascii
import copy
import logging
import struct
import time

VERSION = "0.2.0"

# The time sync (DECODIFICADOR.md §3.8). They change only after a measurement (TESTES.md §7.3).
PING_INTERVAL_S = 1.0
MAX_RTT_S = 0.020           # from D12: a pair is off by at most half its round trip
BUCKET_S = 10               # of each 10 s, only the fastest pair is kept
BUCKETS = 12                # pairs in the line: the last 2 min
MIN_SPAN_S = 30             # pairs closer than this give only the offset
LINK_TIMEOUT_S = 3.0
PONG_WINDOW_S = 3.0         # a PONG counts only for a PING sent within this

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

_IMU = struct.Struct("<Q12h")      # t_us, then mean and peak of ax ay az gx gy gz (§3.2)
_OBD = struct.Struct("<QB")        # t_us, n; the n values follow (§3.3)
_OBD_VALUE = struct.Struct("<BHB")  # pid, age, size; the raw bytes follow
_PONG = struct.Struct("<HQ")       # seq_ping, t_us (§3.4)

_MIN_BODY = {STATUS: _STATUS.size, IMU: _IMU.size, OBD: _OBD.size, PONG: _PONG.size,
             LOG: _LOG.size}

_IMU_KEYS = ("ax", "ay", "az", "gx", "gy", "gz")
_IMU_DIVISORS = (100, 100, 100, 10, 10, 10)    # 0.01 m/s² and 0.1 °/s on the wire

# PID: the name in the sample, the raw bytes the formula uses, and the formula (DECODIFICADOR.md §3.3).
# A new PID is one more line here, and a new VERSION.
_OBD_FORMULAS = {
    0x04: ("load", 1, lambda raw: round(raw[0] * 100 / 255, 1)),
    0x05: ("coolant", 1, lambda raw: raw[0] - 40),
    0x0B: ("map", 1, lambda raw: raw[0]),
    0x0C: ("rpm", 2, lambda raw: (256 * raw[0] + raw[1]) / 4),
    0x0D: ("speed", 1, lambda raw: raw[0]),
    0x0F: ("iat", 1, lambda raw: raw[0] - 40),
    0x11: ("throttle", 1, lambda raw: round(raw[0] * 100 / 255, 1)),
    0x42: ("voltage", 2, lambda raw: (256 * raw[0] + raw[1]) / 1000),
}

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
        self._counters = {"lost": 0, "corrupt": 0, "unsynced": 0, "unknown": 0, "skipped_values": 0,
                          "slow_pongs": 0, "imu": 0, "obd": 0, "boots": 0}
        self._boot = None
        self._last_seq = None
        self._last_valid_at = None
        self._esp32 = None
        self._pings = {}                # seq → instant each PING went out
        self._sync = _Sync()

    def feed(self, data):
        """Bytes read from the port. Returns the samples ready for update()."""
        now = self._clock.monotonic()
        wall = self._clock.time() - now     # taken at each delivery, so a step of chrony counts at once
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
        samples = []
        for frame in frames:
            if frame:                   # back-to-back zeros carry no message
                sample = self._handle_frame(frame, now)
                if sample:
                    source, t_us, values = sample
                    samples.append((source, self._sync.to_monotonic(t_us) + wall, values))
        return samples

    def data_to_send(self):
        """Everything pending for the ESP32, to be written right away."""
        now = self._clock.monotonic()
        out = bytearray()
        if self._next_ping_at is None or now >= self._next_ping_at:
            self._pings = {seq: at for seq, at in self._pings.items() if now - at < PONG_WINDOW_S}
            self._pings[self._seq] = now
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
            "synced": self._synced(),
            "boot": self._boot,
            "sync": self._sync.health(now),
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
            self._sync = _Sync()        # the ESP32's clock started again from zero
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
        elif msg_type == PONG:
            seq_ping, t_us = _PONG.unpack_from(body)
            sent_at = self._pings.pop(seq_ping, None)
            if sent_at is None or now - sent_at >= PONG_WINDOW_S:
                pass                    # no PING of ours, or too old to tell the time
            elif now - sent_at > MAX_RTT_S:
                self._counters["slow_pongs"] += 1
            else:
                self._sync.add(sent_at, now, t_us)
        elif msg_type == IMU:
            t_us, *values = _IMU.unpack_from(body)
            return self._deliver("imu", t_us, _imu_sample(values))
        elif msg_type == OBD:
            parsed = _parse_obd(body)
            if parsed is None:
                self._counters["corrupt"] += 1
            else:
                t_us, values, skipped = parsed
                sample = self._deliver("obd", t_us, values)
                if sample:
                    self._counters["skipped_values"] += skipped
                return sample
        else:
            self._counters["unknown"] += 1
        return None

    def _deliver(self, source, t_us, values):
        """The sample, if this boot has a STATUS and a pair to convert its t_us (PROTOCOLO.md §6.2)."""
        if not self._synced():
            self._counters["unsynced"] += 1
            return None
        self._counters[source] += 1
        return source, t_us, values

    def _synced(self):
        return self._esp32 is not None and self._sync.ready

    def _message(self, msg_type, body=b""):
        data = encode_message(msg_type, 0, self._seq, body)
        self._seq = (self._seq + 1) & 0xFFFF
        return data


class _Sync:
    """Turns the ESP32's t_us into the Pi's monotonic clock, from the PING/PONG pairs (§3.8)."""

    def __init__(self):
        self._best = {}                 # 10 s of the ESP32's clock → its fastest (rtt, t_us, mid)
        self._last = None               # (rtt, arrival) of the last pair accepted
        self._line = None               # (t_us, mid, slope): the Pi's instant at t_us, and s per µs
        self._drift_ppm = None

    @property
    def ready(self):
        return self._line is not None

    def add(self, sent_at, arrived_at, t_us):
        rtt = arrived_at - sent_at
        self._last = (rtt, arrived_at)
        bucket = t_us // (BUCKET_S * 1_000_000)
        if bucket not in self._best or rtt < self._best[bucket][0]:
            self._best[bucket] = (rtt, t_us, (sent_at + arrived_at) / 2)
            if len(self._best) > BUCKETS:
                del self._best[min(self._best)]
            self._fit()

    def _fit(self):
        pairs = [(t_us, mid) for _rtt, t_us, mid in self._best.values()]
        first_us, last_us = min(pairs)[0], max(pairs)[0]
        if last_us - first_us < MIN_SPAN_S * 1_000_000:
            # 1 ms of noise over 10 s would tilt the line by 100 ppm, more than a crystal drifts.
            self._line = (*max(pairs), 1e-6)
            self._drift_ppm = None
            return
        # Least squares, around the mean so that hours of t_us keep their precision.
        mean_us = sum(t_us for t_us, _ in pairs) / len(pairs)
        mean_at = sum(mid for _, mid in pairs) / len(pairs)
        slope = (sum((t_us - mean_us) * (mid - mean_at) for t_us, mid in pairs)
                 / sum((t_us - mean_us) ** 2 for t_us, _ in pairs))
        self._line = (mean_us, mean_at, slope)
        self._drift_ppm = (1e-6 / slope - 1) * 1e6     # positive: the ESP32's clock runs fast

    def to_monotonic(self, t_us):
        line_us, line_at, slope = self._line
        return line_at + (t_us - line_us) * slope

    def health(self, now):
        rtt, arrival = self._last or (None, None)
        return {
            "pairs": len(self._best),
            "rtt_ms": None if rtt is None else round(rtt * 1000, 2),
            "age_s": None if arrival is None else round(now - arrival, 1),
            "drift_ppm": None if self._drift_ppm is None else round(self._drift_ppm, 1),
        }


def _imu_sample(values):
    keys = _IMU_KEYS + tuple(key + "_peak" for key in _IMU_KEYS)
    return {key: value / divisor for key, value, divisor in zip(keys, values, _IMU_DIVISORS * 2)}


def _parse_obd(body):
    """(t_us, sample, values skipped), or None if the values run past the end of the body."""
    t_us, count = _OBD.unpack_from(body)
    sample, ages = {}, {}
    skipped = 0
    at = _OBD.size
    for _ in range(count):
        if at + _OBD_VALUE.size > len(body):
            return None
        pid, age, size = _OBD_VALUE.unpack_from(body, at)
        at += _OBD_VALUE.size
        if at + size > len(body):
            return None
        name, used, formula = _OBD_FORMULAS.get(pid, (None, None, None))
        if formula is None or size < used:
            skipped += 1
        else:
            sample[name] = formula(body[at:at + size])
            ages[name] = age
        at += size
    sample["age_ms"] = ages
    return t_us, sample, skipped


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
