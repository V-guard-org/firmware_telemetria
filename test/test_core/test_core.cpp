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
// portão. O limite de 10 por segundo chega no #22.
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

static void test_core_never_allocates_memory() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;  // a IMU lê e manda amostras o tempo todo
    const size_t before = g_allocations;

    Harness h(can, link, imu, sys);
    h.start(0);
    for (uint16_t s = 0; s < 30; ++s) {
        h.ping(s, s * 1'000'000ull + 400'000);
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
    RUN_TEST(test_core_never_allocates_memory);
    return UNITY_END();
}
