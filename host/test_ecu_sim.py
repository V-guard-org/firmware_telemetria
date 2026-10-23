"""The ECU simulada without a socket: request frames and instants in, answers out (TESTES.md §3.2)."""

import random
import unittest

import esp32_decoder
from car_model import CarModel
from ecu_sim import EcuSim, Frame
from test_esp32_decoder import FakeClock, obd, ping_seq, pong, status

# Written here again, apart from ecu_sim.PIDS, so a wrong number there shows (PROTOCOLO.md §3.3)
PID_NUMBERS = {"load": 0x04, "coolant": 0x05, "map": 0x0B, "rpm": 0x0C, "speed": 0x0D, "iat": 0x0F,
               "throttle": 0x11, "voltage": 0x42}
# What one step of the raw bytes is worth (PROTOCOLO.md §3.3); the decoder also rounds % to 0.1
RESOLUTION = {"load": 100 / 255 + 0.1, "coolant": 1, "map": 1, "rpm": 0.25, "speed": 1, "iat": 1,
              "throttle": 100 / 255 + 0.1, "voltage": 0.001}


class StillCar:
    """A car that holds the OBD values it is given, with its centrals answering."""

    def __init__(self, **values):
        self.values = {"load": 30.0, "coolant": 90.0, "map": 35.0, "rpm": 800.0, "speed": 60.0,
                       "iat": 31.0, "throttle": 14.0, "voltage": 14.1, **values}
        self.ecus_answer = True

    def obd(self):
        return dict(self.values)


def request(pid, to=0x7DF):
    """A mode 01 request, as the firmware sends it."""
    return Frame(to, bytes([0x02, 0x01, pid, 0, 0, 0, 0, 0]))


class Answers(unittest.TestCase):

    def test_rpm_asked_of_all_comes_back_from_0x7e8_about_15_ms_later(self):
        ecu = EcuSim(StillCar(rpm=800), rng=random.Random(1))
        [(at, frame)] = ecu.receive(request(0x0C), 10.0)
        self.assertAlmostEqual(at, 10.015, delta=0.004)
        self.assertEqual(frame, Frame(0x7E8, bytes.fromhex("04 41 0C 0C 80 AA AA AA")))   # 800·4 = 0x0C80

    def test_support_maps_announce_the_8_pids_and_the_next_map(self):
        ecu = EcuSim(StillCar(), rng=random.Random(1))
        answers = {pid: ecu.receive(request(pid), 0.0) for pid in (0x00, 0x20, 0x40)}
        # 01 00: 0x04 0x05 | 0x0B 0x0C 0x0D 0x0F | 0x11 | 0x20, bit 7 of the 1st byte is PID 0x01
        self.assertEqual(answers[0x00][0][1], Frame(0x7E8, bytes.fromhex("06 41 00 18 3A 80 01 AA")))
        self.assertEqual(answers[0x20][0][1], Frame(0x7E8, bytes.fromhex("06 41 20 00 00 00 01 AA")))
        self.assertEqual(answers[0x40][0][1], Frame(0x7E8, bytes.fromhex("06 41 40 40 00 00 00 AA")))

    def test_a_request_to_0x7e0_plus_n_goes_only_to_central_n(self):
        ecu = EcuSim(StillCar(), rng=random.Random(1))
        self.assertEqual([frame.id for _, frame in ecu.receive(request(0x0C, to=0x7E0), 0.0)], [0x7E8])
        self.assertEqual(ecu.receive(request(0x0C, to=0x7E1), 0.0), [])


class DelayAndLoss(unittest.TestCase):

    def delays_ms(self, ecu, count=500):
        return [(at - 1.0) * 1000 for at, _ in
                (answer for _ in range(count) for answer in ecu.receive(request(0x0C), 1.0))]

    def test_the_delay_varies_4_ms_either_way(self):
        ecu = EcuSim(StillCar(), rng=random.Random(2))
        ecu.delay_ms = 30
        delays = self.delays_ms(ecu)
        self.assertGreaterEqual(min(delays), 26)
        self.assertLessEqual(max(delays), 34)
        self.assertLess(min(delays), 27)                # it does spread over the whole ±4 ms
        self.assertGreater(max(delays), 33)

    def test_an_answer_never_comes_before_its_request(self):
        ecu = EcuSim(StillCar(), rng=random.Random(2))
        ecu.delay_ms = 0
        self.assertGreaterEqual(min(self.delays_ms(ecu)), 0)

    def test_half_the_answers_lost_at_50_percent(self):
        ecu = EcuSim(StillCar(), rng=random.Random(3))
        ecu.loss = 0.5
        self.assertAlmostEqual(len(self.delays_ms(ecu, 1000)), 500, delta=60)

    def test_none_lost_by_default(self):
        self.assertEqual(len(self.delays_ms(EcuSim(StillCar()), 200)), 200)

    def test_the_same_seed_gives_the_same_answers(self):
        def run(seed):
            ecu = EcuSim(StillCar(), rng=random.Random(seed), two_centrals=True)
            ecu.loss = 0.3
            return [ecu.receive(request(pid), 0.0) for pid in (0x00, 0x0C, 0x0D) * 50]
        self.assertEqual(run(7), run(7))
        self.assertNotEqual(run(7), run(8))


