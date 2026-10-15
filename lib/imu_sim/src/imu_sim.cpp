#include "imu_sim.h"

namespace vguard {
namespace {

constexpr size_t kBodySize = 13;          // 6 × i16 e a marca de falha
constexpr uint64_t kSilenceUs = 1'000'000;  // mais que isto sem IMU_SIM, a leitura falha

int16_t i16(const uint8_t* p) {
    return static_cast<int16_t>(p[0] | (p[1] << 8));
}

}  // namespace

bool ImuSim::read(ImuReading& out, uint64_t t_us) {
    if (!has_reading_ || failing_ || silent(t_us)) {
        return false;
    }
    out = last_;
    return true;
}

bool ImuSim::restart(uint64_t t_us) {
    if (!started_) {
        // O 1º reinício é o da partida: o 1 s de espera conta dele, seja qual for o relógio.
        started_ = true;
        last_message_us_ = t_us;
    }
    return !failing_ && !silent(t_us);
}

bool ImuSim::on_dev_message(const uint8_t* body, size_t len, uint64_t t_us) {
    if (len != kBodySize) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        last_.accel[i] = i16(body + 2 * i) / 100.0f;     // 0,01 m/s²
        last_.gyro[i] = i16(body + 6 + 2 * i) / 10.0f;   // 0,1 °/s
    }
    failing_ = body[12] != 0;
    has_reading_ = true;
    last_message_us_ = t_us;
    return true;
}

bool ImuSim::silent(uint64_t t_us) const {
    return t_us > last_message_us_ + kSilenceUs;
}

}  // namespace vguard
