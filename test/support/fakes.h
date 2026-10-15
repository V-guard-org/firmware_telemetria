// Portas falsas para os testes do núcleo (docs/PLATAFORMA.md §4.3).
#pragma once

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core.h"
#include "protocol.h"

namespace vguard::testing {

// A porta CAN que só anota o que o núcleo pediu. O OBD chega no ticket do caminho feliz.
class FakeCan : public CanPort {
public:
    void listen() override { ++listen_calls; }
    void talk() override { ++talk_calls; }
    void transmit(const CanFrame&) override { ++transmitted; }
    ListenSummary listen_summary() const override { return {}; }

    int listen_calls = 0;
    int talk_calls = 0;
    int transmitted = 0;
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
        FrameReader reader;
        for (size_t i = 0; i < len_; ++i) {
            const auto result = reader.push(bytes_[i]);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(FrameReader::Result::kRejected, result,
                                          "o núcleo escreveu uma mensagem inválida");
            if (result == FrameReader::Result::kMessage) {
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

// A IMU que ainda não lê nada: a máquina da IMU chega no ticket da IMU de ponta a ponta.
class FakeImu : public ImuPort {
public:
    bool read(ImuReading&) override { return false; }
    bool restart() override { return true; }
    bool simulated() const override { return simulated_; }

    bool simulated_ = true;
};

class FakeSystem : public SystemPort {
public:
    uint32_t heap_min() const override { return heap_min_; }

    uint32_t heap_min_ = 0;
};

}  // namespace vguard::testing
