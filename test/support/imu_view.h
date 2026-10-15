// Lê o corpo de uma amostra de IMU pelos offsets do docs/PROTOCOLO.md §3.2, à parte do
// código do núcleo, para que os testes confiram o documento e não a própria implementação.
#pragma once

#include <unity.h>

#include <cstdint>
#include <vector>

namespace vguard::testing {

enum Axis : size_t { kAx, kAy, kAz, kGx, kGy, kGz };

struct ImuView {
    uint64_t t_us;
    int16_t mean[6];  // ax, ay, az em 0,01 m/s²; gx, gy, gz em 0,1 °/s
    int16_t peak[6];

    static ImuView parse(const std::vector<uint8_t>& body) {
        TEST_ASSERT_EQUAL_MESSAGE(32, body.size(), "o corpo da IMU tem 32 bytes");
        ImuView s{};
        for (size_t i = 0; i < 8; ++i) {
            s.t_us |= static_cast<uint64_t>(body[i]) << (8 * i);
        }
        for (size_t axis = 0; axis < 6; ++axis) {
            s.mean[axis] = static_cast<int16_t>(body[8 + 2 * axis] | (body[9 + 2 * axis] << 8));
            s.peak[axis] = static_cast<int16_t>(body[20 + 2 * axis] | (body[21 + 2 * axis] << 8));
        }
        return s;
    }
};

}  // namespace vguard::testing
