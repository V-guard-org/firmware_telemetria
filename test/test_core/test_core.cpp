// O núcleo, testado só pelo que sai nas portas falsas (costura 1 da spec da fase 1).
// Roda no notebook com: pio test -e native
#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <new>

// O core.h vem antes do harness para o PlatformIO achar a lib/core: ele só procura as
// bibliotecas pelos #include dos arquivos de teste, não pelos de test/support/.
#include "core.h"
#include "support/harness.h"

// Conta as alocações do programa de teste inteiro, para provar que o núcleo não aloca
// (PLATAFORMA.md §2.2). Os falsos e o harness também não alocam enquanto o núcleo roda.
static size_t g_allocations = 0;

void* operator new(std::size_t size) {
    ++g_allocations;
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace vguard;
using namespace vguard::testing;

// PONG (PROTOCOLO.md §3.4): seq_ping u16 no offset 0 e t_us u64 no offset 2.
struct PongView {
    uint16_t seq_ping;
    uint64_t t_us;
};

static PongView parse_pong(const SentMessage& msg) {
    TEST_ASSERT_EQUAL(MessageType::kPong, msg.type);
    TEST_ASSERT_EQUAL(10, msg.body.size());
    PongView p{};
    p.seq_ping = static_cast<uint16_t>(msg.body[0] | (msg.body[1] << 8));
    for (size_t i = 0; i < 8; ++i) {
        p.t_us |= static_cast<uint64_t>(msg.body[2 + i]) << (8 * i);
    }
    return p;
}

// O PING de seq 7 do PROTOCOLO.md §8, no cabo.
static const uint8_t kPingWire[] = {0x02, 0x81, 0x01, 0x02, 0x07, 0x02, 0x1A, 0x01, 0x00};

void setUp() {}
void tearDown() {}

static void test_status_at_start_has_every_field() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    sys.min_free_heap = 123'456;
    imu.present = true;
    Harness h(can, link, imu, sys, {0xBEEF, ResetReason::kWatchdog, Platform::kNotebook, "0.1.0-teste"});

    h.start(1'234);

    const auto sent = link.messages();
    TEST_ASSERT_EQUAL(1, sent.size());
    TEST_ASSERT_EQUAL(MessageType::kStatus, sent[0].type);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, sent[0].boot);
    TEST_ASSERT_EQUAL(0, sent[0].seq);

    const StatusView s = StatusView::parse(sent[0].body);
    TEST_ASSERT_EQUAL(1, s.version);
    TEST_ASSERT_EQUAL_UINT64(1'234, s.t_us);
    TEST_ASSERT_EQUAL(0, s.platform);          // notebook
    TEST_ASSERT_EQUAL(1, s.sources);           // bit 0: IMU simulada
    TEST_ASSERT_EQUAL(5, s.reset_reason);      // watchdog
    TEST_ASSERT_EQUAL(25, s.imu_rate);
    TEST_ASSERT_EQUAL(5, s.obd_rate);
    TEST_ASSERT_EQUAL(0, s.imu_state);         // ok
    TEST_ASSERT_EQUAL(0, s.obd_state);         // ESPERANDO_PI
    const uint8_t no_pids[12] = {};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(no_pids, s.pids, 12);
    TEST_ASSERT_EQUAL(0, s.dropped + s.rejected + s.imu_failures + s.obd_sent + s.obd_answered +
                             s.obd_timeouts + s.blocked + s.can_errors + s.bus_off);
    TEST_ASSERT_EQUAL(123'456, s.heap_min);
    TEST_ASSERT_EQUAL_STRING("0.1.0-teste", s.firmware.c_str());
}

static void test_status_every_second_with_increasing_seq() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);

    h.start(0);
    TEST_ASSERT_EQUAL_UINT64(1'000'000, h.core().next_deadline_us());
    h.run_until(5'500'000);

    const auto statuses = link.messages_of(MessageType::kStatus);
    TEST_ASSERT_EQUAL(6, statuses.size());
    for (size_t i = 0; i < statuses.size(); ++i) {
        TEST_ASSERT_EQUAL(i, statuses[i].seq);
        TEST_ASSERT_EQUAL_UINT64(i * 1'000'000, StatusView::parse(statuses[i].body).t_us);
    }
    TEST_ASSERT_EQUAL_UINT64(6'000'000, h.core().next_deadline_us());
}

