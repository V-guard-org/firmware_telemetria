"""End to end: the Decoder opens the link of the real native firmware (#19, PLATAFORMA.md §4.2).

It needs the firmware built with `pio run -e native`, which ./verifica.sh does first.
Without it, the test is skipped, as on the Pi.
"""

import os
import select
import subprocess
import tempfile
import time
import tty
import unittest

from esp32_decoder import Decoder

FIRMWARE = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, ".pio", "build",
                        "native", "program")


@unittest.skipUnless(os.path.exists(FIRMWARE), "native firmware not built (pio run -e native)")
class NativeFirmwareWithDecoder(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.path = os.path.join(self.dir.name, "vguard-esp32")
        self.firmware = subprocess.Popen([FIRMWARE, "--path", self.path], stderr=subprocess.PIPE)
        self.addCleanup(self.stop_firmware)
        deadline = time.monotonic() + 2
        while not os.path.exists(self.path):
            self.assertLess(time.monotonic(), deadline, "the firmware never created its link")
            time.sleep(0.01)
        # Like the reader in DECODIFICADOR.md §2.2; a pty has no baud rate to set.
        self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY)
        self.addCleanup(os.close, self.fd)
        tty.setraw(self.fd)
        self.decoder = Decoder()

    def stop_firmware(self):
        if self.firmware.poll() is None:
            self.firmware.terminate()
            self.firmware.wait(timeout=2)
        self.firmware.stderr.close()

    def run_for(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            data = os.read(self.fd, 4096) if ready else b""
            self.assertEqual([], [s for s in self.decoder.feed(data) if s[2]], "no samples yet")
            os.write(self.fd, self.decoder.data_to_send())

    def test_status_of_the_native_firmware_reaches_the_decoder(self):
        self.run_for(1.5)
        self.assertIsNotNone(self.decoder.stats()["esp32"], "no STATUS in 1.5 s")
        corrupt_at_start = self.decoder.stats()["counters"]["corrupt"]

        self.run_for(2.5)
        stats = self.decoder.stats()
        esp32 = stats["esp32"]
        self.assertEqual("notebook", esp32["platform"])
        self.assertEqual("waiting_pi", esp32["obd_state"])
        self.assertEqual("power_on", esp32["reset_reason"])
        self.assertEqual(1, esp32["protocol"])
        self.assertEqual((25, 5), (esp32["imu_rate"], esp32["obd_rate"]))
        self.assertTrue(esp32["imu_simulated"])
        self.assertTrue(esp32["firmware"].startswith("0.1.0"), esp32["firmware"])
        self.assertEqual(0, esp32["heap_min"])
        self.assertEqual({"dropped": 0, "rejected": 0}, {k: esp32["counters"][k] for k in ("dropped", "rejected")})
        self.assertEqual(corrupt_at_start, stats["counters"]["corrupt"])
        self.assertEqual(0, stats["counters"]["lost"])
        self.assertTrue(stats["synced"], "the PONGs gave no time pair")

    def test_the_link_is_removed_when_the_firmware_stops(self):
        self.firmware.terminate()
        self.assertEqual(0, self.firmware.wait(timeout=2))
        self.assertFalse(os.path.lexists(self.path))


if __name__ == "__main__":
    unittest.main()