class Ignition(unittest.TestCase):
    """With the real car model: ignition off mutes the centrals, and so does the start for 0.5 s."""

    def setUp(self):
        self.car = CarModel(noise=False)
        self.ecu = EcuSim(self.car, rng=random.Random(1))

    def answers(self):
        return [frame.data for _, frame in self.ecu.receive(request(0x0C), 0.0)]

    def run_car(self, seconds):
        for _ in range(round(seconds * 100)):
            self.car.step(0.01)

    def test_ignition_off_mutes_every_central(self):
        self.car.ignition = False
        self.assertEqual(self.answers(), [])
        self.assertEqual(EcuSim(self.car, two_centrals=True).receive(request(0x00), 0.0), [])

    def test_the_start_mutes_them_for_half_a_second_then_rpm_is_250(self):
        self.car.ignition = False
        self.car.ignition = True
        self.car.start()
        self.run_car(0.45)
        self.assertEqual(self.answers(), [])
        self.run_car(0.1)
        self.assertEqual(self.answers(), [bytes.fromhex("04 41 0C 03 E8 AA AA AA")])     # 250·4 = 0x03E8


class PidModes(unittest.TestCase):
    """Absent: not announced and no answer. Mute: announced, but no answer."""

    def setUp(self):
        self.ecu = EcuSim(StillCar(), rng=random.Random(1))

    def data(self, pid):
        return [frame.data for _, frame in self.ecu.receive(request(pid), 0.0)]

    def test_an_absent_pid_is_not_announced_and_does_not_answer(self):
        self.ecu.set_pid("voltage", "absent")
        # 0x42 was the only PID past 0x20, so 01 00 no longer announces 01 20 either
        self.assertEqual(self.data(0x00), [bytes.fromhex("06 41 00 18 3A 80 00 AA")])
        self.assertEqual(self.data(0x20), [])
        self.assertEqual(self.data(0x42), [])

    def test_a_mute_pid_is_announced_but_does_not_answer(self):
        self.ecu.set_pid("rpm", "mute")
        self.assertEqual(self.data(0x00), [bytes.fromhex("06 41 00 18 3A 80 01 AA")])
        self.assertEqual(self.data(0x0C), [])
        self.assertEqual(len(self.data(0x0D)), 1)

    def test_back_to_normal_it_answers_again(self):
        self.ecu.set_pid("rpm", "absent")
        self.ecu.set_pid("rpm", "normal")
        self.assertEqual(len(self.data(0x0C)), 1)

    def test_an_unknown_pid_or_mode_is_an_error(self):
        with self.assertRaises(ValueError):
            self.ecu.set_pid("oil", "mute")
        with self.assertRaises(ValueError):
            self.ecu.set_pid("rpm", "quiet")


class ClosedList(unittest.TestCase):
    """Each frame received is checked against the rule of TESTES.md §5, apart from the firmware's gate."""

    OUTSIDE = {
        "clear the trouble codes (mode 04)": Frame(0x7DF, bytes.fromhex("01 04 00 00 00 00 00 00")),
        "write to the ECU (0x2E)": Frame(0x7E0, bytes.fromhex("04 2E F1 90 01 00 00 00")),
        "security access (0x27)": Frame(0x7E0, bytes.fromhex("02 27 01 00 00 00 00 00")),
        "routine (0x31)": Frame(0x7E0, bytes.fromhex("04 31 01 FF 00 00 00 00")),
        "diagnostic session (0x10)": Frame(0x7E0, bytes.fromhex("02 10 03 00 00 00 00 00")),
        "flow control (0x30)": Frame(0x7E0, bytes.fromhex("30 00 00 00 00 00 00 00")),
        "mode 09": Frame(0x7DF, bytes.fromhex("02 09 02 00 00 00 00 00")),
        "29-bit ID": Frame(0x18DB33F1, bytes.fromhex("02 01 0C 00 00 00 00 00"), extended=True),
        "29-bit 0x7DF": Frame(0x7DF, bytes.fromhex("02 01 0C 00 00 00 00 00"), extended=True),
        "remote frame": Frame(0x7DF, bytes(8), remote=True),
        "7 bytes": Frame(0x7DF, bytes.fromhex("02 01 0C 00 00 00 00")),
        "no data": Frame(0x7DF, b""),
        "3 useful bytes": Frame(0x7DF, bytes.fromhex("03 01 0C 00 00 00 00 00")),
        "an answer ID": Frame(0x7E8, bytes.fromhex("02 01 0C 00 00 00 00 00")),
        "past 0x7E7": Frame(0x7DE, bytes.fromhex("02 01 0C 00 00 00 00 00")),
    }

    def setUp(self):
        self.ecu = EcuSim(StillCar(), rng=random.Random(1))

    def test_each_frame_outside_the_list_counts_once_and_gets_no_answer(self):
        for reason, frame in self.OUTSIDE.items():
            with self.subTest(reason):
                before = self.ecu.outside_list
                self.assertEqual(self.ecu.receive(frame, 0.0), [])
                self.assertEqual(self.ecu.outside_list, before + 1)

    def test_mode_01_to_0x7df_or_0x7e0_to_0x7e7_is_inside_whatever_the_pid_and_padding(self):
        for to in (0x7DF, *range(0x7E0, 0x7E8)):
            self.ecu.receive(Frame(to, bytes.fromhex("02 01 A6 55 55 55 55 55")), 0.0)
        self.assertEqual(self.ecu.outside_list, 0)


