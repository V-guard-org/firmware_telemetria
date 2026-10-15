// Moldura do protocolo (costura 3 da spec da fase 1): CRC, COBS e os exemplos
// byte a byte do docs/PROTOCOLO.md §8. Roda no notebook com: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "protocol.h"

using namespace vguard;

void setUp() {}
void tearDown() {}

static void test_crc_of_the_check_string() {
    const char* text = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(reinterpret_cast<const uint8_t*>(text), 9));
}

// Os bytes do PROTOCOLO.md §8, como estão no documento.
static const uint8_t kPingWire[] = {0x02, 0x81, 0x01, 0x02, 0x07, 0x02, 0x1A, 0x01, 0x00};

static const uint8_t kImuWire[] = {
    0x05, 0x02, 0xEF, 0xBE, 0x01, 0x04, 0x60, 0xE3, 0x16, 0x01, 0x01, 0x01, 0x01, 0x04,
    0xC2, 0xFF, 0x0A, 0x04, 0xD5, 0x03, 0x05, 0x04, 0xF4, 0xFF, 0x7B, 0x04, 0xB0, 0xFF,
    0x19, 0x04, 0x10, 0x04, 0x0B, 0x04, 0xEC, 0xFF, 0x96, 0x03, 0xC8, 0x15, 0x00};

static void test_imu_example_encodes_byte_by_byte() {
    // Boot 0xBEEF, seq 1, t_us 1 500 000; médias e picos do §8, nas unidades do cabo.
    ImuSampleBody sample{};
    sample.t_us = 1'500'000;
    const int16_t mean[6] = {-62, 10, 981, 5, -12, 123};
    const int16_t peak[6] = {-80, 25, 1040, 11, -20, 150};
    std::memcpy(sample.mean, mean, sizeof(mean));
    std::memcpy(sample.peak, peak, sizeof(peak));

    uint8_t body[kMaxBody];
    const size_t body_len = build_imu_body(sample, body);
    uint8_t wire[kMaxWire];
    const size_t len = encode_message(MessageType::kImu, 0xBEEF, 1, body, body_len, wire);

    TEST_ASSERT_EQUAL(32, body_len);
    TEST_ASSERT_EQUAL(sizeof(kImuWire), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kImuWire, wire, len);
}

static const uint8_t kObdWire[] = {
    0x05, 0x03, 0xEF, 0xBE, 0x02, 0x04, 0x80, 0x31, 0x17, 0x01, 0x01, 0x01, 0x01, 0x04, 0x02,
    0x0C, 0x28, 0x06, 0x02, 0x1A, 0xF8, 0x0D, 0x78, 0x05, 0x01, 0x3C, 0xA4, 0x0D, 0x00};

static void test_obd_example_encodes_byte_by_byte() {
    // Boot 0xBEEF, seq 2, t_us 1 520 000: RPM 1A F8 com 40 ms, velocidade 3C com 120 ms.
    const ObdValue values[] = {
        {0x0C, 40, 2, {0x1A, 0xF8}},
        {0x0D, 120, 1, {0x3C}},
    };

    uint8_t body[kMaxBody];
    const size_t body_len = build_obd_body(1'520'000, values, 2, body);
    uint8_t wire[kMaxWire];
    const size_t len = encode_message(MessageType::kObd, 0xBEEF, 2, body, body_len, wire);

    TEST_ASSERT_EQUAL(sizeof(kObdWire), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kObdWire, wire, len);
}

static void test_ping_example_encodes_byte_by_byte() {
    uint8_t wire[kMaxWire];
    const size_t len = encode_message(MessageType::kPing, 0, 7, nullptr, 0, wire);
    TEST_ASSERT_EQUAL(sizeof(kPingWire), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kPingWire, wire, len);
}

// Passa os bytes ao leitor e devolve o resultado do último byte.
static MessageReader::Result push_all(MessageReader& reader, const uint8_t* data, size_t len) {
    MessageReader::Result result = MessageReader::Result::kPending;
    for (size_t i = 0; i < len; ++i) {
        result = reader.push(data[i]);
        if (i + 1 < len) {
            TEST_ASSERT_EQUAL(MessageReader::Result::kPending, result);
        }
    }
    return result;
}

static void test_ping_example_decodes() {
    MessageReader reader;
    TEST_ASSERT_EQUAL(MessageReader::Result::kMessage,
                      push_all(reader, kPingWire, sizeof(kPingWire)));

    const Message& msg = reader.message();
    TEST_ASSERT_EQUAL_HEX8(0x81, static_cast<uint8_t>(msg.type));
    TEST_ASSERT_EQUAL_HEX16(0, msg.boot);
    TEST_ASSERT_EQUAL(7, msg.seq);
    TEST_ASSERT_EQUAL(0, msg.body_len);
}

