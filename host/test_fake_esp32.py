"""Quick test: the fake ESP32 wired to the Decoder, without a pty, on the fake clock (TESTES.md §3.2)."""

import random
import unittest

import esp32_decoder
import fake_esp32
from test_esp32_decoder import FakeClock

OBD_KEYS = {"load", "coolant", "map", "rpm", "speed", "iat", "throttle", "voltage", "age_ms"}


class FakeEsp32WithDecoder(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.start = self.clock.time()
        self.esp32 = fake_esp32.FakeEsp32(self.clock, random.Random(17))
        self.decoder = esp32_decoder.Decoder(self.clock)
        self.imu = []                                   # (seconds since the start, sample)
        self.obd = []

    def run_for(self, seconds, pings=True):
        """Both ends on the fake clock, 10 ms at a time; with pings=False the PINGs never arrive."""
        for _ in range(round(seconds * 100)):
            sent = self.decoder.data_to_send()
            if pings:
                self.esp32.feed(sent)
            for source, ts, sample in self.decoder.feed(self.esp32.data_to_send()):
                (self.imu if source == "imu" else self.obd).append((ts - self.start, sample))
            self.clock.advance(0.01)

    def test_60_s_of_imu_and_obd_without_lost_or_corrupt(self):
        with self.assertLogs("esp32", "WARNING") as logs:
            self.run_for(60)

        stats = self.decoder.stats()
        self.assertTrue(stats["connected"])
        self.assertTrue(stats["synced"])
        self.assertEqual({key: stats["counters"][key] for key in ("lost", "corrupt", "unknown", "boots")},
                         {"lost": 0, "corrupt": 0, "unknown": 0, "boots": 1})
        self.assertEqual(stats["counters"]["skipped_values"], 0)
        self.assertEqual(stats["counters"]["imu"], len(self.imu))
        self.assertGreaterEqual(len(self.imu), 25 * 59)                     # 25/s from the 1st PONG
        self.assertAlmostEqual(self.obd[0][0], 1.2, delta=0.1)              # the 1st PING went at 0 s
        self.assertAlmostEqual(len(self.obd), 5 * (60 - 1.2), delta=2)
        gaps = [later[0] - earlier[0] for earlier, later in zip(self.imu, self.imu[1:])]
        self.assertAlmostEqual(min(gaps), 0.04, delta=1e-5)                 # the ESP32's own clock
        self.assertAlmostEqual(max(gaps), 0.04, delta=1e-5)

        self.assertEqual(stats["esp32"], {
            "protocol": 1,
            "uptime_s": 59.0,                           # the STATUS of each whole second
            "platform": "notebook",
            "imu_simulated": True,
            "reset_reason": "power_on",
            "imu_rate": 25,
            "obd_rate": 5,
            "imu_state": "ok",
            "obd_state": "reading",
            # the 8 PIDs, and 0x20 and 0x40, which announce the next blocks of the map
            "supported_pids": [0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x11, 0x20, 0x40, 0x42],
            "counters": {"dropped": 0, "rejected": 0, "imu_failures": 0,
                         "obd_sent": stats["esp32"]["counters"]["obd_sent"],
                         "obd_answered": stats["esp32"]["counters"]["obd_sent"],
                         "obd_timeouts": 0, "blocked": 0, "can_errors": 0, "bus_off": 0},
            "heap_min": 0,
            "firmware": f"fake-{esp32_decoder.VERSION}",
        })
        # 01 00, 01 20 and 01 40; then rpm and speed 5 times a second, the other 6 once a second
        self.assertAlmostEqual(stats["esp32"]["counters"]["obd_sent"], 3 + 16 * (59 - 1.05), delta=16)
        self.assertEqual(len(logs.records), 1)
        self.assertEqual(logs.records[0].levelname, "WARNING")

    def test_the_values_follow_the_60_s_course(self):
        with self.assertLogs("esp32"):
            self.run_for(60)

        self.assertTrue(all(set(sample) == OBD_KEYS for _, sample in self.obd))
        self.assertTrue(all(set(sample["age_ms"]) == OBD_KEYS - {"age_ms"} for _, sample in self.obd))
        self.assertLessEqual(max(sample["age_ms"]["rpm"] for _, sample in self.obd), 200)
        self.assertLessEqual(max(sample["age_ms"]["voltage"] for _, sample in self.obd), 1000)
        speeds = [sample["speed"] for _, sample in self.obd]
        self.assertEqual([speeds[0], speeds[-1]], [0, 0])                   # stopped, then stopped again
        self.assertIn(max(speeds), range(58, 62))                           # up to 60 km/h
        self.assertEqual(self.obd[0][1]["rpm"], 800.0)                      # idle
        self.assertGreater(max(sample["rpm"] for _, sample in self.obd), 2000)

        stopped = [sample for at, sample in self.imu if at < 5]
        self.assertAlmostEqual(sum(sample["az"] for sample in stopped) / len(stopped), 9.81, delta=0.05)
        self.assertGreater(max(sample["ax"] for _, sample in self.imu), 1.5)    # speeding up
        self.assertLess(min(sample["ax"] for _, sample in self.imu), -2.5)      # braking
        for _, sample in self.imu:
            for axis in ("ax", "ay", "az", "gx", "gy", "gz"):
                self.assertGreaterEqual(abs(sample[axis + "_peak"]), abs(sample[axis]))

    def test_obd_only_while_the_pi_pings(self):
        self.run_for(5, pings=False)
        self.assertEqual(self.decoder.stats()["esp32"]["obd_state"], "waiting_pi")
        with self.assertLogs("esp32"):                  # the warning goes when the Pi shows up
            self.run_for(10)                            # PINGs from 5 s to 14 s
            self.run_for(10, pings=False)
        self.assertAlmostEqual(self.obd[0][0], 5 + 1.2, delta=0.1)
        self.assertLess(self.obd[-1][0], 14 + 3)                            # 3 s after the last PING
        self.assertGreater(self.obd[-1][0], 14 + 3 - 0.25)
        self.assertGreater(self.imu[-1][0], 24.9)                           # the IMU goes on
        self.assertEqual(self.decoder.stats()["esp32"]["obd_state"], "waiting_pi")

    def test_two_fakes_draw_different_boots(self):
        boots = set()
        for seed in range(2):
            decoder = esp32_decoder.Decoder(self.clock)
            decoder.feed(fake_esp32.FakeEsp32(self.clock, random.Random(seed)).data_to_send())
            boots.add(decoder.stats()["boot"])
        self.assertEqual(len(boots), 2)


if __name__ == "__main__":
    unittest.main()
