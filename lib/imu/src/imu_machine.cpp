#include "imu_machine.h"

#include <cmath>

namespace vguard {
namespace {

constexpr uint64_t kReadPeriodUs = 10'000;  // 100 leituras por segundo
constexpr uint32_t kReadsPerSecond = 100;
constexpr uint64_t kRestartPeriodUs = 1'000'000;
constexpr uint8_t kFailuresToFail = 10;     // seguidas
constexpr double kScale[6] = {100, 100, 100, 10, 10, 10};  // 0,01 m/s² e 0,1 °/s no cabo
constexpr double kWireMax = 32767;

// O próximo instante de algo feito a cada período; depois de um atraso, sem rajada.
uint64_t next_due(uint64_t due_us, uint64_t period_us, uint64_t now_us) {
    due_us += period_us;
    return due_us > now_us ? due_us : now_us + period_us;
}

// Em unidades do cabo, travado em ±32767 (PROTOCOLO.md §3.2).
int16_t to_wire(double value, double scale) {
    double scaled = value * scale;
    if (scaled > kWireMax) {
        scaled = kWireMax;
    } else if (scaled < -kWireMax) {
        scaled = -kWireMax;
    }
    return static_cast<int16_t>(std::lround(scaled));
}

}  // namespace

ImuMachine::ImuMachine(ImuPort& port, uint8_t rate) : port_(port), rate_(rate) {}

void ImuMachine::start(uint64_t now_us) {
    if (port_.restart(now_us)) {
        become_ok(now_us);
    } else {
        become_failed(now_us);
    }
}

bool ImuMachine::on_deadline(uint64_t now_us, ImuSampleBody& sample) {
    if (failed_) {
        if (now_us >= next_restart_us_) {
            if (port_.restart(now_us)) {
                become_ok(now_us);
            } else {
                next_restart_us_ = next_due(next_restart_us_, kRestartPeriodUs, now_us);
            }
        }
        return false;
    }
    if (now_us < next_read_us_) {
        return false;
    }
    next_read_us_ = next_due(next_read_us_, kReadPeriodUs, now_us);

    ImuReading reading;
    ++reads_;
    if (port_.read(reading, now_us)) {
        // Do referencial do sensor para o do carro: a identidade até o T11 (PROTOCOLO.md §3.2).
        failures_in_a_row_ = 0;
        add(reading, now_us);
    } else {
        ++failures_;
        if (++failures_in_a_row_ >= kFailuresToFail) {
            become_failed(now_us);
            return false;
        }
    }
    if (reads_ < kReadsPerSecond / rate_) {
        return false;
    }
    const bool any_valid = valid_ > 0;  // com todas as leituras com falha, não sai amostra
    if (any_valid) {
        fill(sample);
    }
    clear_interval();
    return any_valid;
}

uint64_t ImuMachine::next_deadline_us() const {
    return failed_ ? next_restart_us_ : next_read_us_;
}

void ImuMachine::become_ok(uint64_t now_us) {
    failed_ = false;
    failures_in_a_row_ = 0;
    clear_interval();
    next_read_us_ = now_us + kReadPeriodUs;
}

void ImuMachine::become_failed(uint64_t now_us) {
    failed_ = true;
    clear_interval();
    next_restart_us_ = now_us + kRestartPeriodUs;
}

void ImuMachine::add(const ImuReading& reading, uint64_t t_us) {
    const float axes[6] = {reading.accel[0], reading.accel[1], reading.accel[2],
                           reading.gyro[0],  reading.gyro[1],  reading.gyro[2]};
    for (size_t i = 0; i < 6; ++i) {
        sum_[i] += axes[i];
        // O pico é a leitura de maior módulo, com o sinal.
        if (valid_ == 0 || std::fabs(axes[i]) > std::fabs(peak_[i])) {
            peak_[i] = axes[i];
        }
    }
    sum_t_us_ += t_us;
    ++valid_;
}

void ImuMachine::fill(ImuSampleBody& sample) const {
    // O instante é a média dos instantes das leituras válidas: o meio do intervalo.
    sample.t_us = sum_t_us_ / valid_;
    for (size_t i = 0; i < 6; ++i) {
        sample.mean[i] = to_wire(sum_[i] / valid_, kScale[i]);
        sample.peak[i] = to_wire(peak_[i], kScale[i]);
    }
}

void ImuMachine::clear_interval() {
    reads_ = 0;
    valid_ = 0;
    sum_t_us_ = 0;
    for (size_t i = 0; i < 6; ++i) {
        sum_[i] = 0;
        peak_[i] = 0;
    }
}

}  // namespace vguard
