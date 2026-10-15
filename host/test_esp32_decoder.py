"""Tests for the decoder, through its public calls and a fake clock (TESTES.md §3.2)."""

import logging
import random
import unittest

import esp32_decoder


def wire(text):
    return bytes.fromhex(text)


# PROTOCOLO.md §8, byte by byte.
IMU_BEFORE_COBS = wire(
    "02 EF BE 01 00 60 E3 16 00 00 00 00 00 C2 FF 0A 00 D5 03 05 00 F4 FF 7B 00"
    "B0 FF 19 00 10 04 0B 00 EC FF 96 00 C8 15")
IMU_ON_WIRE = wire(
    "05 02 EF BE 01 04 60 E3 16 01 01 01 01 04 C2 FF 0A 04 D5 03 05 04 F4 FF 7B"
    "04 B0 FF 19 04 10 04 0B 04 EC FF 96 03 C8 15 00")
OBD_BEFORE_COBS = wire(
    "03 EF BE 02 00 80 31 17 00 00 00 00 00 02 0C 28 00 02 1A F8 0D 78 00 01 3C A4 0D")
OBD_ON_WIRE = wire(
    "05 03 EF BE 02 04 80 31 17 01 01 01 01 04 02 0C 28 06 02 1A F8 0D 78 05 01 3C"
    "A4 0D 00")
PING_ON_WIRE = wire("02 81 01 02 07 02 1A 01 00")


class Framing(unittest.TestCase):

    def test_crc_is_ccitt_false(self):
        self.assertEqual(esp32_decoder.crc16(b"123456789"), 0x29B1)

    def test_messages_match_the_protocol_examples(self):
        # header | body, then CRC, COBS and the 0x00 at the end
        self.assertEqual(
            esp32_decoder.encode_message(0x02, 0xBEEF, 1, IMU_BEFORE_COBS[5:-2]), IMU_ON_WIRE)
        self.assertEqual(
            esp32_decoder.encode_message(0x03, 0xBEEF, 2, OBD_BEFORE_COBS[5:-2]), OBD_ON_WIRE)
        self.assertEqual(esp32_decoder.encode_message(0x81, 0, 7), PING_ON_WIRE)


class FakeClock:
    """Stands in for the time module: the test decides every instant.

    It counts whole microseconds, so a hundred steps of 0.01 s land exactly on 1 s.
    """

    def __init__(self):
        self.now_us = 1000 * 1_000_000

    def monotonic(self):
        return self.now_us / 1e6

    def time(self):
        return self.monotonic() + 1_790_000_000.0

    def advance(self, seconds):
        self.now_us += round(seconds * 1e6)


def ping(seq):
    return esp32_decoder.encode_message(0x81, 0, seq)


