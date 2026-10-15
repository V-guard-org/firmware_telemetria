"""End to end: the headless simulator runs a roteiro against the real native firmware (#20).

It needs the firmware built with `pio run -e native`, which ./verifica.sh does first.
Without it, the test is skipped, as in test_native_firmware.py.
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
PROOF = """\
0:00 pi ping desliga
0:02 confere obd esperando-pi
0:02 confere boots 1
0:03 reinicia esp32
0:06 confere boots {boots}
0:07 fim
"""


@unittest.skipUnless(os.path.exists(FIRMWARE), "native firmware not built (pio run -e native)")
class HeadlessSimulator(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def start(self, name, roteiro):
        """The simulator on its own port, so the runs go side by side."""
        path = os.path.join(self.dir.name, f"{name}.txt")
        with open(path, "w", encoding="utf-8") as file:
            file.write(roteiro)
        process = subprocess.Popen(
            [sys.executable, SIMULATOR, "--firmware", FIRMWARE, "--sem-tela", "--roteiro", path,
             "--porta", os.path.join(self.dir.name, f"{name}-esp32")],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.addCleanup(self.stop, process)
        return process

    def stop(self, process):
        """A SIGTERM, so the simulator stops its firmware; a SIGKILL would leave it running."""
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=10)

    def finish(self, process):
        """The exit code and the summary, the JSON line at the end of the output."""
        out, _ = process.communicate(timeout=30)
        return process.returncode, json.loads(out.splitlines()[-1])

    def test_the_roteiro_passes_and_the_same_with_a_wrong_confere_fails(self):
        right = self.start("right", PROOF.format(boots=2))
        wrong = self.start("wrong", PROOF.format(boots=3))

        code, summary = self.finish(right)
        self.assertEqual(0, code, summary)
        self.assertEqual(2, summary["boots"])
        self.assertEqual({"passed": 3, "failed": []}, summary["checks"])
        self.assertEqual(7.0, summary["duration_s"])
        self.assertEqual("waiting_pi", summary["esp32"]["obd_state"])
        # Until the IMU simulada registers for it (#21), each IMU_SIM counts in rejeitadas:
        # 100 a second over the 3 s from the restart to the last STATUS, plus the ticks the
        # restart held up, sent in a burst once the port opens again.
        self.assertGreater(summary["esp32"]["counters"]["rejected"], 250)
        self.assertLess(summary["esp32"]["counters"]["rejected"], 400)

        code, summary = self.finish(wrong)
        self.assertEqual(1, code, summary)
        self.assertEqual(2, summary["checks"]["passed"])
        [failed] = summary["checks"]["failed"]
        self.assertEqual({"line": 5, "command": "confere boots 3", "expected": "3", "seen": "2"},
                         {key: failed[key] for key in ("line", "command", "expected", "seen")})


if __name__ == "__main__":
    unittest.main()
