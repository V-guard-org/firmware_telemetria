"""End to end: the headless simulator runs roteiros against the real native firmware (#20, #21).

It needs the firmware built with `pio run -e native`, which ./verifica.sh does first.
Without it, the tests are skipped, as in test_native_firmware.py. The runs go side by side,
started once for the whole class, so the tests take as long as the longest roteiro.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, os.pardir, ".pio", "build", "native", "program")
SIMULATOR = os.path.join(HERE, "simulator.py")

# The restart from the simulator gives a new boot. Without PING the OBD stays in
# ESPERANDO_PI, now and once the OBD arrives (#25).
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


@unittest.skipUnless(os.path.exists(FIRMWARE), "native firmware not built (pio run -e native)")
class HeadlessSimulator(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.dir.cleanup)
        cls.runs = {name: cls.start(name, roteiro) for name, roteiro in (
            ("right", RESTART.format(boots=2)), ("wrong", RESTART.format(boots=3)), ("imu", IMU))}

    @classmethod
    def start(cls, name, roteiro):
        """The simulator on its own port, so the runs go side by side."""
        path = os.path.join(cls.dir.name, f"{name}.txt")
        with open(path, "w", encoding="utf-8") as file:
            file.write(roteiro)
        process = subprocess.Popen(
            [sys.executable, SIMULATOR, "--firmware", FIRMWARE, "--sem-tela", "--roteiro", path,
             "--porta", os.path.join(cls.dir.name, f"{name}-esp32")],
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


if __name__ == "__main__":
    unittest.main()
