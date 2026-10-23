"""End to end: the headless simulator runs roteiros against the real native firmware (#20 to #25).

It needs the firmware built with `pio run -e native`, which ./verifica.sh does first.
Without it, the tests are skipped, as in test_native_firmware.py. The runs go side by side,
started once for the whole class, so the tests take as long as the longest roteiro. They run
with --sem-can, except the OBD one, which needs vcan0 and is skipped without it (README).
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, os.pardir, ".pio", "build", "native", "program")
SIMULATOR = os.path.join(HERE, "simulator.py")


def _has_vcan0():
    try:
        with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sock:
            sock.bind(("vcan0",))
        return True
    except OSError:
        return False


HAS_VCAN0 = _has_vcan0()

# The restart from the simulator gives a new boot. Without PING the OBD stays in ESPERANDO_PI.
RESTART = """\
0:00 pi ping desliga
0:02 confere obd esperando-pi
0:02 confere boots 1
0:03 reinicia esp32
0:06 confere boots {boots}
0:07 fim
"""

# The IMU failure the simulator provokes, and its way back (#21). The car stays stopped.
IMU = """\
0:02 buraco
0:03 imu falha liga
0:05 confere imu falha
0:05 confere vazias imu 1
0:06 imu falha desliga
0:09 confere imu ok
0:10 fim
"""

# The proof of #22, in one roteiro: the STATUS confirms the 50 the Pi asks for, then the Pi stops
# reading for 10 s. A set_rates() goes out as a TAXAS after the next STATUS, and the one after it
# confirms, so each confere comes 3 s after its taxas. The lock goes at 100/s, about 4 KB/s: the
# 24 KB that the pty and the native adapter hold fill in about 6 s, and the core's queue drops
# the rest. At 50/s they would take about 11 s, longer than the lock.
RATES_AND_STUCK_PI = """\
0:00 taxas imu 50 obd 5
0:03 confere taxas imu 50 obd 5
0:03 taxas imu 100 obd 10
0:06 confere taxas imu 100 obd 10
0:06 pi trava
0:16 pi destrava
0:19 confere boots 1
0:19 fim
"""

# The proof of #25 on vcan0: the car starts, and the Decoder gets OBD from the ECU simulada.
OBD = """\
0:00 ignição liga
0:01 partida
0:03 acelerador 30%
0:06 confere obd lendo
0:06 confere pid rpm presente
0:06 confere pid speed presente
0:07 fim
"""


@unittest.skipUnless(os.path.exists(FIRMWARE), "native firmware not built (pio run -e native)")
class HeadlessSimulator(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.dir.cleanup)
        cls.runs = {name: cls.start(name, roteiro) for name, roteiro in (
            ("right", RESTART.format(boots=2)), ("wrong", RESTART.format(boots=3)), ("imu", IMU),
            ("rates_and_stuck_pi", RATES_AND_STUCK_PI))}
        if HAS_VCAN0:
            # The only run on vcan0: two would answer each other's firmware.
            cls.runs["obd"] = cls.start("obd", OBD, can=True)

    @classmethod
    def start(cls, name, roteiro, can=False):
        """The simulator on its own port, so the runs go side by side."""
        path = os.path.join(cls.dir.name, f"{name}.txt")
        with open(path, "w", encoding="utf-8") as file:
            file.write(roteiro)
        process = subprocess.Popen(
            [sys.executable, SIMULATOR, "--firmware", FIRMWARE, "--sem-tela", "--roteiro", path,
             "--porta", os.path.join(cls.dir.name, f"{name}-esp32")] + ([] if can else ["--sem-can"]),
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        cls.addClassCleanup(cls.stop, process)
        return process

    @staticmethod
    def stop(process):
        """A SIGTERM, so the simulator stops its firmware; a SIGKILL would leave it running."""
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=10)

    def finish(self, name):
        """The exit code and the summary, the JSON line at the end of the output."""
        process = self.runs[name]
        out, _ = process.communicate(timeout=30)
        return process.returncode, json.loads(out.splitlines()[-1])

    def test_the_restart_roteiro_passes(self):
        code, summary = self.finish("right")
        self.assertEqual(0, code, summary)
        self.assertEqual(2, summary["boots"])
        self.assertEqual({"passed": 3, "failed": []}, summary["checks"])
        self.assertEqual(7.0, summary["duration_s"])
        self.assertEqual("waiting_pi", summary["esp32"]["obd_state"])
        self.assertEqual("ok", summary["esp32"]["imu_state"])
        self.assertEqual(0, summary["esp32"]["counters"]["rejected"])  # the IMU simulada takes the IMU_SIM

    def test_the_same_roteiro_with_a_wrong_confere_fails(self):
        code, summary = self.finish("wrong")
        self.assertEqual(1, code, summary)
        self.assertEqual(2, summary["checks"]["passed"])
        [failed] = summary["checks"]["failed"]
        self.assertEqual({"line": 5, "command": "confere boots 3", "expected": "3", "seen": "2"},
                         {key: failed[key] for key in ("line", "command", "expected", "seen")})

    def test_the_imu_fails_when_the_simulator_says_so_and_comes_back(self):
        code, summary = self.finish("imu")
        self.assertEqual(0, code, summary)
        self.assertEqual({"passed": 3, "failed": []}, summary["checks"])
        self.assertEqual(1, summary["empty"]["imu"])
        # 25 a second, less the 3 s or so in FALHA
        self.assertGreater(summary["samples"]["imu"], 150)
        last = summary["last_samples"]["imu"]
        self.assertAlmostEqual(9.81, last["az"], delta=0.3)     # the mean, stopped and level
        self.assertAlmostEqual(0.0, last["ax"], delta=0.3)
        self.assertGreater(summary["esp32"]["counters"]["imu_failures"], 9)
        self.assertEqual(0, summary["esp32"]["counters"]["rejected"])

    def test_the_status_confirms_the_rates_and_a_stuck_pi_makes_the_firmware_drop(self):
        code, summary = self.finish("rates_and_stuck_pi")
        self.assertEqual(0, code, summary)
        self.assertEqual({"passed": 3, "failed": []}, summary["checks"])
        self.assertEqual(1, summary["boots"])                  # no reboot
        self.assertEqual((100, 10), (summary["esp32"]["imu_rate"], summary["esp32"]["obd_rate"]))
        # About 4 s of a full queue at 100 a second
        self.assertGreater(summary["esp32"]["counters"]["dropped"], 100)
        # A message dropped in the ESP32 spends no seq, so the cable shows no gap.
        self.assertEqual(0, summary["decoder"]["counters"]["lost"])
        self.assertEqual(0, summary["esp32"]["counters"]["rejected"])

    @unittest.skipUnless(HAS_VCAN0, "no vcan0 (README: sudo ip link add dev vcan0 type vcan)")
    def test_the_car_starts_and_the_decoder_gets_obd_from_the_ecu_simulada(self):
        code, summary = self.finish("obd")
        self.assertEqual(0, code, summary)
        self.assertEqual({"passed": 3, "failed": []}, summary["checks"])
        self.assertEqual(0, summary["outside_list"])
        self.assertEqual(0, summary["esp32"]["counters"]["blocked"])
        self.assertEqual("reading", summary["esp32"]["obd_state"])
        self.assertGreater(summary["samples"]["obd"], 15)  # 5 a second, from about 1.2 s
        # The values as the model has them, a sample period or so later: 4 s at 30% throttle.
        obd, car = summary["last_samples"]["obd"], summary["car_obd"]
        self.assertEqual(set(car), set(obd) - {"age_ms"})
        self.assertGreater(car["speed"], 20)
        self.assertAlmostEqual(car["speed"], obd["speed"], delta=3)
        self.assertAlmostEqual(car["rpm"], obd["rpm"], delta=0.1 * car["rpm"])
        self.assertTrue(all(age < 1500 for age in obd["age_ms"].values()), obd["age_ms"])


if __name__ == "__main__":
    unittest.main()
