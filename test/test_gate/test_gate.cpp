// Portão da lista fechada, sozinho (costura 2 da spec da fase 1, docs/TESTES.md §5).
// Cada quadro passa pelo portão e é comparado com a regra da D2 escrita aqui, à parte do
// código do portão. Roda no notebook com: pio test -e native
// A varredura da parte 2 roda 134 milhões de vezes: sem otimização, o laço do próprio teste
// a levaria de cerca de 1 s (TESTES.md §5) para 3 s. O portão é compilado como o resto.
#pragma GCC optimize("O2")
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "closed_list_gate.h"

using namespace vguard;

namespace {

// A porta CAN da plataforma: guarda o que o portão deixou passar.
class RecordingCan : public CanPort {
public:
    void listen() override { calls.push_back('L'); }
    void talk() override { calls.push_back('T'); }
    void transmit(const CanFrame& frame) override {
        ++transmitted;
        last = frame;
    }
    ListenSummary listen_summary() const override { return {7, 15, 120}; }

    std::vector<char> calls;
    uint64_t transmitted = 0;
    CanFrame last{};
};

// Quem o portão avisa a cada quadro barrado.
class RecordingListener : public GateListener {
public:
    void on_frame_blocked(const CanFrame& frame, bool was_released) override {
        ++blocked;
        last = frame;
        released = was_released;
    }

    uint64_t blocked = 0;
    CanFrame last{};
    bool released = false;
};

CanFrame request(uint32_t id, uint8_t pid) {
    return {id, false, false, 8, {0x02, 0x01, pid, 0x55, 0x55, 0x55, 0x55, 0x55}};
}

// A regra do TESTES.md §5, escrita de novo aqui, sem olhar o portão: os IDs aceitos numa
// tabela, e o conteúdo byte a byte. O PID e os bytes 3 a 7 são livres.
bool on_the_closed_list(const CanFrame& frame) {
    static const uint32_t kAccepted[] = {0x7DF, 0x7E0, 0x7E1, 0x7E2, 0x7E3,
                                         0x7E4, 0x7E5, 0x7E6, 0x7E7};
    bool accepted_id = false;
    for (uint32_t id : kAccepted) {
        accepted_id = accepted_id || frame.id == id;
    }
    return accepted_id && !frame.extended && !frame.remote && frame.dlc == 8 &&
           frame.data[0] == 0x02 && frame.data[1] == 0x01;
}

CanFrame frame(uint32_t id, std::initializer_list<uint8_t> bytes, bool extended = false,
               bool remote = false) {
    CanFrame f{id, extended, remote, static_cast<uint8_t>(bytes.size()), {}};
    uint8_t i = 0;
    for (uint8_t b : bytes) {
        f.data[i++] = b;
    }
    return f;
}

}  // namespace

void setUp() {}
void tearDown() {}

static void test_listen_and_talk_go_through_to_the_port() {
    RecordingCan can;
    RecordingListener listener;
    ClosedListGate gate(can, listener);

    gate.talk();
    gate.listen();

    TEST_ASSERT_EQUAL(2, can.calls.size());
    TEST_ASSERT_EQUAL('T', can.calls[0]);
    TEST_ASSERT_EQUAL('L', can.calls[1]);
    const ListenSummary summary = gate.listen_summary();
    TEST_ASSERT_EQUAL(7, summary.frames);
    TEST_ASSERT_EQUAL(15, summary.edges);
    TEST_ASSERT_EQUAL(120, summary.longest_dominant_us);
}

static void test_a_mode_01_request_goes_out_while_the_transceiver_is_released() {
    RecordingCan can;
    RecordingListener listener;
    ClosedListGate gate(can, listener);

    gate.talk();
    gate.transmit(request(0x7DF, 0x0C));
    gate.transmit(request(0x7E0, 0x0D));
    gate.transmit(request(0x7E7, 0x42));

    TEST_ASSERT_EQUAL(3, can.transmitted);
    TEST_ASSERT_EQUAL_HEX32(0x7E7, can.last.id);
    TEST_ASSERT_EQUAL_HEX8(0x42, can.last.data[2]);
    TEST_ASSERT_EQUAL(0, listener.blocked);
    TEST_ASSERT_EQUAL(0, gate.blocked());
}

