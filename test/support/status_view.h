// Lê o corpo de um STATUS pelos offsets do docs/PROTOCOLO.md §3.1, à parte do código do
// núcleo, para que os testes confiram o documento e não a própria implementação.
#pragma once

#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vguard::testing {

struct StatusView {
    uint8_t version;
    uint64_t t_us;
    uint8_t platform;
    uint8_t sources;
    uint8_t reset_reason;
    uint8_t imu_rate;
    uint8_t obd_rate;
    uint8_t imu_state;
    uint8_t obd_state;
    uint8_t pids[12];
    uint32_t dropped, rejected, imu_failures, obd_sent, obd_answered, obd_timeouts, blocked,
        can_errors, bus_off;
    uint32_t heap_min;
    std::string firmware;

    static StatusView parse(const std::vector<uint8_t>& body) {
        TEST_ASSERT_TRUE_MESSAGE(body.size() >= 68 && body.size() <= 100,
                                 "o corpo do STATUS tem de 68 a 100 bytes");
        auto le = [&](size_t offset, size_t size) {
            uint64_t v = 0;
            for (size_t i = 0; i < size; ++i) {
                v |= static_cast<uint64_t>(body[offset + i]) << (8 * i);
            }
            return v;
        };
        StatusView s{};
        s.version = body[0];
        s.t_us = le(1, 8);
        s.platform = body[9];
        s.sources = body[10];
        s.reset_reason = body[11];
        s.imu_rate = body[12];
        s.obd_rate = body[13];
        s.imu_state = body[14];
        s.obd_state = body[15];
        for (size_t i = 0; i < 12; ++i) {
            s.pids[i] = body[16 + i];
        }
        uint32_t* counters[] = {&s.dropped,      &s.rejected, &s.imu_failures,
                                &s.obd_sent,     &s.obd_answered, &s.obd_timeouts,
                                &s.blocked,      &s.can_errors,   &s.bus_off};
        for (size_t i = 0; i < 9; ++i) {
            *counters[i] = static_cast<uint32_t>(le(28 + 4 * i, 4));
        }
        s.heap_min = static_cast<uint32_t>(le(64, 4));
        s.firmware.assign(body.begin() + 68, body.end());
        return s;
    }
};

}  // namespace vguard::testing
