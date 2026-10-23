// Portas falsas para os testes do núcleo (docs/PLATAFORMA.md §4.3).
#pragma once

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "core.h"
#include "protocol.h"

namespace vguard::testing {

// A porta CAN que ainda não faz nada: o OBD chega no ticket do caminho feliz (#25).
class FakeCan : public CanPort {
public:
    void listen() override {}
    void talk() override {}
    void transmit(const CanFrame&) override {}
    ListenSummary listen_summary() const override { return {}; }
};

// Uma mensagem que saiu pela UART, já conferida e com o corpo copiado.
struct SentMessage {
    MessageType type;
    uint16_t boot;
    uint16_t seq;
    std::vector<uint8_t> body;
};

// A UART que guarda os bytes escritos, num buffer fixo, para não alocar durante o teste.
// Travada, ela não aceita nenhum byte, como uma Pi que parou de ler.
class FakeLink : public LinkPort {
public:
    size_t tx_free() const override { return stuck_ ? 0 : sizeof(bytes_) - len_; }

    void write(const uint8_t* data, size_t len) override {
        TEST_ASSERT_TRUE_MESSAGE(len <= tx_free(), "o núcleo escreveu mais do que cabia");
        std::memcpy(bytes_ + len_, data, len);
        len_ += len;
    }

    void set_stuck(bool stuck) { stuck_ = stuck; }

    // Todas as mensagens escritas até agora. Uma mensagem que não confere falha o teste.
    std::vector<SentMessage> messages() const {
        std::vector<SentMessage> out;
        MessageReader reader;
        for (size_t i = 0; i < len_; ++i) {
            const auto result = reader.push(bytes_[i]);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MessageReader::Result::kRejected, result,
                                          "o núcleo escreveu uma mensagem inválida");
            if (result == MessageReader::Result::kMessage) {
                const Message& m = reader.message();
                out.push_back({m.type, m.boot, m.seq,
                               std::vector<uint8_t>(m.body, m.body + m.body_len)});
            }
        }
        return out;
    }

    std::vector<SentMessage> messages_of(MessageType type) const {
        std::vector<SentMessage> out;
        for (auto& m : messages()) {
            if (m.type == type) {
                out.push_back(m);
            }
        }
        return out;
    }

private:
    uint8_t bytes_[1 << 16];
    size_t len_ = 0;
    bool stuck_ = false;
};

// A IMU falsa. Sem nada programado, é um sensor ausente: o reinício falha. Presente, ela
// devolve as leituras do roteiro, uma por chamada, e depois sempre a mesma leitura; uma
// leitura vazia no roteiro é uma leitura com falha.
class FakeImu : public ImuPort {
public:
    bool read(ImuReading& out, uint64_t) override {
        if (next_ < script.size()) {
            const std::optional<ImuReading>& r = script[next_++];
            if (r) {
                out = *r;
            }
            return r.has_value();
        }
        if (failing) {
            return false;
        }
        out = reading;
        return true;
    }
    bool restart(uint64_t) override {
        ++restarts;
        return present;
    }
    bool simulated() const override { return is_simulated; }

    bool present = false;
    bool failing = false;                       // depois do roteiro, toda leitura falha
    ImuReading reading{{0, 0, 9.81f}, {0, 0, 0}};  // parado e nivelado
    std::vector<std::optional<ImuReading>> script;
    bool is_simulated = true;
    uint32_t restarts = 0;

private:
    size_t next_ = 0;
};

// Quem se registra para uma mensagem de desenvolvimento, como a IMU simulada para a 0x83.
// Guarda o corpo e o instante de cada mensagem entregue.
class FakeDevHandler : public DevMessageHandler {
public:
    struct Delivered {
        std::vector<uint8_t> body;
        uint64_t t_us;
    };

    bool on_dev_message(const uint8_t* body, size_t len, uint64_t t_us) override {
        delivered.push_back({std::vector<uint8_t>(body, body + len), t_us});
        return accepts;
    }

    bool accepts = true;  // false: o corpo não serve, e a mensagem soma em rejeitadas
    std::vector<Delivered> delivered;
};

class FakeSystem : public SystemPort {
public:
    uint32_t heap_min() const override { return min_free_heap; }

    uint32_t min_free_heap = 0;
};

}  // namespace vguard::testing