static void test_status_called_late_goes_out_once_and_restarts_the_rhythm() {
    // O notebook dormiu 3,5 s: a plataforma chama o núcleo uma vez, bem depois do prazo.
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);

    h.start(0);
    h.core().on_deadline(3'500'000);

    const auto statuses = link.messages_of(MessageType::kStatus);
    TEST_ASSERT_EQUAL(2, statuses.size());
    TEST_ASSERT_EQUAL_UINT64(3'500'000, StatusView::parse(statuses[1].body).t_us);
    TEST_ASSERT_EQUAL_UINT64(4'500'000, h.core().next_deadline_us());
}

static void test_ping_in_pieces_is_answered_with_the_instant_of_the_last_piece() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.start(0);

    h.link_bytes(kPingWire, 3, 200'000);
    h.link_bytes(kPingWire + 3, 4, 200'150);
    TEST_ASSERT_EQUAL(0, link.messages_of(MessageType::kPong).size());
    h.link_bytes(kPingWire + 7, 2, 200'300);  // este pedaço tem o 0x00 final

    const auto pongs = link.messages_of(MessageType::kPong);
    TEST_ASSERT_EQUAL(1, pongs.size());
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, pongs[0].boot);
    TEST_ASSERT_EQUAL(1, pongs[0].seq);  // depois do STATUS da partida
    const PongView pong = parse_pong(pongs[0]);
    TEST_ASSERT_EQUAL(7, pong.seq_ping);
    TEST_ASSERT_EQUAL_UINT64(200'300, pong.t_us);
}

static void test_pong_goes_ahead_of_status_and_seq_is_given_when_written() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);

    link.set_stuck(true);              // a UART não aceita nada: tudo fica na fila
    h.start(0);                        // STATUS de 0 s
    h.run_until(1'500'000);            // STATUS de 1 s
    h.ping(9, 1'500'000);              // PONG
    TEST_ASSERT_EQUAL(0, link.messages().size());

    link.set_stuck(false);
    h.run_until(2'000'000);            // STATUS de 2 s, e a fila sai

    const auto sent = link.messages();
    TEST_ASSERT_EQUAL(4, sent.size());
    TEST_ASSERT_EQUAL(9, parse_pong(sent[0]).seq_ping);
    TEST_ASSERT_EQUAL_UINT64(0, StatusView::parse(sent[1].body).t_us);
    TEST_ASSERT_EQUAL_UINT64(1'000'000, StatusView::parse(sent[2].body).t_us);
    TEST_ASSERT_EQUAL_UINT64(2'000'000, StatusView::parse(sent[3].body).t_us);
    for (size_t i = 0; i < sent.size(); ++i) {
        TEST_ASSERT_EQUAL(i, sent[i].seq);  // o PONG, o último a entrar, sai com o seq 0
    }
}