// Parte 1: os quadros perigosos, um por um, com o transceptor liberado.
static void test_each_dangerous_frame_is_blocked_counted_and_reported() {
    const CanFrame dangerous[] = {
        frame(0x7DF, {0x01, 0x04, 0, 0, 0, 0, 0, 0}),            // modo 04: apaga as falhas
        frame(0x7E0, {0x04, 0x2E, 0xF1, 0x90, 0x01, 0, 0, 0}),   // 0x2E: escreve na central
        frame(0x7E0, {0x02, 0x27, 0x01, 0, 0, 0, 0, 0}),         // 0x27: acesso de segurança
        frame(0x7E0, {0x04, 0x31, 0x01, 0xFF, 0x00, 0, 0, 0}),   // 0x31: rotina
        frame(0x7E0, {0x02, 0x10, 0x03, 0, 0, 0, 0, 0}),         // 0x10: sessão de diagnóstico
        frame(0x7E0, {0x30, 0x00, 0x00, 0, 0, 0, 0, 0}),         // 0x30: controle de fluxo
        frame(0x7DF, {0x02, 0x09, 0x02, 0, 0, 0, 0, 0}),         // modo 09: o VIN
        frame(0x18DB33F1, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}, true),  // ID de 29 bits
        frame(0x7DF, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}, true),   // 0x7DF, mas de 29 bits
        frame(0x7DF, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}, false, true),  // quadro remoto
        frame(0x7E8, {0x02, 0x01, 0x0C, 0, 0, 0, 0, 0}),         // o ID de uma resposta
        frame(0x7DF, {0x02, 0x01, 0x0C, 0, 0, 0, 0}),            // 7 bytes
        frame(0x7DF, {0x03, 0x02, 0x0C, 0x00, 0, 0, 0, 0}),      // modo 02, 3 bytes úteis
    };

    for (const CanFrame& f : dangerous) {
        TEST_ASSERT_FALSE_MESSAGE(on_the_closed_list(f), "a lista do teste tem um quadro aceito");
        RecordingCan can;
        RecordingListener listener;
        ClosedListGate gate(can, listener);
        gate.talk();

        gate.transmit(f);

        TEST_ASSERT_EQUAL_MESSAGE(0, can.transmitted, "um quadro perigoso chegou à porta");
        TEST_ASSERT_EQUAL(1, gate.blocked());
        TEST_ASSERT_EQUAL(1, listener.blocked);
        TEST_ASSERT_TRUE(listener.released);
        TEST_ASSERT_EQUAL_HEX32(f.id, listener.last.id);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(f.data, listener.last.data, 8);
    }
}

// Parte 2: todas as combinações dos 2 048 IDs de 11 bits com os 65 536 valores dos bytes 0
// e 1, cerca de 134 milhões de quadros. Só passam os 9 IDs com 02 01.
static void test_every_11_bit_id_with_every_byte_0_and_1_follows_the_rule() {
    RecordingCan can;
    RecordingListener listener;
    ClosedListGate gate(can, listener);
    gate.talk();

    uint64_t wrong = 0;
    CanFrame first_wrong{};
    CanFrame f = request(0, 0x0C);
    for (uint32_t id = 0; id < 0x800; ++id) {
        f.id = id;
        for (uint32_t bytes = 0; bytes < 0x10000; ++bytes) {
            f.data[0] = static_cast<uint8_t>(bytes >> 8);
            f.data[1] = static_cast<uint8_t>(bytes);
            const uint64_t before = can.transmitted;
            gate.transmit(f);
            if ((can.transmitted != before) != on_the_closed_list(f) && wrong++ == 0) {
                first_wrong = f;
            }
        }
    }

    char message[64];
    snprintf(message, sizeof(message), "1º erro: ID 0x%03X, bytes %02X %02X",
             static_cast<unsigned>(first_wrong.id), first_wrong.data[0], first_wrong.data[1]);
    TEST_ASSERT_EQUAL_MESSAGE(0, wrong, message);
    TEST_ASSERT_EQUAL(9, can.transmitted);
    TEST_ASSERT_EQUAL(0x800ull * 0x10000 - 9, gate.blocked());
    TEST_ASSERT_EQUAL(0x800ull * 0x10000 - 9, listener.blocked);
}

// Parte 3: um quadro da lista, fora da liberação.
static void test_a_valid_request_outside_the_release_is_blocked() {
    RecordingCan can;
    RecordingListener listener;
    ClosedListGate gate(can, listener);

    gate.transmit(request(0x7DF, 0x0C));  // no boot, antes do 1º talk()
    TEST_ASSERT_EQUAL(0, can.transmitted);
    TEST_ASSERT_EQUAL(1, gate.blocked());
    TEST_ASSERT_FALSE(listener.released);

    gate.talk();
    gate.transmit(request(0x7DF, 0x0C));
    TEST_ASSERT_EQUAL(1, can.transmitted);

    gate.listen();
    gate.transmit(request(0x7E0, 0x0D));  // depois do listen()
    TEST_ASSERT_EQUAL(1, can.transmitted);
    TEST_ASSERT_EQUAL(2, gate.blocked());
    TEST_ASSERT_EQUAL(2, listener.blocked);
    TEST_ASSERT_EQUAL_HEX32(0x7E0, listener.last.id);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_valid_request_outside_the_release_is_blocked);
    RUN_TEST(test_listen_and_talk_go_through_to_the_port);
    RUN_TEST(test_a_mode_01_request_goes_out_while_the_transceiver_is_released);
    RUN_TEST(test_each_dangerous_frame_is_blocked_counted_and_reported);
    RUN_TEST(test_every_11_bit_id_with_every_byte_0_and_1_follows_the_rule);
    return UNITY_END();
}
