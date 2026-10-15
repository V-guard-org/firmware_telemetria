"""Quick test: the fake ESP32 wired to the Decoder, without a pty, on the fake clock (TESTES.md §3.2)."""

import random
import unittest

import esp32_decoder
import fake_esp32
from test_esp32_decoder import FakeClock


class FakeEsp32WithDecoder(unittest.TestCase):

    def test_60_s_of_status_without_corrupt_or_lost(self):
        clock = FakeClock()
        esp32 = fake_esp32.FakeEsp32(clock, random.Random(16))
        decoder = esp32_decoder.Decoder(clock)

        with self.assertLogs("esp32", "WARNING") as logs:
            for _ in range(6000):                       # 60 s in steps of 10 ms
                esp32.feed(decoder.data_to_send())
                decoder.feed(esp32.data_to_send())
                clock.advance(0.01)

        stats = decoder.stats()
        self.assertTrue(stats["connected"])
        self.assertIn(stats["boot"], range(1, 65536))
        self.assertEqual(stats["counters"], {"lost": 0, "corrupt": 0, "unknown": 0, "boots": 1})
        self.assertEqual(stats["esp32"], {
            "protocol": 1,
            "uptime_s": 59.0,                           # the STATUS of each whole second
            "platform": "notebook",
            "imu_simulated": True,
            "reset_reason": "power_on",
            "imu_rate": 25,
            "obd_rate": 5,
            "imu_state": "ok",
            "obd_state": "waiting_pi",
            "supported_pids": [],
            "counters": {"dropped": 0, "rejected": 0, "imu_failures": 0, "obd_sent": 0,
                         "obd_answered": 0, "obd_timeouts": 0, "blocked": 0, "can_errors": 0,
                         "bus_off": 0},
            "heap_min": 0,
            "firmware": f"fake-{esp32_decoder.VERSION}",
        })
        self.assertEqual(len(logs.records), 1)
        self.assertEqual(logs.records[0].levelname, "WARNING")

    def test_two_fakes_draw_different_boots(self):
        clock = FakeClock()
        boots = set()
        for seed in range(2):
            decoder = esp32_decoder.Decoder(clock)
            with self.assertLogs("esp32"):
                decoder.feed(fake_esp32.FakeEsp32(clock, random.Random(seed)).data_to_send())
            boots.add(decoder.stats()["boot"])
        self.assertEqual(len(boots), 2)


if __name__ == "__main__":
    unittest.main()