static void test_output_held_by_a_full_uart_is_retried_within_10_ms() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);

    link.set_stuck(true);
    h.start(0);
    h.ping(3, 200'000);                // o PONG fica na fila
    TEST_ASSERT_EQUAL_UINT64(210'000, h.core().next_deadline_us());

    link.set_stuck(false);             // a UART voltou a aceitar, sem evento nenhum
    h.run_until(210'000);

    const auto pongs = link.messages_of(MessageType::kPong);
    TEST_ASSERT_EQUAL(1, pongs.size());
    TEST_ASSERT_EQUAL_UINT64(200'000, parse_pong(pongs[0]).t_us);
    TEST_ASSERT_EQUAL_UINT64(1'000'000, h.core().next_deadline_us());  // fila vazia
}

static void test_full_queue_drops_the_oldest_status_without_spending_seq() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);

    link.set_stuck(true);
    h.start(0);
    h.run_until(20'500'000);           // 21 STATUS, de 0 a 20 s, numa fila de 16
    link.set_stuck(false);
    h.run_until(20'510'000);           // a nova tentativa de escrita esvazia a fila

    const auto sent = link.messages();
    TEST_ASSERT_EQUAL(16, sent.size());
    TEST_ASSERT_EQUAL_UINT64(5'000'000, StatusView::parse(sent.front().body).t_us);
    for (size_t i = 0; i < sent.size(); ++i) {
        TEST_ASSERT_EQUAL(i, sent[i].seq);  // as descartadas não deixam lacunas
    }
    // O de 20 s foi montado antes de entrar na fila, quando 4 já tinham saído.
    TEST_ASSERT_EQUAL(4, StatusView::parse(sent.back().body).dropped);

    h.run_until(21'000'000);
    TEST_ASSERT_EQUAL(5, h.last_status().dropped);
}

static void test_wrong_crc_and_unknown_types_count_as_rejected() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.start(0);

    uint8_t bad_crc[sizeof(kPingWire)];
    std::memcpy(bad_crc, kPingWire, sizeof(bad_crc));
    bad_crc[6] ^= 0x01;
    h.link_bytes(bad_crc, sizeof(bad_crc), 100'000);

    uint8_t wire[kMaxWire];
    // Um código que ninguém usa, um do sentido ESP32 → Pi, e a IMU_SIM, sem ninguém
    // registrado para ela (PLATAFORMA.md §5).
    for (const uint8_t type : {0x90, 0x02, 0x83}) {
        const size_t len = encode_message(static_cast<MessageType>(type), 0, 1, nullptr, 0, wire);
        h.link_bytes(wire, len, 200'000);
    }
    h.ping(2, 300'000);
    h.run_until(1'000'000);

    TEST_ASSERT_EQUAL(4, h.last_status().rejected);
    TEST_ASSERT_EQUAL(1, link.messages_of(MessageType::kPong).size());
}

// O caminho do LOG (PROTOCOLO.md §3.5), o mesmo do LOG de erro de um quadro barrado pelo
// portão. O limite de 10 por segundo está no test_output.
static void test_a_platform_error_goes_out_as_a_log_with_its_instant() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.start(0);
    h.run_until(250'000);

    h.core().on_platform_log(LogLevel::kError, "twai: bus-off", 250'000);

    const auto logs = link.messages_of(MessageType::kLog);
    TEST_ASSERT_EQUAL(1, logs.size());
    const uint8_t expected[] = {
        0x90, 0xD0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,                    // t_us 250 000
        0x01,                                                              // erro
        't', 'w', 'a', 'i', ':', ' ', 'b', 'u', 's', '-', 'o', 'f', 'f'};
    TEST_ASSERT_EQUAL(sizeof(expected), logs[0].body.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, logs[0].body.data(), sizeof(expected));
    TEST_ASSERT_EQUAL(0, h.last_status().blocked);
}

static size_t imu_count(const FakeLink& link) {
    return link.messages_of(MessageType::kImu).size();
}

static void test_taxas_changes_both_rates_and_the_status_shows_them() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    Harness h(can, link, imu, sys);
    h.start(0);

    h.taxas(50, 10, 500'000);
    h.run_until(1'000'000);
    const StatusView s = h.last_status();
    TEST_ASSERT_EQUAL(50, s.imu_rate);
    TEST_ASSERT_EQUAL(10, s.obd_rate);
    TEST_ASSERT_EQUAL(0, s.rejected);

    const size_t before = imu_count(link);
    h.run_until(2'000'000);
    TEST_ASSERT_EQUAL(50, imu_count(link) - before);  // 2 leituras por amostra
}

// Cada campo vale sozinho; cada valor fora da lista deixa o seu campo como estava e soma 1 em
// rejeitadas (PROTOCOLO.md §4.2).
static void test_each_taxas_field_stands_alone_and_a_value_off_the_list_is_rejected() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.start(0);

    struct Case {
        uint8_t imu, obd;           // o que a Pi manda
        uint8_t imu_in_use, obd_in_use;
        uint32_t rejected;          // a soma até aqui
    };
    const Case cases[] = {
        {33, 2, 25, 2, 1},          // a IMU fora da lista, o OBD muda mesmo assim
        {100, 0, 100, 2, 2},
        {0, 255, 100, 2, 4},        // os dois fora: soma 2
        {50, 1, 50, 1, 4},          // as outras taxas aceitas, uma por segundo
        {25, 5, 25, 5, 4},
        {20, 10, 20, 10, 4},
        {10, 10, 10, 10, 4},
        {30, 3, 10, 10, 6},
        {101, 4, 10, 10, 8},
        {5, 20, 10, 10, 10},
        {10, 10, 10, 10, 10},       // o mesmo valor de novo vale
    };
    uint64_t t = 100'000;
    for (const Case& c : cases) {
        h.taxas(c.imu, c.obd, t);
        h.run_until(t + 900'000);   // o STATUS do segundo seguinte
        const StatusView s = h.last_status();
        TEST_ASSERT_EQUAL(c.imu_in_use, s.imu_rate);
        TEST_ASSERT_EQUAL(c.obd_in_use, s.obd_rate);
        TEST_ASSERT_EQUAL(c.rejected, s.rejected);
        t += 1'000'000;
    }
}

// Como no ESP32 falso (DECODIFICADOR.md §4): um TAXAS curto soma 1 e não muda nada; bytes
// além dos dois campos são ignorados, como o corpo de um PING.
static void test_a_short_taxas_is_rejected_and_extra_bytes_are_ignored() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.start(0);

    const uint8_t one[] = {50};
    h.from_pi(MessageType::kTaxas, one, sizeof(one), 100'000);
    h.from_pi(MessageType::kTaxas, nullptr, 0, 200'000);
    h.run_until(1'000'000);
    StatusView s = h.last_status();
    TEST_ASSERT_EQUAL(25, s.imu_rate);
    TEST_ASSERT_EQUAL(5, s.obd_rate);
    TEST_ASSERT_EQUAL(2, s.rejected);

    const uint8_t three[] = {50, 10, 0xAA};
    h.from_pi(MessageType::kTaxas, three, sizeof(three), 1'100'000);
    h.run_until(2'000'000);
    s = h.last_status();
    TEST_ASSERT_EQUAL(50, s.imu_rate);
    TEST_ASSERT_EQUAL(10, s.obd_rate);
    TEST_ASSERT_EQUAL(2, s.rejected);
}

// Uma mensagem de desenvolvimento no cabo, como a IMU_SIM: tipo 0x83, boot 0, corpo 1 2 3.
struct DevWire {
    uint8_t bytes[kMaxWire];
    size_t len;
};

static DevWire dev_message(uint8_t type) {
    const uint8_t body[] = {1, 2, 3};
    DevWire wire{};
    wire.len = encode_message(static_cast<MessageType>(type), 0, 0, body, sizeof(body), wire.bytes);
    return wire;
}

// A 2ª entrada, a UART0 da fase 4 com a IMU simulada (PLATAFORMA.md §5): remonta as mensagens
// como a dos fios da Pi, mas só entrega as de desenvolvimento. O resto soma em rejeitadas.
static void test_dev_bytes_deliver_only_development_messages() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    FakeDevHandler handler;
    Harness h(can, link, imu, sys);
    TEST_ASSERT_TRUE(h.core().register_dev_handler(0x83, handler));
    h.start(0);

    const DevWire sim = dev_message(0x83);
    h.dev_bytes(sim.bytes, 4, 100'000);
    h.dev_bytes(sim.bytes + 4, sim.len - 4, 100'200);  // este pedaço tem o 0x00 final
    TEST_ASSERT_EQUAL(1, handler.delivered.size());
    const uint8_t body[] = {1, 2, 3};
    TEST_ASSERT_EQUAL(sizeof(body), handler.delivered[0].body.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, handler.delivered[0].body.data(), sizeof(body));
    TEST_ASSERT_EQUAL_UINT64(100'200, handler.delivered[0].t_us);

    // Por aqui, um PING não tem resposta e um TAXAS não muda nada.
    h.dev_bytes(kPingWire, sizeof(kPingWire), 200'000);
    uint8_t wire[kMaxWire];
    const uint8_t rates[] = {50, 10};
    h.dev_bytes(wire, encode_message(MessageType::kTaxas, 0, 1, rates, 2, wire), 300'000);
    // Nem um tipo sem registro, um do sentido ESP32 → Pi ou um CRC errado.
    const DevWire unknown = dev_message(0x90);
    h.dev_bytes(unknown.bytes, unknown.len, 400'000);
    const DevWire status = dev_message(0x01);
    h.dev_bytes(status.bytes, status.len, 500'000);
    uint8_t bad_crc[sizeof(kPingWire)];
    std::memcpy(bad_crc, kPingWire, sizeof(bad_crc));
    bad_crc[6] ^= 0x01;
    h.dev_bytes(bad_crc, sizeof(bad_crc), 600'000);
    // E um corpo que o registrado recusa.
    handler.accepts = false;
    h.dev_bytes(sim.bytes, sim.len, 700'000);
    h.run_until(1'000'000);

    const StatusView s = h.last_status();
    TEST_ASSERT_EQUAL(6, s.rejected);
    TEST_ASSERT_EQUAL(25, s.imu_rate);
    TEST_ASSERT_EQUAL(5, s.obd_rate);
    TEST_ASSERT_EQUAL(0, link.messages_of(MessageType::kPong).size());
    TEST_ASSERT_EQUAL(2, handler.delivered.size());
}

// As duas entradas são remontadas cada uma no seu leitor: pedaços intercalados não se misturam.
static void test_dev_bytes_and_pi_bytes_are_put_together_apart() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    FakeDevHandler handler;
    Harness h(can, link, imu, sys);
    TEST_ASSERT_TRUE(h.core().register_dev_handler(0x83, handler));
    h.start(0);

    const DevWire sim = dev_message(0x83);
    h.link_bytes(kPingWire, 4, 100'000);
    h.dev_bytes(sim.bytes, 4, 100'050);
    h.link_bytes(kPingWire + 4, sizeof(kPingWire) - 4, 100'100);
    h.dev_bytes(sim.bytes + 4, sim.len - 4, 100'150);
    h.run_until(1'000'000);

    const auto pongs = link.messages_of(MessageType::kPong);
    TEST_ASSERT_EQUAL(1, pongs.size());
    TEST_ASSERT_EQUAL(7, parse_pong(pongs[0]).seq_ping);
    TEST_ASSERT_EQUAL_UINT64(100'100, parse_pong(pongs[0]).t_us);
    TEST_ASSERT_EQUAL(1, handler.delivered.size());
    TEST_ASSERT_EQUAL_UINT64(100'150, handler.delivered[0].t_us);
    TEST_ASSERT_EQUAL(0, h.last_status().rejected);
}

static void test_core_never_allocates_memory() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;  // a IMU lê e manda amostras o tempo todo
    const size_t before = g_allocations;

    Harness h(can, link, imu, sys);
    h.start(0);
    h.taxas(10, 10, 100'000);  // 10/s, para os 30 s caberem nos 64 KB da UART falsa
    for (uint16_t s = 0; s < 30; ++s) {
        h.ping(s, s * 1'000'000ull + 400'000);
        for (int i = 0; i < 12; ++i) {  // 2 acima do limite, a cada segundo
            h.platform_log(LogLevel::kWarning, "aviso", s * 1'000'000ull + 500'000);
        }
    }
    h.run_until(30'000'000);

    TEST_ASSERT_EQUAL(before, g_allocations);
    TEST_ASSERT_EQUAL(30, link.messages_of(MessageType::kPong).size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_status_at_start_has_every_field);
    RUN_TEST(test_status_every_second_with_increasing_seq);
    RUN_TEST(test_status_called_late_goes_out_once_and_restarts_the_rhythm);
    RUN_TEST(test_ping_in_pieces_is_answered_with_the_instant_of_the_last_piece);
    RUN_TEST(test_pong_goes_ahead_of_status_and_seq_is_given_when_written);
    RUN_TEST(test_output_held_by_a_full_uart_is_retried_within_10_ms);
    RUN_TEST(test_full_queue_drops_the_oldest_status_without_spending_seq);
    RUN_TEST(test_wrong_crc_and_unknown_types_count_as_rejected);
    RUN_TEST(test_a_platform_error_goes_out_as_a_log_with_its_instant);
    RUN_TEST(test_taxas_changes_both_rates_and_the_status_shows_them);
    RUN_TEST(test_each_taxas_field_stands_alone_and_a_value_off_the_list_is_rejected);
    RUN_TEST(test_a_short_taxas_is_rejected_and_extra_bytes_are_ignored);
    RUN_TEST(test_dev_bytes_deliver_only_development_messages);
    RUN_TEST(test_dev_bytes_and_pi_bytes_are_put_together_apart);
    RUN_TEST(test_core_never_allocates_memory);
    return UNITY_END();
}
