// Portas falsas para os testes do núcleo (docs/PLATAFORMA.md §4.3).
#pragma once

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "core.h"
#include "protocol.h"

namespace vguard::testing {

// A porta CAN falsa: guarda, em ordem e com o instante, o que o núcleo fez nela, num buffer
// fixo para não alocar durante o teste. O resumo da escuta é o que o teste programar; sem
// nada programado, é um barramento mudo. O harness acerta o relógio antes de cada chamada.
class FakeCan : public CanPort {
public:
    struct Call {
        char what;  // 'L' listen(), 'T' talk(), 'X' transmit()
        CanFrame frame;
        uint64_t t_us;
    };

    void listen() override { record('L', {}); }
    void talk() override { record('T', {}); }
    void transmit(const CanFrame& frame) override { record('X', frame); }
    ListenSummary listen_summary() const override { return summary; }

    size_t count() const { return count_; }
    const Call& call(size_t i) const { return calls_[i]; }

    // As chamadas até agora, uma letra cada: "LTXX…".
    std::string letters() const {
        std::string out;
        for (size_t i = 0; i < count_; ++i) {
            out += calls_[i].what;
        }
        return out;
    }

    // Os quadros transmitidos a partir do instante from_us.
    std::vector<Call> transmitted(uint64_t from_us = 0) const {
        std::vector<Call> out;
        for (size_t i = 0; i < count_; ++i) {
            if (calls_[i].what == 'X' && calls_[i].t_us >= from_us) {
                out.push_back(calls_[i]);
            }
        }
        return out;
    }

    ListenSummary summary{};
    uint64_t now_us = 0;

private:
    void record(char what, const CanFrame& frame) {
        TEST_ASSERT_TRUE_MESSAGE(count_ < kCapacity, "a porta CAN falsa encheu");
        calls_[count_++] = {what, frame, now_us};
    }

    static constexpr size_t kCapacity = 8192;
    Call calls_[kCapacity];
    size_t count_ = 0;
};

// O carro no barramento: centrais que respondem às requisições do modo 01, escritas aqui à
// parte do firmware. Cada central responde pelo seu ID, de 0x7E8 a 0x7EF, a uma requisição
// para todas (0x7DF) ou para ela (o ID menos 8). Os mapas de suporte (01 00, 01 20 e 01 40)
// saem dos PIDs que ela tem, e cada valor é o que o teste puser em value.
class FakeCar {
public:
    struct Answer {
        uint64_t t_us;
        CanFrame frame;
    };

    FakeCar() {
        for (unsigned pid = 0; pid < 256; ++pid) {
            value[pid][0] = static_cast<uint8_t>(pid);  // um valor qualquer, diferente por PID
            value[pid][1] = 0x01;
        }
    }

    void add_central(uint32_t answer_id, std::initializer_list<uint8_t> pids) {
        TEST_ASSERT_TRUE(central_count_ < 8);
        Central& c = centrals_[central_count_++];
        c.answer_id = answer_id;
        for (uint8_t pid : pids) {
            c.has[pid] = true;
        }
    }

    // A requisição que o núcleo transmitiu em t_us: marca as respostas.
    void on_request(const CanFrame& request, uint64_t t_us) {
        if (request.data[0] != 0x02 || request.data[1] != 0x01) {
            return;
        }
        const uint8_t pid = request.data[2];
        for (size_t i = 0; i < central_count_; ++i) {
            const Central& c = centrals_[i];
            if (request.id != 0x7DF && request.id != c.answer_id - 8) {
                continue;
            }
            uint8_t raw[4];
            size_t len = 0;
            if (pid == 0x00 || pid == 0x20 || pid == 0x40) {
                if (!support_map(c, pid, raw)) {
                    continue;
                }
                len = 4;
            } else if (c.has[pid]) {
                len = (pid == 0x0C || pid == 0x42) ? 2 : 1;
                std::memcpy(raw, value[pid], len);
            } else {
                continue;
            }
            CanFrame answer{c.answer_id, false, false, 8, {}};
            std::memset(answer.data, 0xAA, 8);
            if (multi_frame[pid]) {
                // O 1º quadro de uma resposta em vários: 1 e o tamanho, 10 bytes.
                answer.data[0] = 0x10;
                answer.data[1] = 0x0A;
                answer.data[2] = 0x41;
                answer.data[3] = pid;
            } else {
                answer.data[0] = static_cast<uint8_t>(2 + len);
                answer.data[1] = 0x41;
                answer.data[2] = pid;
                std::memcpy(answer.data + 3, raw, len);
            }
            const uint32_t delay = delay_us[pid] != 0 ? delay_us[pid] : default_delay_us;
            schedule({t_us + delay, answer});
        }
    }

    // A próxima resposta a chegar, ou nullptr.
    const Answer* next() const { return pending_count_ > 0 ? &pending_[0] : nullptr; }
    void pop() {
        for (size_t i = 1; i < pending_count_; ++i) {
            pending_[i - 1] = pending_[i];
        }
        --pending_count_;
    }

    uint8_t value[256][2];
    uint32_t default_delay_us = 5'000;
    uint32_t delay_us[256] = {};      // 0: o padrão
    bool multi_frame[256] = {};       // responde com o 1º quadro de uma resposta em vários

private:
    struct Central {
        uint32_t answer_id;
        bool has[256];
    };

    // O mapa de suporte de base (0x00, 0x20 ou 0x40): no 1º byte, o bit 7 é o PID base + 1, e
    // o último bit diz que há PIDs acima de base + 0x20. Só responde a um mapa que o anterior
    // anunciou.
    static bool support_map(const Central& c, uint8_t base, uint8_t out[4]) {
        if (base > 0 && !has_above(c, base)) {
            return false;
        }
        uint32_t bits = 0;
        for (unsigned pid = base + 1u; pid <= base + 0x20u; ++pid) {
            if (c.has[pid] || (pid == base + 0x20u && has_above(c, pid))) {
                bits |= 1u << (base + 0x20u - pid);
            }
        }
        for (int i = 0; i < 4; ++i) {
            out[i] = static_cast<uint8_t>(bits >> (24 - 8 * i));
        }
        return true;
    }

    static bool has_above(const Central& c, unsigned pid) {
        for (unsigned p = pid + 1; p < 256; ++p) {
            if (c.has[p]) {
                return true;
            }
        }
        return false;
    }

    void schedule(const Answer& answer) {
        TEST_ASSERT_TRUE_MESSAGE(pending_count_ < kMaxPending, "respostas demais pendentes");
        size_t i = pending_count_++;
        while (i > 0 && pending_[i - 1].t_us > answer.t_us) {
            pending_[i] = pending_[i - 1];
            --i;
        }
        pending_[i] = answer;
    }

    static constexpr size_t kMaxPending = 64;
    Central centrals_[8] = {};
    size_t central_count_ = 0;
    Answer pending_[kMaxPending];
    size_t pending_count_ = 0;
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
