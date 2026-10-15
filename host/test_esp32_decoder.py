"""Tests for the decoder, through its public calls and a fake clock (TESTES.md §3.2)."""

import json
import logging
import random
import statistics
import sys
import threading
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
        self.epoch = 1_790_000_000.0        # time() − monotonic(); chrony may step it

    def monotonic(self):
        return self.now_us / 1e6

    def time(self):
        return self.monotonic() + self.epoch

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

    def test_pings_keep_to_whole_seconds_whatever_the_call_rhythm(self):
        sent = 0
        for _ in range(144):                # calls from 0 s to 10.01 s, every 70 ms
            sent += bool(self.decoder.data_to_send())
            self.clock.advance(0.07)
        self.assertEqual(sent, 11)          # at 0 s and right after each of 1 s … 10 s

    def test_a_new_boot_gets_a_ping_at_once_without_moving_the_others(self):
        self.decoder.feed(status(boot=0xBEEF))
        self.assertEqual(self.decoder.data_to_send(), ping(0))
        self.clock.advance(0.3)
        self.decoder.feed(status(boot=0x1234))          # PROTOCOLO.md §6.2
        self.assertEqual(self.decoder.data_to_send(), ping(1))
        self.assertEqual(self.decoder.data_to_send(), b"")
        self.clock.advance(0.7)
        self.assertEqual(self.decoder.data_to_send(), ping(2))
        self.clock.advance(0.3)
        self.decoder.feed(status(boot=0x1234, seq=1))   # the same boot: nothing extra
        self.assertEqual(self.decoder.data_to_send(), b"")

    def test_eighth_ping_is_the_protocol_example(self):
        for _ in range(7):
            self.decoder.data_to_send()
            self.clock.advance(1.0)
        self.assertEqual(self.decoder.data_to_send(), PING_ON_WIRE)


