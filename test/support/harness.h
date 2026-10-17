// Monta o núcleo com as portas falsas e um relógio falso que salta de prazo em prazo
// (docs/PLATAFORMA.md §4.3).
#pragma once

#include <unity.h>

#include <cstdint>

#include "core.h"
#include "fakes.h"
#include "protocol.h"
#include "status_view.h"

namespace vguard::testing {

inline BootInfo test_boot() {
    return {0xBEEF, ResetReason::kPowerOn, Platform::kNotebook, "0.1.0-teste"};
}

class Harness {
public:
    Harness(FakeCan& can, FakeLink& link, ImuPort& imu, FakeSystem& system,
            const BootInfo& boot = test_boot())
        : link_(link), core_(can, link, imu, system, boot) {}

    Core& core() { return core_; }

    void start(uint64_t t_us = 0) {
        now_us_ = t_us;
        core_.start(t_us);
    }

    // Avança o relógio até t_us, chamando o núcleo em cada prazo vencido no caminho.
    void run_until(uint64_t t_us) {
        TEST_ASSERT_TRUE_MESSAGE(t_us >= now_us_, "o relógio falso não volta");
        while (core_.next_deadline_us() <= t_us) {
            const uint64_t deadline = core_.next_deadline_us();
            TEST_ASSERT_TRUE_MESSAGE(deadline >= now_us_, "prazo no passado");
            now_us_ = deadline;
            core_.on_deadline(deadline);
        }
        now_us_ = t_us;
    }

    // Um pedaço de bytes da Pi, que chega no instante t_us.
    void link_bytes(const uint8_t* data, size_t len, uint64_t t_us) {
        run_until(t_us);
        core_.on_link_bytes(data, len, t_us);
    }

    // Um pedaço de bytes da 2ª entrada, a UART0 da fase 4 (PLATAFORMA.md §5), no instante t_us.
    void dev_bytes(const uint8_t* data, size_t len, uint64_t t_us) {
        run_until(t_us);
        core_.on_dev_bytes(data, len, t_us);
    }

    // Uma mensagem da Pi inteira, num pedaço só, no instante t_us.
    void from_pi(MessageType type, const uint8_t* body, size_t len, uint64_t t_us) {
        uint8_t wire[kMaxWire];
        link_bytes(wire, encode_message(type, 0, 0, body, len, wire), t_us);
    }

    // Um PING inteiro, num pedaço só, no instante t_us.
    void ping(uint16_t seq, uint64_t t_us) {
        uint8_t wire[kMaxWire];
        const size_t len = encode_message(MessageType::kPing, 0, seq, nullptr, 0, wire);
        link_bytes(wire, len, t_us);
    }

    // Um TAXAS (PROTOCOLO.md §4.2): a taxa da IMU e a do OBD, em amostras/s.
    void taxas(uint8_t imu, uint8_t obd, uint64_t t_us) {
        const uint8_t body[] = {imu, obd};
        from_pi(MessageType::kTaxas, body, sizeof(body), t_us);
    }

    // Um aviso ou um erro da plataforma, no instante t_us.
    void platform_log(LogLevel level, const char* text, uint64_t t_us) {
        run_until(t_us);
        core_.on_platform_log(level, text, t_us);
    }

    StatusView last_status() const {
        const auto statuses = link_.messages_of(MessageType::kStatus);
        TEST_ASSERT_FALSE_MESSAGE(statuses.empty(), "nenhum STATUS saiu");
        return StatusView::parse(statuses.back().body);
    }

private:
    FakeLink& link_;
    Core core_;
    uint64_t now_us_ = 0;
};

}  // namespace vguard::testing