class Pings(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def test_first_ping_goes_on_the_first_call_even_with_nobody_on_the_line(self):
        self.assertEqual(self.decoder.data_to_send(), ping(0))

    def test_one_ping_per_second(self):
        self.decoder.data_to_send()
        sent = []
        for _ in range(30):                 # 3 s, called every 100 ms
            self.clock.advance(0.1)
            sent.append(self.decoder.data_to_send())
        self.assertEqual([data for data in sent if data], [ping(1), ping(2), ping(3)])
        self.assertEqual([i for i, data in enumerate(sent) if data], [9, 19, 29])

    def test_eighth_ping_is_the_protocol_example(self):
        for _ in range(7):
            self.decoder.data_to_send()
            self.clock.advance(1.0)
        self.assertEqual(self.decoder.data_to_send(), PING_ON_WIRE)


def status_body(version=1, t_us=12_345_678, platform=1, sources=0x01, reset=5,
                imu_rate=50, obd_rate=10, imu_state=0, obd_state=3,
                pids=bytes.fromhex("98 3B 80 11  00 00 00 00  40 00 00 00"),
                counters=(1, 2, 3, 4, 5, 6, 7, 8, 9), heap_min=123_456,
                firmware=b"0.4.0-a58af37"):
    """A STATUS body laid out by hand from PROTOCOLO.md §3.1."""
    return (bytes([version]) + t_us.to_bytes(8, "little")
            + bytes([platform, sources, reset, imu_rate, obd_rate, imu_state, obd_state])
            + pids + b"".join(c.to_bytes(4, "little") for c in counters)
            + heap_min.to_bytes(4, "little") + firmware)


def status(boot=0xBEEF, seq=0, **fields):
    return esp32_decoder.encode_message(0x01, boot, seq, status_body(**fields))


class Status(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def test_status_shows_up_in_stats_with_english_names(self):
        self.assertEqual(self.decoder.feed(status()), [])
        self.assertEqual(self.decoder.stats()["esp32"], {
            "protocol": 1,
            "uptime_s": 12.345678,
            "platform": "esp32",
            "imu_simulated": True,
            "reset_reason": "watchdog",
            "imu_rate": 50,
            "obd_rate": 10,
            "imu_state": "ok",
            "obd_state": "reading",
            # 0x98 = 1001 1000: PIDs 0x01, 0x04, 0x05; 0x3B = 0011 1011: 0x0B, 0x0C, 0x0D, 0x0F, 0x10;
            # 0x80: 0x11; 0x11 = 0001 0001: 0x1C, 0x20; then 0x40 in the 3rd block: 0x42
            "supported_pids": [0x01, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1C, 0x20, 0x42],
            "counters": {"dropped": 1, "rejected": 2, "imu_failures": 3, "obd_sent": 4,
                         "obd_answered": 5, "obd_timeouts": 6, "blocked": 7, "can_errors": 8,
                         "bus_off": 9},
            "heap_min": 123_456,
            "firmware": "0.4.0-a58af37",
        })

    def test_codes_outside_the_tables_are_unknown(self):
        self.decoder.feed(status(platform=2, reset=7, imu_state=2, obd_state=9))
        esp32 = self.decoder.stats()["esp32"]
        self.assertEqual(
            [esp32["platform"], esp32["reset_reason"], esp32["imu_state"], esp32["obd_state"]],
            ["unknown"] * 4)


class Link(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def test_stats_is_a_new_dict_each_call(self):
        self.decoder.feed(status())
        stats = self.decoder.stats()
        stats["counters"]["corrupt"] = 99
        stats["esp32"]["counters"]["blocked"] = 99
        stats["esp32"]["supported_pids"].clear()
        self.assertEqual(self.decoder.stats()["counters"]["corrupt"], 0)
        self.assertEqual(self.decoder.stats()["esp32"]["counters"]["blocked"], 7)
        self.assertEqual(len(self.decoder.stats()["esp32"]["supported_pids"]), 12)

    def test_nothing_before_the_first_message(self):
        stats = self.decoder.stats()
        self.assertEqual([stats["connected"], stats["boot"], stats["esp32"]], [False, None, None])

    def test_connected_while_valid_messages_arrive_within_3_s(self):
        self.decoder.feed(status(boot=0x1234))
        self.assertEqual(self.decoder.stats()["boot"], 0x1234)
        self.clock.advance(2.999)
        self.assertTrue(self.decoder.stats()["connected"])
        self.clock.advance(0.001)
        self.assertFalse(self.decoder.stats()["connected"])


class BadBytes(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def counters(self):
        return self.decoder.stats()["counters"]

    def test_wrong_crc_is_corrupt(self):
        data = b"\x01\xEF\xBE\x00\x00" + status_body()
        wrong_crc = (esp32_decoder.crc16(data) ^ 0x0001).to_bytes(2, "little")
        self.decoder.feed(esp32_decoder.cobs_encode(data + wrong_crc) + b"\x00")
        self.assertEqual(self.counters()["corrupt"], 1)
        self.assertIsNone(self.decoder.stats()["esp32"])

    def test_invalid_cobs_is_corrupt(self):
        self.decoder.feed(b"\x09\x01\x02\x00")     # the code promises 8 bytes, only 2 follow
        self.assertEqual(self.counters()["corrupt"], 1)

    def test_fewer_than_7_bytes_is_corrupt(self):
        self.decoder.feed(esp32_decoder.cobs_encode(b"\x01\xEF\xBE\x00\x00\x00") + b"\x00")
        self.assertEqual(self.counters()["corrupt"], 1)

    def test_realigns_on_the_next_zero_after_garbage(self):
        # the reader opened the port in the middle of a message
        self.decoder.feed(IMU_ON_WIRE[17:] + status(seq=3))
        self.assertEqual(self.counters()["corrupt"], 1)
        self.assertEqual(self.decoder.stats()["esp32"]["obd_state"], "reading")

    def test_a_message_split_across_reads_is_put_back_together(self):
        message = status()
        for i in range(len(message)):
            self.decoder.feed(message[i:i + 1])
        self.assertEqual(self.decoder.stats()["esp32"]["firmware"], "0.4.0-a58af37")
        self.assertEqual(self.counters()["corrupt"], 0)

    def test_more_than_258_bytes_without_a_zero_is_dropped_as_one_corrupt(self):
        self.decoder.feed(b"\x55" * 259)
        self.assertEqual(self.counters()["corrupt"], 1)
        self.decoder.feed(b"\x55" * 300 + b"\x00" + status())
        self.assertEqual(self.counters()["corrupt"], 1)
        self.assertIsNotNone(self.decoder.stats()["esp32"])

    def test_unknown_type_is_counted_and_ignored(self):
        self.decoder.feed(esp32_decoder.encode_message(0x7E, 0xBEEF, 0, b"\x01\x02"))
        self.assertEqual(self.counters()["unknown"], 1)
        self.assertEqual(self.counters()["corrupt"], 0)
        self.assertTrue(self.decoder.stats()["connected"])

    def test_status_or_log_too_short_for_its_fields_is_corrupt(self):
        self.decoder.feed(esp32_decoder.encode_message(0x01, 0xBEEF, 0, status_body()[:67]))
        self.decoder.feed(esp32_decoder.encode_message(0x05, 0xBEEF, 1, bytes(8)))
        self.assertEqual(self.counters()["corrupt"], 2)
        self.assertIsNone(self.decoder.stats()["esp32"])

    def test_back_to_back_zeros_are_not_messages(self):
        self.decoder.feed(b"\x00\x00" + status() + b"\x00")
        self.assertEqual(self.counters()["corrupt"], 0)


class Sequence(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def counters(self):
        return self.decoder.stats()["counters"]

    def feed_seqs(self, seqs, boot=0xBEEF):
        for seq in seqs:
            self.decoder.feed(status(boot=boot, seq=seq))
            self.clock.advance(0.04)

    def test_protocol_examples_arrive_whole(self):
        self.decoder.feed(IMU_ON_WIRE + OBD_ON_WIRE)
        self.assertEqual(self.counters(), {"lost": 0, "corrupt": 0, "unknown": 0, "boots": 1})
        self.assertEqual(self.decoder.stats()["boot"], 0xBEEF)

    def test_gaps_in_seq_are_lost_messages(self):
        self.feed_seqs([0, 1, 4, 5, 9])
        self.assertEqual(self.counters()["lost"], 2 + 3)

    def test_seq_wraps_after_65535(self):
        self.feed_seqs([65534, 65535, 0, 1])
        self.assertEqual(self.counters()["lost"], 0)

    def test_after_3_s_of_silence_the_sequence_restarts_without_counting(self):
        self.feed_seqs([10, 11])
        self.clock.advance(3.0)
        self.feed_seqs([200, 201])
        self.assertEqual(self.counters()["lost"], 0)

    def test_short_silence_still_counts_the_gap(self):
        self.feed_seqs([10, 11])
        self.clock.advance(2.9)
        self.feed_seqs([200])
        self.assertEqual(self.counters()["lost"], 188)

    def test_new_boot_restarts_the_sequence_and_forgets_the_old_status(self):
        self.feed_seqs([0, 1, 2], boot=0xBEEF)
        self.decoder.feed(esp32_decoder.encode_message(0x02, 0x1234, 0, IMU_BEFORE_COBS[5:-2]))
        stats = self.decoder.stats()
        self.assertEqual(stats["boot"], 0x1234)
        self.assertIsNone(stats["esp32"])
        self.assertEqual([stats["counters"]["lost"], stats["counters"]["boots"]], [0, 2])

    def test_corrupt_messages_do_not_move_the_sequence(self):
        self.feed_seqs([0, 1])
        self.decoder.feed(b"\x05\x01\x02\x03\x04\x00")
        self.feed_seqs([2])
        self.assertEqual(self.counters()["lost"], 0)


def log(level, text, seq=0):
    body = (2_000_000).to_bytes(8, "little") + bytes([level]) + text.encode()
    return esp32_decoder.encode_message(0x05, 0xBEEF, seq, body)


class EspLog(unittest.TestCase):

    def setUp(self):
        self.decoder = esp32_decoder.Decoder(FakeClock())

    def test_level_1_is_an_error(self):
        with self.assertLogs("esp32", "WARNING") as logs:
            self.decoder.feed(log(1, "twai: bus-off"))
        self.assertEqual(logs.output, ["ERROR:esp32:twai: bus-off"])

    def test_level_2_and_any_other_are_warnings(self):
        with self.assertLogs("esp32", "WARNING") as logs:
            self.decoder.feed(log(2, "imu: read failed") + log(7, "odd level, az 9.81 m/s²", seq=1))
        self.assertEqual(logs.output, ["WARNING:esp32:imu: read failed",
                                       "WARNING:esp32:odd level, az 9.81 m/s²"])

    def test_feed_returns_no_samples_for_a_log(self):
        with self.assertLogs("esp32"):
            self.assertEqual(self.decoder.feed(log(2, "x")), [])


class Garbage(unittest.TestCase):
    """Bytes from the line never raise an exception in feed() (DECODIFICADOR.md §3.9)."""

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)
        logging.disable(logging.CRITICAL)
        self.addCleanup(logging.disable, logging.NOTSET)

    def test_random_bytes(self):
        rng = random.Random(16)
        for _ in range(2000):
            zeros = rng.choice((0.0, 0.01, 0.5))       # from overlong runs to empty frames
            noise = bytes(0 if rng.random() < zeros else rng.randrange(256)
                          for _ in range(rng.randrange(1, 400)))
            self.decoder.feed(noise)
            self.clock.advance(rng.choice((0.0, 0.01, 4.0)))
        self.assertGreater(self.decoder.stats()["counters"]["corrupt"], 0)

    def test_valid_frames_with_random_bodies(self):
        rng = random.Random(16)
        for seq in range(5000):
            msg_type = rng.choice((0x01, 0x02, 0x03, 0x04, 0x05, 0x81, rng.randrange(256)))
            body = rng.randbytes(rng.randrange(0, 250))
            self.decoder.feed(esp32_decoder.encode_message(msg_type, rng.randrange(65536), seq, body))
            self.decoder.stats()


if __name__ == "__main__":
    unittest.main()
