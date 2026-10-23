"""The ECU simulada: the car's ECUs answering OBD requests on the CAN (docs/SIMULADOR.md §2).

Like the Decoder with the port, it never opens the socket: a request frame and its
instant go in, and the answers come out, each with the instant it is due on the bus.
The simulator's loop reads and writes the socket, so the same ECU serves vcan0 and can0.
"""

import random
from typing import NamedTuple

DELAY_MS = 15                   # the answer's delay, ± JITTER_MS
JITTER_MS = 4
PADDING = 0xAA                  # fills the 8 bytes; not 0, so reading past the length shows
SUPPORT_MAPS = (0x00, 0x20, 0x40)   # 01 00, 01 20 and 01 40: which PIDs an ECU has

# The PIDs the car has, by their names in DECODIFICADOR.md §3.3, and the inverse of the
# formulas of PROTOCOLO.md §3.3, written apart from the decoder (ADR-0006)
PIDS = {
    0x04: ("load", lambda value: _one(value * 255 / 100)),
    0x05: ("coolant", lambda value: _one(value + 40)),
    0x0B: ("map", lambda value: _one(value)),
    0x0C: ("rpm", lambda value: _two(value * 4)),
    0x0D: ("speed", lambda value: _one(value)),
    0x0F: ("iat", lambda value: _one(value + 40)),
    0x11: ("throttle", lambda value: _one(value * 255 / 100)),
    0x42: ("voltage", lambda value: _two(value * 1000)),
}

# Each central (a module of the car, CONTEXT.md) by the ID it answers from, with the PIDs
# it has; it is asked at that ID − 8, or with all the others at FUNCTIONAL_ID (SIMULADOR.md §8)
FUNCTIONAL_ID = 0x7DF
PID_NAMES = frozenset(name for name, _ in PIDS.values())
ONE_CENTRAL = {0x7E8: PID_NAMES}
TWO_CENTRALS = {0x7E8: PID_NAMES - {"speed"}, 0x7E9: frozenset({"speed", "rpm"})}
PID_MODES = ("normal", "absent", "mute")


class Frame(NamedTuple):
    """A CAN frame: an 11-bit ID unless extended, and up to 8 data bytes."""
    id: int
    data: bytes
    extended: bool = False
    remote: bool = False


class EcuSim:
    """The car's centrals, answering from the values of a car model (car_model.CarModel)."""

    def __init__(self, car, rng=None, two_centrals=False):
        self.car = car
        self.delay_ms = DELAY_MS
        self.loss = 0.0                 # the share of answers lost, 0 to 0.5
        self._rng = rng or random.Random()
        self._centrals = TWO_CENTRALS if two_centrals else ONE_CENTRAL
        self._modes = {}                # PID name → "absent" or "mute"; normal ones are not here
        self.outside_list = 0           # fora_da_lista: frames received outside the closed list, 0 in every ensaio

    def set_pid(self, name, mode):
        """One of the car's PIDs, by name: "normal", "absent" (not announced, no answer) or
        "mute" (announced, no answer). Absent counts for the firmware from its next discovery."""
        if name not in PID_NAMES or mode not in PID_MODES:
            raise ValueError(f"no such PID or mode: {name} {mode}")
        if mode == "normal":
            self._modes.pop(name, None)
        else:
            self._modes[name] = mode

    def receive(self, frame, at):
        """A frame the firmware put on the bus at `at` seconds: the answers, as (instant, Frame)."""
        if not in_closed_list(frame):
            self.outside_list += 1
            return []
        if not self.car.ecus_answer:
            return []
        pid = frame.data[2]
        answers = []
        for answer_id, names in self._centrals.items():
            if frame.id not in (FUNCTIONAL_ID, answer_id - 8):
                continue
            raw = self._answer(names, pid)
            if raw is None or self._rng.random() < self.loss:
                continue
            data = bytes([2 + len(raw), 0x41, pid]) + raw
            delay_ms = max(0.0, self.delay_ms + self._rng.uniform(-JITTER_MS, JITTER_MS))
            answers.append((at + delay_ms / 1000, Frame(answer_id, data.ljust(8, bytes([PADDING])))))
        return answers

    def _answer(self, names, pid):
        """The raw bytes a central with the PIDs of these names answers to pid, or None if it keeps quiet."""
        announced = {number for number, (name, _) in PIDS.items()
                     if name in names and self._modes.get(name) != "absent"}
        if pid in SUPPORT_MAPS:
            return _support_maps(announced).get(pid)
        if pid not in announced or self._modes.get(PIDS[pid][0]) == "mute":
            return None
        name, to_raw = PIDS[pid]
        return bytes(to_raw(self.car.obd()[name]))


def in_closed_list(frame):
    """The rule of TESTES.md §5, written here apart from the firmware's gate: an 11-bit data frame
    to 0x7DF or 0x7E0 to 0x7E7, with 8 bytes, a single frame of 2 useful bytes in mode 01.
    The PID is free and the padding is not checked."""
    return (not frame.extended and not frame.remote
            and (frame.id == FUNCTIONAL_ID or 0x7E0 <= frame.id <= 0x7E7)
            and len(frame.data) == 8 and frame.data[0] == 0x02 and frame.data[1] == 0x01)


def _support_maps(pids):
    """The answers to 01 00, 01 20 and 01 40: 4 bytes each, bit 7 of the 1st byte is the map's PID + 1.

    The last bit says the next map is there; a central answers a map only if the one before announced it.
    """
    maps = {}
    for base in SUPPORT_MAPS:
        bits = sum(1 << (base + 0x20 - pid) for pid in pids if base < pid < base + 0x20)
        more = any(pid > base + 0x20 for pid in pids)
        maps[base] = (bits | more).to_bytes(4, "big")
        if not more:
            break
    return maps


def _one(value):
    """A value in A, held to what one byte takes: out of range, it saturates."""
    return [min(255, max(0, round(value)))]


def _two(value):
    """A value in 256A + B."""
    return list(divmod(min(65535, max(0, round(value))), 256))