def status_body(version=1, t_us=12_345_678, platform=1, sources=0x01, reset=5,
                imu_rate=25, obd_rate=5, imu_state=0, obd_state=3,
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
        self.assertEqual(self.decoder.feed(status(imu_rate=50, obd_rate=10)), [])
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
        stats["requested_rates"]["imu"] = 99
        self.assertEqual(self.decoder.stats()["counters"]["corrupt"], 0)
        self.assertEqual(self.decoder.stats()["requested_rates"]["imu"], 25)
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

    def test_a_message_over_255_bytes_is_corrupt_even_in_one_read(self):
        self.decoder.feed(esp32_decoder.encode_message(0x01, 0xBEEF, 0, status_body(firmware=b"x" * 200)))
        self.assertEqual(self.counters()["corrupt"], 1)
        self.assertIsNone(self.decoder.stats()["esp32"])

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
        # the CRC matched, so the header still counts: nothing was lost on the wire
        self.decoder.feed(status(seq=2))
        self.assertEqual(self.counters()["lost"], 0)

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
        self.assertEqual(self.counters(), {"lost": 0, "corrupt": 0, "unsynced": 2, "unknown": 0,
                                           "skipped_values": 0, "slow_pongs": 0, "imu": 0, "obd": 0,
                                           "empty": 0, "boots": 1})
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

    def test_a_repeated_or_earlier_seq_restarts_the_count_without_a_gap(self):
        self.feed_seqs([5, 5, 6, 2, 3, 5])
        self.assertEqual(self.counters()["lost"], 1)

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


def pong(seq_ping, t_us, boot=0xBEEF, seq=0):
    body = seq_ping.to_bytes(2, "little") + t_us.to_bytes(8, "little")
    return esp32_decoder.encode_message(0x04, boot, seq, body)


def ping_seq(data):
    """The seq of the PING among the messages in data, as the ESP32 reads it."""
    for frame in data.split(b"\x00")[:-1]:
        message = esp32_decoder.cobs_decode(frame)
        if message[0] == 0x81:
            return int.from_bytes(message[3:5], "little")
    raise AssertionError("no PING")


class Samples(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def sync(self, boot=0xBEEF, t_us=1_000_000):
        """A STATUS, then a PING answered at once: t_us lands on this instant."""
        self.decoder.feed(status(boot=boot))
        self.decoder.feed(pong(ping_seq(self.decoder.data_to_send()), t_us, boot=boot))

    def test_imu_example_from_the_protocol(self):
        self.sync()
        [(source, ts, sample)] = self.decoder.feed(IMU_ON_WIRE)
        self.assertEqual(source, "imu")
        self.assertAlmostEqual(ts, self.clock.time() + 0.5, delta=1e-6)     # t_us 1 500 000
        self.assertEqual(sample, {"ax": -0.62, "ay": 0.1, "az": 9.81, "gx": 0.5, "gy": -1.2, "gz": 12.3,
                                  "ax_peak": -0.8, "ay_peak": 0.25, "az_peak": 10.4,
                                  "gx_peak": 1.1, "gy_peak": -2.0, "gz_peak": 15.0})

    def test_obd_example_from_the_protocol(self):
        self.sync()
        [(source, ts, sample)] = self.decoder.feed(OBD_ON_WIRE)
        self.assertEqual(source, "obd")
        self.assertAlmostEqual(ts, self.clock.time() + 0.52, delta=1e-6)    # t_us 1 520 000
        self.assertEqual(sample, {"rpm": 1726.0, "speed": 60, "age_ms": {"rpm": 40, "speed": 120}})

    def test_the_eight_pids_with_their_formulas(self):
        self.sync()
        [(_, _, sample)] = self.decoder.feed(obd([
            (0x04, 900, "4D"),          # 77·100/255 = 30.196…
            (0x05, 900, "82"),          # 130 − 40
            (0x0B, 900, "23"),
            (0x0C, 100, "0C 80"),       # (256·12 + 128)/4
            (0x0D, 100, "3C"),
            (0x0F, 900, "47"),          # 71 − 40
            (0x11, 900, "24"),          # 36·100/255 = 14.117…
            (0x42, 65535, "37 14"),     # (256·55 + 20)/1000
        ]))
        self.assertEqual(sample, {
            "load": 30.2, "coolant": 90, "map": 35, "rpm": 800.0, "speed": 60, "iat": 31,
            "throttle": 14.1, "voltage": 14.1,
            "age_ms": {"load": 900, "coolant": 900, "map": 900, "rpm": 100, "speed": 100,
                       "iat": 900, "throttle": 900, "voltage": 65535},
        })
        self.assertEqual({key: type(value).__name__ for key, value in sample.items()}, {
            "load": "float", "coolant": "int", "map": "int", "rpm": "float", "speed": "int",
            "iat": "int", "throttle": "float", "voltage": "float", "age_ms": "dict"})

    def test_a_pid_without_formula_or_with_fewer_bytes_is_skipped_by_its_size(self):
        self.sync()
        [(_, _, sample)] = self.decoder.feed(obd([
            (0x0C, 50, "1A"),           # rpm needs A and B
            (0x0D, 50, "3C"),
            (0x10, 50, "01 F4"),        # MAF: not in the table
            (0x11, 50, "24"),
        ]))
        self.assertEqual(sample, {"speed": 60, "throttle": 14.1,
                                  "age_ms": {"speed": 50, "throttle": 50}})
        self.assertEqual(self.decoder.stats()["counters"]["skipped_values"], 2)

    def test_an_obd_message_with_no_value_left_gives_no_sample(self):
        self.sync()
        self.assertEqual(self.decoder.feed(obd([(0x10, 50, "01 F4")])), [])    # {} means "stopped" (§3.4)
        counters = self.decoder.stats()["counters"]
        self.assertEqual([counters["obd"], counters["skipped_values"]], [0, 1])

    def test_skipped_values_count_even_in_a_sample_dropped_as_unsynced(self):
        self.decoder.feed(obd([(0x10, 50, "01 F4"), (0x0D, 50, "3C")]))
        counters = self.decoder.stats()["counters"]
        self.assertEqual([counters["unsynced"], counters["skipped_values"]], [1, 1])

    def test_imu_pong_or_obd_too_short_for_its_fields_is_corrupt(self):
        self.sync()
        whole_value = obd([(0x0D, 50, "3C")])
        short = [
            esp32_decoder.encode_message(0x02, 0xBEEF, 1, IMU_BEFORE_COBS[5:-3]),
            esp32_decoder.encode_message(0x04, 0xBEEF, 2, bytes(9)),
            esp32_decoder.encode_message(0x03, 0xBEEF, 3, bytes(8)),
            # n = 2, but only one value follows; then a value cut in its raw bytes
            esp32_decoder.encode_message(0x03, 0xBEEF, 4, bytes(8) + b"\x02\x0D\x32\x00\x01\x3C"),
            esp32_decoder.encode_message(0x03, 0xBEEF, 5, bytes(8) + b"\x01\x0C\x32\x00\x02\x1A"),
        ]
        self.assertEqual(self.decoder.feed(b"".join(short)), [])
        self.assertEqual(self.decoder.stats()["counters"]["corrupt"], 5)
        self.assertEqual(len(self.decoder.feed(whole_value)), 1)

    def test_delivered_samples_are_counted_by_source(self):
        self.decoder.feed(IMU_ON_WIRE)
        self.sync()
        self.decoder.feed(IMU_ON_WIRE + OBD_ON_WIRE + IMU_ON_WIRE)
        counters = self.decoder.stats()["counters"]
        self.assertEqual([counters["imu"], counters["obd"], counters["unsynced"]], [2, 1, 1])

    def unsynced(self):
        return self.decoder.stats()["counters"]["unsynced"]

    def test_samples_wait_for_the_status_and_the_first_pair(self):
        self.assertEqual(self.decoder.feed(IMU_ON_WIRE + OBD_ON_WIRE), [])
        self.decoder.feed(pong(ping_seq(self.decoder.data_to_send()), 1_000_000))
        self.assertEqual(self.decoder.feed(IMU_ON_WIRE), [])        # a pair, but no STATUS yet
        self.assertEqual(self.unsynced(), 3)
        self.decoder.feed(status())
        self.assertEqual(len(self.decoder.feed(IMU_ON_WIRE)), 1)
        self.assertEqual(self.unsynced(), 3)

    def test_a_status_without_a_pair_is_not_enough(self):
        self.decoder.feed(status())
        self.assertEqual(self.decoder.feed(OBD_ON_WIRE), [])
        self.assertEqual(self.unsynced(), 1)

    def test_a_new_boot_needs_its_own_status_and_pair(self):
        self.sync(boot=0xBEEF)
        new_boot = esp32_decoder.encode_message(0x02, 0x1234, 0, IMU_BEFORE_COBS[5:-2])
        self.assertEqual(self.decoder.feed(new_boot), [])
        self.decoder.feed(status(boot=0x1234, seq=1))
        self.assertEqual(self.decoder.feed(new_boot), [])           # the pair was of the old boot
        self.clock.advance(1.0)
        self.decoder.feed(pong(ping_seq(self.decoder.data_to_send()), 3_000_000, boot=0x1234, seq=2))
        [(_, ts, _)] = self.decoder.feed(new_boot)
        self.assertAlmostEqual(ts, self.clock.time() - 1.5, delta=1e-6)    # t_us 1 500 000
        self.assertEqual(self.unsynced(), 2)


class EmptySamples(unittest.TestCase):
    """DECODIFICADOR.md §3.4: one {} when a source stops, so the Pi stops using its last sample."""

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)
        self.decoder.feed(status())
        self.decoder.feed(pong(ping_seq(self.decoder.data_to_send()), 1_000_000))
        self.start = self.clock.time()                  # where t_us 1 000 000 lands

    def empty(self):
        return self.decoder.stats()["counters"]["empty"]

    def test_the_obd_leaving_reading_gives_one_empty_sample_at_the_status_instant(self):
        self.assertEqual(len(self.decoder.feed(OBD_ON_WIRE)), 1)
        [(source, ts, sample)] = self.decoder.feed(status(t_us=3_000_000, obd_state=4))    # SEM_RESPOSTA
        self.assertEqual((source, sample), ("obd", {}))
        self.assertAlmostEqual(ts, self.start + 2.0, delta=1e-6)
        self.assertEqual(self.decoder.feed(status(t_us=4_000_000, obd_state=4)), [])
        self.assertEqual([self.decoder.stats()["counters"]["obd"], self.empty()], [1, 1])

    def test_the_imu_failing_gives_one_empty_imu_sample(self):
        self.decoder.feed(IMU_ON_WIRE + OBD_ON_WIRE)
        [(source, ts, sample)] = self.decoder.feed(status(t_us=3_000_000, imu_state=1))
        self.assertEqual((source, sample), ("imu", {}))
        self.assertAlmostEqual(ts, self.start + 2.0, delta=1e-6)
        self.assertEqual(self.decoder.feed(status(t_us=4_000_000, imu_state=1)), [])
        self.assertEqual(self.empty(), 1)

    def test_no_empty_sample_for_a_source_that_never_had_values(self):
        self.assertEqual(self.decoder.feed(status(obd_state=0, imu_state=1)), [])
        self.assertEqual(self.empty(), 0)

    def test_after_a_reboot_the_empty_waits_for_the_first_synced_status_of_the_new_boot(self):
        self.decoder.feed(OBD_ON_WIRE)
        self.assertEqual(self.decoder.feed(status(boot=0x1234, t_us=1_000, obd_state=0)), [])
        self.decoder.feed(pong(ping_seq(self.decoder.data_to_send()), 2_000, boot=0x1234, seq=1))
        self.clock.advance(1.0)
        [(source, ts, sample)] = self.decoder.feed(status(boot=0x1234, seq=2, t_us=1_002_000, obd_state=1))
        self.assertEqual((source, sample), ("obd", {}))
        self.assertAlmostEqual(ts, self.clock.time(), delta=1e-6)

    def test_3_s_of_silence_gives_one_empty_sample_per_source_at_the_pi_time(self):
        self.decoder.feed(IMU_ON_WIRE + OBD_ON_WIRE)
        self.clock.advance(2.9)
        self.assertEqual(self.decoder.feed(b""), [])
        self.clock.advance(0.1)
        samples = self.decoder.feed(b"")
        self.assertEqual([(source, sample) for source, _, sample in samples], [("imu", {}), ("obd", {})])
        for _, ts, _ in samples:
            self.assertAlmostEqual(ts, self.clock.time(), delta=1e-6)
        self.clock.advance(5.0)
        self.assertEqual(self.decoder.feed(b""), [])
        self.assertEqual(self.empty(), 2)

    def test_a_source_stopped_by_its_status_gets_no_second_empty_from_the_silence(self):
        self.decoder.feed(IMU_ON_WIRE + OBD_ON_WIRE)
        self.decoder.feed(status(obd_state=4))
        self.clock.advance(3.0)
        self.assertEqual([source for source, _, _ in self.decoder.feed(b"")], ["imu"])
        self.assertEqual(self.empty(), 2)

    def test_the_samples_come_back_and_the_next_stop_gives_another_empty(self):
        self.decoder.feed(OBD_ON_WIRE)
        self.decoder.feed(status(obd_state=4))
        self.decoder.feed(status(obd_state=3))
        self.assertEqual(len(self.decoder.feed(OBD_ON_WIRE)), 1)
        self.assertEqual(len(self.decoder.feed(status(obd_state=4))), 1)
        self.assertEqual(self.empty(), 2)


class UnknownVersion(unittest.TestCase):
    """PROTOCOLO.md §6.2: an old firmware in the car with a newer decoder on the Pi."""

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def sync(self, boot, seq):
        self.decoder.feed(pong(ping_seq(self.decoder.data_to_send()), 1_000_000, boot=boot, seq=seq))

    def test_one_error_per_boot_and_no_sample_of_that_boot(self):
        with self.assertLogs(level="ERROR") as logs:
            self.decoder.feed(status(version=2) + status(version=2, seq=1))
            self.sync(0xBEEF, seq=2)
            self.assertEqual(self.decoder.feed(IMU_ON_WIRE + OBD_ON_WIRE + status(version=2, seq=3)), [])
        self.assertEqual(len(logs.records), 1)
        stats = self.decoder.stats()
        self.assertEqual([stats["incompatible"], stats["synced"], stats["esp32"]], [True, False, None])
        self.assertEqual([stats["counters"]["unsynced"], stats["counters"]["corrupt"]], [2, 0])

    def test_the_pings_go_on(self):
        with self.assertLogs(level="ERROR"):
            self.decoder.feed(status(version=2))
        sent = [self.decoder.data_to_send()]
        self.clock.advance(1.0)
        sent.append(self.decoder.data_to_send())
        self.assertEqual(sent, [ping(0), ping(1)])

    def test_only_the_version_byte_is_read_and_each_new_boot_starts_over(self):
        with self.assertLogs(level="ERROR") as logs:
            self.decoder.feed(esp32_decoder.encode_message(0x01, 0xBEEF, 0, b"\x02"))
            self.assertTrue(self.decoder.stats()["incompatible"])
            self.decoder.feed(status(boot=0x1234))
            self.sync(0x1234, seq=1)
            self.assertFalse(self.decoder.stats()["incompatible"])
            imu = esp32_decoder.encode_message(0x02, 0x1234, 2, IMU_BEFORE_COBS[5:-2])
            self.assertEqual(len(self.decoder.feed(imu)), 1)
            self.decoder.feed(esp32_decoder.encode_message(0x01, 0x5678, 0, b"\x07" + bytes(200)))
        self.assertEqual(len(logs.records), 2)
        self.assertEqual(self.decoder.stats()["counters"]["corrupt"], 0)

    def test_a_status_without_even_the_version_byte_is_corrupt(self):
        self.decoder.feed(esp32_decoder.encode_message(0x01, 0xBEEF, 0))
        stats = self.decoder.stats()
        self.assertEqual([stats["counters"]["corrupt"], stats["incompatible"]], [1, False])


def taxas(seq, imu, obd):
    return esp32_decoder.encode_message(0x82, 0, seq, bytes([imu, obd]))


class Rates(unittest.TestCase):
    """PROTOCOLO.md §6.4: the Pi asks again at each STATUS until the ESP32 uses its rates."""

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def requested(self):
        return self.decoder.stats()["requested_rates"]

    def test_the_default_rates_until_set_rates_and_each_field_alone(self):
        self.assertEqual(self.requested(), {"imu": 25, "obd": 5})
        self.decoder.set_rates(imu=100)
        self.decoder.set_rates(obd=1)
        self.assertEqual(self.requested(), {"imu": 100, "obd": 1})

    def test_a_rate_outside_the_list_raises_and_changes_nothing(self):
        for imu, obd in ((30, None), (None, 3), (50, 0), (25.0, None), (None, "5")):
            with self.subTest(imu=imu, obd=obd), self.assertRaises(ValueError):
                self.decoder.set_rates(imu=imu, obd=obd)
        self.assertEqual(self.requested(), {"imu": 25, "obd": 5})

    def test_one_taxas_after_each_status_with_other_rates_always_with_both_values(self):
        self.decoder.set_rates(imu=50)
        self.decoder.feed(status(imu_rate=25, obd_rate=5))
        self.assertEqual(self.decoder.data_to_send(), ping(0) + taxas(1, 50, 5))
        self.clock.advance(0.5)
        self.assertEqual(self.decoder.data_to_send(), b"")         # nothing until the next STATUS
        self.decoder.feed(status(seq=1, imu_rate=25, obd_rate=5))  # the TAXAS was lost on the wire
        self.assertEqual(self.decoder.data_to_send(), taxas(2, 50, 5))
        self.decoder.feed(status(seq=2, imu_rate=50, obd_rate=5))  # now in use
        self.clock.advance(0.5)
        self.assertEqual(self.decoder.data_to_send(), ping(3))

    def test_no_taxas_while_the_esp32_uses_the_requested_rates(self):
        self.decoder.feed(status(imu_rate=25, obd_rate=5))
        self.assertEqual(self.decoder.data_to_send(), ping(0))


class Threads(unittest.TestCase):
    """stats() and set_rates() from any thread while the reader feeds (DECODIFICADOR.md §2.5)."""

    def test_stats_is_whole_while_the_reader_feeds_new_boots(self):
        clock = FakeClock()
        decoder = esp32_decoder.Decoder(clock)
        done = threading.Event()
        torn = []

        def health():
            rng = random.Random(18)
            while not done.is_set():
                stats = decoder.stats()
                if stats["synced"] and (stats["esp32"] is None or stats["sync"]["pairs"] == 0):
                    torn.append(stats)
                decoder.set_rates(imu=rng.choice((100, 50, 25, 20, 10)), obd=rng.choice((1, 2, 5, 10)))

        switch = sys.getswitchinterval()
        sys.setswitchinterval(1e-6)                     # hand the GIL over as often as it can
        self.addCleanup(sys.setswitchinterval, switch)
        thread = threading.Thread(target=health)
        thread.start()
        try:
            for boot in range(1, 3000):                 # each boot: STATUS, then its pair at once
                decoder.feed(status(boot=boot))
                decoder.feed(pong(ping_seq(decoder.data_to_send()), 1_000_000, boot=boot, seq=1))
        finally:
            done.set()
            thread.join()
        self.assertEqual(torn, [])
        self.assertIn(decoder.stats()["requested_rates"]["imu"], (100, 50, 25, 20, 10))


class Pongs(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)
        self.decoder.feed(status())

    def answer(self, after, seq_ping=None):
        """Sends the PING due now and feeds its PONG `after` seconds later."""
        sent = self.decoder.data_to_send()
        self.clock.advance(after)
        self.decoder.feed(pong(ping_seq(sent) if seq_ping is None else seq_ping, 1_000_000))

    def synced(self):
        return self.decoder.stats()["synced"]

    def test_a_pong_is_matched_to_its_ping_by_seq(self):
        self.assertFalse(self.synced())
        self.answer(0.002, seq_ping=5)
        self.assertFalse(self.synced())
        self.decoder.feed(pong(0, 1_000_000))
        self.assertTrue(self.synced())

    def test_a_pong_3_s_after_its_ping_is_ignored(self):
        self.answer(3.0)
        self.assertFalse(self.synced())
        self.assertEqual(self.decoder.stats()["counters"]["slow_pongs"], 0)

    def test_a_round_trip_over_20_ms_is_a_slow_pong_and_gives_no_pair(self):
        self.answer(0.0201)
        self.assertFalse(self.synced())
        self.assertEqual(self.decoder.stats()["counters"]["slow_pongs"], 1)
        self.clock.advance(1.0)
        self.answer(0.0199)
        self.assertTrue(self.synced())
        self.assertEqual(self.decoder.stats()["counters"]["slow_pongs"], 1)


class Esp32Peer:
    """The ESP32 at the other end of the line, with its own clock, which may run fast."""

    def __init__(self, clock, decoder, drift_ppm=0.0):
        self.clock, self.decoder = clock, decoder
        self.boot_at_us = clock.now_us - 5_000_000      # it booted 5 s ago
        self.rate = 1 + drift_ppm / 1e6
        self.seq = 0
        self.rtts = []
        decoder.feed(self.message(0x01, status_body()))

    def t_us(self):
        return round((self.clock.now_us - self.boot_at_us) * self.rate)

    def message(self, msg_type, body):
        self.seq += 1
        return esp32_decoder.encode_message(msg_type, 0xBEEF, self.seq - 1, body)

    def answer_ping(self, up, down):
        """The PING due now reaches the ESP32 `up` s later, and its PONG the Pi `down` s after that."""
        sent = self.decoder.data_to_send()
        self.clock.advance(up)
        t_us = self.t_us()
        self.clock.advance(down)
        body = ping_seq(sent).to_bytes(2, "little") + t_us.to_bytes(8, "little")
        self.decoder.feed(self.message(0x04, body))
        if up + down <= 0.020:
            self.rtts.append(up + down)

    def run(self, seconds, delays=lambda: (0.001, 0.001)):
        """A PING every second, each answered with the (up, down) delays given."""
        for _ in range(seconds):
            start_us = self.clock.now_us
            self.answer_ping(*delays())
            self.clock.advance((start_us + 1_000_000 - self.clock.now_us) / 1e6)  # the next PING is due

    def error_ms(self):
        """How far from the truth the decoder puts a sample stamped now."""
        [(_, ts, _)] = self.decoder.feed(self.message(0x02, self.t_us().to_bytes(8, "little") + bytes(24)))
        return (ts - self.clock.time()) * 1000


class TimeSync(unittest.TestCase):

    def setUp(self):
        self.clock = FakeClock()
        self.decoder = esp32_decoder.Decoder(self.clock)

    def sync_stats(self):
        return self.decoder.stats()["sync"]

    def test_no_pair_yet(self):
        Esp32Peer(self.clock, self.decoder)
        self.assertEqual(self.sync_stats(), {"pairs": 0, "rtt_ms": None, "age_s": None, "drift_ppm": None})

    def test_keeps_the_fastest_pair_of_each_10_s(self):
        esp32 = Esp32Peer(self.clock, self.decoder)        # t_us 5 s: three PINGs in the same 10 s
        esp32.answer_ping(0.001, 0.015)                     # off by 7 ms
        self.clock.advance(1.0 - 0.016)
        esp32.answer_ping(0.002, 0.002)                     # exact
        self.clock.advance(1.0 - 0.004)
        esp32.answer_ping(0.015, 0.001)                     # off by 7 ms the other way
        self.assertAlmostEqual(esp32.error_ms(), 0.0, delta=0.001)
        self.clock.advance(0.5)
        self.assertEqual(self.sync_stats(), {"pairs": 1, "rtt_ms": 16.0, "age_s": 0.5, "drift_ppm": None})

    def test_the_line_corrects_the_drift_of_the_esp32_crystal(self):
        esp32 = Esp32Peer(self.clock, self.decoder, drift_ppm=200)
        esp32.run(20)
        self.assertIsNone(self.sync_stats()["drift_ppm"])      # pairs span under 30 s: offset only
        esp32.run(160)
        self.assertEqual(self.sync_stats()["pairs"], 12)
        self.assertAlmostEqual(self.sync_stats()["drift_ppm"], 200, delta=0.5)
        self.clock.advance(60)                                  # no more PONGs: the line goes on
        # the offset alone would be 200 ppm × 60 s = 12 ms off by now
        self.assertAlmostEqual(esp32.error_ms(), 0.0, delta=0.1)

    def test_within_half_the_round_trip_with_unequal_and_varying_delays(self):
        """TESTES.md §7.1: the ESP32 stamps the PONG somewhere between t1 and t2."""
        rng = random.Random(17)

        def delays():
            if rng.random() < 0.1:                  # the reader paused: a slow pong
                return rng.uniform(0.0, 0.002), rng.uniform(0.025, 0.4)
            rtt = rng.uniform(0.001, 0.019)
            up = rtt * rng.random()                 # all on the way out, all on the way back, or between
            return up, rtt - up

        esp32 = Esp32Peer(self.clock, self.decoder, drift_ppm=-40)
        errors = []
        for _ in range(300):
            esp32.run(1, delays)
            errors.append(esp32.error_ms())
        self.assertLessEqual(max(map(abs, errors)), max(esp32.rtts) / 2 * 1000)
        # The fastest pair of each 10 s keeps it much tighter: about 0.6 ms, against 2.4 ms
        # if each PONG simply replaced the last (10 seeds measured).
        self.assertLess(statistics.mean(map(abs, errors)), 1.2)
        self.assertGreater(self.decoder.stats()["counters"]["slow_pongs"], 0)

    def test_the_wall_clock_is_read_at_each_delivery(self):
        esp32 = Esp32Peer(self.clock, self.decoder)
        esp32.run(1)
        self.clock.epoch += 3600.25                 # chrony steps the Pi's clock when the GPS locks
        self.assertAlmostEqual(esp32.error_ms(), 0.0, delta=0.001)


def obd(values, t_us=2_000_000, boot=0xBEEF, seq=1):
    """An OBD message from (pid, age_ms, raw bytes in hex) triples."""
    body = t_us.to_bytes(8, "little") + bytes([len(values)])
    for pid, age, raw in values:
        raw = bytes.fromhex(raw)
        body += bytes([pid]) + age.to_bytes(2, "little") + bytes([len(raw)]) + raw
    return esp32_decoder.encode_message(0x03, boot, seq, body)


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

    def test_random_samples_of_a_synced_boot_come_out_as_plain_json(self):
        esp32 = Esp32Peer(self.clock, self.decoder)
        esp32.run(1)
        rng = random.Random(17)
        samples = []
        for _ in range(5000):
            msg_type = rng.choice((0x02, 0x03, 0x04))
            if msg_type == 0x03 and rng.random() < 0.8:     # well formed, with odd PIDs and sizes
                values = [(rng.choice((0x04, 0x0C, 0x0D, 0x42, rng.randrange(256))), rng.randrange(65536),
                           rng.randbytes(rng.randrange(0, 6)).hex()) for _ in range(rng.randrange(0, 12))]
                samples += self.decoder.feed(obd(values, t_us=rng.randrange(2 ** 64), seq=esp32.seq))
                esp32.seq += 1
            else:
                samples += self.decoder.feed(esp32.message(msg_type, rng.randbytes(rng.randrange(0, 250))))
        self.assertGreater(len(samples), 1000)
        for source, ts, sample in samples:
            self.assertIn(source, ("imu", "obd"))
            self.assertIsInstance(ts, float)
            self.assertNotIn("null", json.dumps(sample, allow_nan=False))


if __name__ == "__main__":
    unittest.main()
