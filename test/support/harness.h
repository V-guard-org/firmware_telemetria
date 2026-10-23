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
        : can_(can), link_(link), core_(can, link, imu, system, boot) {}

    Core& core() { return core_; }

    // Põe o carro no barramento: ele responde às requisições que o núcleo transmitir.
    void connect(FakeCar& car) { car_ = &car; }

    void start(uint64_t t_us = 0) {
        set_clock(t_us);
        core_.start(t_us);
        deliver_transmits(t_us);
    }

    // Avança o relógio até t_us, chamando o núcleo em cada prazo vencido e em cada resposta do
    // carro no caminho. Uma resposta no mesmo instante de um prazo chega antes dele.
    void run_until(uint64_t t_us) {
        TEST_ASSERT_TRUE_MESSAGE(t_us >= now_us_, "o relógio falso não volta");
        while (true) {
            const uint64_t deadline = core_.next_deadline_us();
            const FakeCar::Answer* answer = car_ != nullptr ? car_->next() : nullptr;
            if (answer != nullptr && answer->t_us <= t_us && answer->t_us <= deadline) {
                const FakeCar::Answer a = *answer;
                car_->pop();
                can_frame(a.frame, a.t_us);
                continue;
            }
            if (deadline > t_us) {
                break;
            }
            TEST_ASSERT_TRUE_MESSAGE(deadline >= now_us_, "prazo no passado");
            set_clock(deadline);
            core_.on_deadline(deadline);
            deliver_transmits(deadline);
        }
        now_us_ = t_us;
    }

    // A plataforma chama o núcleo uma vez só, em t_us, bem depois do prazo: o notebook dormiu.
    void late_deadline(uint64_t t_us) {
        TEST_ASSERT_TRUE_MESSAGE(t_us >= now_us_, "o relógio falso não volta");
        set_clock(t_us);
        core_.on_deadline(t_us);
        deliver_transmits(t_us);
    }

    // Um quadro que chega do barramento no instante t_us.
    void can_frame(const CanFrame& frame, uint64_t t_us) {
        enter(t_us, [&] { core_.on_can_frame(frame, t_us); });
    }

    // Um pedaço de bytes da Pi, que chega no instante t_us.
    void link_bytes(const uint8_t* data, size_t len, uint64_t t_us) {
        enter(t_us, [&] { core_.on_link_bytes(data, len, t_us); });
    }

    // Um pedaço de bytes da 2ª entrada, a UART0 da fase 4 (PLATAFORMA.md §5), no instante t_us.
    void dev_bytes(const uint8_t* data, size_t len, uint64_t t_us) {
        enter(t_us, [&] { core_.on_dev_bytes(data, len, t_us); });
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
        enter(t_us, [&] { core_.on_platform_log(level, text, t_us); });
    }

    StatusView last_status() const {
        const auto statuses = link_.messages_of(MessageType::kStatus);
        TEST_ASSERT_FALSE_MESSAGE(statuses.empty(), "nenhum STATUS saiu");
        return StatusView::parse(statuses.back().body);
    }

private:
    // Um evento no instante t_us: o relógio avança até lá, o núcleo é chamado, e o que ele
    // transmitiu volta como resultado e chega ao carro.
    template <typename Call>
    void enter(uint64_t t_us, Call&& call) {
        run_until(t_us);
        set_clock(t_us);
        call();
        deliver_transmits(t_us);
    }

    void set_clock(uint64_t t_us) {
        now_us_ = t_us;
        can_.now_us = t_us;
    }

    // Cada quadro transmitido volta como resultado ok na volta seguinte do laço, no mesmo
    // instante, como no vcan (PLATAFORMA.md §3.1), e chega ao carro.
    void deliver_transmits(uint64_t t_us) {
        while (delivered_ < can_.count()) {
            const FakeCan::Call call = can_.call(delivered_++);
            if (call.what != 'X') {
                continue;
            }
            if (car_ != nullptr) {
                car_->on_request(call.frame, t_us);
            }
            core_.on_can_tx_result(TxResult::kOk, t_us);
        }
    }

    FakeCan& can_;
    FakeLink& link_;
    Core core_;
    FakeCar* car_ = nullptr;
    size_t delivered_ = 0;
    uint64_t now_us_ = 0;
};

}  // namespace vguard::testing