class TwoCentrals(unittest.TestCase):
    """The engine at 0x7E8 has all but speed; the gearbox at 0x7E9 has only speed and rpm (SIMULADOR.md §8)."""

    def setUp(self):
        self.ecu = EcuSim(StillCar(), rng=random.Random(1), two_centrals=True)

    def frames(self, pid, to=0x7DF):
        return [frame for _, frame in self.ecu.receive(request(pid, to), 0.0)]

    def test_both_answer_01_00_with_their_own_maps(self):
        self.assertEqual(self.frames(0x00), [
            Frame(0x7E8, bytes.fromhex("06 41 00 18 32 80 01 AA")),    # no 0x0D
            Frame(0x7E9, bytes.fromhex("06 41 00 00 18 00 00 AA")),    # 0x0C and 0x0D, nothing past 0x20
        ])

    def test_only_the_engine_announces_and_answers_01_20(self):
        self.assertEqual([frame.id for frame in self.frames(0x20)], [0x7E8])

    def test_each_answers_only_its_own_pids(self):
        self.assertEqual([frame.id for frame in self.frames(0x0C)], [0x7E8, 0x7E9])
        self.assertEqual([frame.id for frame in self.frames(0x0D)], [0x7E9])
        self.assertEqual([frame.id for frame in self.frames(0x0D, to=0x7E1)], [0x7E9])
        self.assertEqual(self.frames(0x0D, to=0x7E0), [])
        self.assertEqual(self.frames(0x42, to=0x7E1), [])
        self.assertEqual(self.frames(0x0C, to=0x7E2), [])


class RoundTrip(unittest.TestCase):
    """The model's value, made bytes by the ECU, comes back through the Decoder within the PID's resolution."""

    VALUES = {
        "load": (0, 18, 33.3, 88, 100),
        "coolant": (25, 72.4, 90),
        "map": (30, 62.5, 95),
        "rpm": (0, 250, 800, 2345.6, 6123.4),
        "speed": (0, 37.6, 60, 180),
        "iat": (31,),
        "throttle": (14, 50.5, 94),
        "voltage": (12.4, 13.87, 14.1),
    }

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)
        self.decoder.feed(status())
        self.decoder.feed(pong(ping_seq(self.decoder.data_to_send()), 1_000_000))

    def test_every_pid_comes_back_within_its_resolution(self):
        seq = 1
        for name, values in self.VALUES.items():
            for value in values:
                with self.subTest(name=name, value=value):
                    car = StillCar(**{name: value})
                    [(_, frame)] = EcuSim(car, rng=random.Random(1)).receive(request(PID_NUMBERS[name]), 0.0)
                    raw = frame.data[3:1 + frame.data[0]]
                    [(_, _, sample)] = self.decoder.feed(obd([(PID_NUMBERS[name], 0, raw.hex())], seq=seq))
                    self.assertAlmostEqual(sample[name], value, delta=RESOLUTION[name] / 2)
                    seq += 1

    def test_the_car_models_own_values_come_back_through_a_drive(self):
        car = CarModel(random.Random(4))
        ecu = EcuSim(car, rng=random.Random(4))
        self.assertEqual(set(car.obd()), set(PID_NUMBERS))
        # idle, full throttle up to speed, a hard brake, then the engine off and a new start
        phases = ((0.0, 0.0, 5), (1.0, 0.0, 40), (0.0, 1.0, 6), ("off", 0.0, 3), ("start", 0.0, 3))
        seq = 1
        for throttle, brake, seconds in phases:
            if throttle == "off":
                car.ignition, throttle = False, 0.0
            elif throttle == "start":
                car.ignition, throttle = True, 0.0
                car.start()
            car.throttle, car.brake = throttle, brake
            for tick in range(seconds * 100):
                car.step(0.01)
                if tick % 50 or not car.ecus_answer:
                    continue
                values = car.obd()
                for name, number in PID_NUMBERS.items():
                    with self.subTest(name=name, value=values[name]):
                        [(_, frame)] = ecu.receive(request(number), 0.0)
                        raw = frame.data[3:1 + frame.data[0]]
                        [(_, _, sample)] = self.decoder.feed(obd([(number, 0, raw.hex())], seq=seq))
                        self.assertAlmostEqual(sample[name], values[name], delta=RESOLUTION[name] / 2)
                        seq = (seq + 1) & 0xFFFF


if __name__ == "__main__":
    unittest.main()