static void test_imu_example_decodes_back_to_its_body() {
    // O corpo antes do COBS, do §8, sem o cabeçalho e sem o CRC: tem zeros, que o COBS repõe.
    const uint8_t body[] = {0x60, 0xE3, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0xFF, 0x0A,
                            0x00, 0xD5, 0x03, 0x05, 0x00, 0xF4, 0xFF, 0x7B, 0x00, 0xB0, 0xFF,
                            0x19, 0x00, 0x10, 0x04, 0x0B, 0x00, 0xEC, 0xFF, 0x96, 0x00};
    MessageReader reader;
    TEST_ASSERT_EQUAL(MessageReader::Result::kMessage, push_all(reader, kImuWire, sizeof(kImuWire)));

    const Message& msg = reader.message();
    TEST_ASSERT_EQUAL(MessageType::kImu, msg.type);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, msg.boot);
    TEST_ASSERT_EQUAL(1, msg.seq);
    TEST_ASSERT_EQUAL(sizeof(body), msg.body_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, msg.body, sizeof(body));
}

static void test_wrong_crc_is_rejected() {
    uint8_t wire[sizeof(kPingWire)];
    std::memcpy(wire, kPingWire, sizeof(wire));
    wire[6] ^= 0x01;  // um bit do CRC
    MessageReader reader;
    TEST_ASSERT_EQUAL(MessageReader::Result::kRejected, push_all(reader, wire, sizeof(wire)));
}

static void test_message_shorter_than_header_and_crc_is_rejected() {
    // COBS de 81 00 00 07 00 1A: seis bytes, um a menos que o mínimo.
    const uint8_t wire[] = {0x02, 0x81, 0x01, 0x02, 0x07, 0x02, 0x1A, 0x00};
    MessageReader reader;
    TEST_ASSERT_EQUAL(MessageReader::Result::kRejected, push_all(reader, wire, sizeof(wire)));
}

static void test_invalid_cobs_is_rejected() {
    // O código 0x09 promete mais bytes do que chegaram antes do 0x00.
    const uint8_t wire[] = {0x09, 0x81, 0x01, 0x02, 0x07, 0x02, 0x1A, 0x01, 0x00};
    MessageReader reader;
    TEST_ASSERT_EQUAL(MessageReader::Result::kRejected, push_all(reader, wire, sizeof(wire)));
}

static void test_lone_zero_bytes_are_ignored() {
    MessageReader reader;
    TEST_ASSERT_EQUAL(MessageReader::Result::kPending, reader.push(0x00));
    TEST_ASSERT_EQUAL(MessageReader::Result::kPending, reader.push(0x00));
    TEST_ASSERT_EQUAL(MessageReader::Result::kMessage,
                      push_all(reader, kPingWire, sizeof(kPingWire)));
}

static void test_garbage_without_zero_is_rejected_once_and_the_next_message_is_read() {
    MessageReader reader;
    int rejected = 0;
    for (int i = 0; i < 1000; ++i) {
        if (reader.push(0x55) == MessageReader::Result::kRejected) {
            ++rejected;
        }
    }
    TEST_ASSERT_EQUAL(MessageReader::Result::kRejected, reader.push(0x00));
    TEST_ASSERT_EQUAL(0, rejected);
    TEST_ASSERT_EQUAL(MessageReader::Result::kMessage,
                      push_all(reader, kPingWire, sizeof(kPingWire)));
    TEST_ASSERT_EQUAL(7, reader.message().seq);
}

static void test_the_longest_message_round_trips() {
    // 255 bytes antes do COBS, sem nenhum zero: 257 no cabo, mais o 0x00.
    uint8_t body[kMaxBody];
    for (size_t i = 0; i < kMaxBody; ++i) {
        body[i] = static_cast<uint8_t>(1 + i % 255);
    }
    uint8_t wire[kMaxWire];
    const size_t len = encode_message(MessageType::kLog, 0x1234, 0x0101, body, kMaxBody, wire);
    TEST_ASSERT_EQUAL(kMaxWire, len);

    MessageReader reader;
    TEST_ASSERT_EQUAL(MessageReader::Result::kMessage, push_all(reader, wire, len));
    TEST_ASSERT_EQUAL(kMaxBody, reader.message().body_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, reader.message().body, kMaxBody);
}

static void test_more_than_255_bytes_after_cobs_is_rejected() {
    // 257 códigos 0x01 viram 256 zeros: cabe no cabo, mas passa do tamanho máximo.
    MessageReader reader;
    for (size_t i = 0; i < kMaxEncoded; ++i) {
        TEST_ASSERT_EQUAL(MessageReader::Result::kPending, reader.push(0x01));
    }
    TEST_ASSERT_EQUAL(MessageReader::Result::kRejected, reader.push(0x00));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc_of_the_check_string);
    RUN_TEST(test_ping_example_encodes_byte_by_byte);
    RUN_TEST(test_imu_example_encodes_byte_by_byte);
    RUN_TEST(test_obd_example_encodes_byte_by_byte);
    RUN_TEST(test_ping_example_decodes);
    RUN_TEST(test_imu_example_decodes_back_to_its_body);
    RUN_TEST(test_wrong_crc_is_rejected);
    RUN_TEST(test_message_shorter_than_header_and_crc_is_rejected);
    RUN_TEST(test_invalid_cobs_is_rejected);
    RUN_TEST(test_lone_zero_bytes_are_ignored);
    RUN_TEST(test_garbage_without_zero_is_rejected_once_and_the_next_message_is_read);
    RUN_TEST(test_the_longest_message_round_trips);
    RUN_TEST(test_more_than_255_bytes_after_cobs_is_rejected);
    return UNITY_END();
}
