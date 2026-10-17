// O que sai pelo cabo quando algo aperta (costura 1 da spec da fase 1): a ordem e os
// descartes da fila de saída (PROTOCOLO.md §6.5), o seq que só se gasta ao escrever (§2.1)
// e o limite de 10 LOGs por segundo (§3.5). Só se olha o que sai na UART falsa.
// Roda no notebook com: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// O core.h vem antes do harness para o PlatformIO achar a lib/core.
#include "core.h"
#include "support/harness.h"
#include "support/imu_view.h"
#include "support/log_view.h"

using namespace vguard;
using namespace vguard::testing;

void setUp() {}
void tearDown() {}

// Com a IMU falsa presente, sai uma amostra a cada 40 ms (25/s): a de 40 ms tem as leituras de
// 10 a 40 ms, e o seu t_us, o meio do intervalo, é 25 ms.
static uint64_t sample_t_us(int n) {
    return n * 40'000ull - 15'000;
}

static void assert_seq_from_zero(const std::vector<SentMessage>& sent) {
    for (size_t i = 0; i < sent.size(); ++i) {
        TEST_ASSERT_EQUAL_MESSAGE(i, sent[i].seq, "um descarte não pode deixar lacuna de seq");
    }
}

static void test_pong_then_status_then_the_samples_in_order_and_the_log_last() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    Harness h(can, link, imu, sys);

    link.set_stuck(true);                                   // tudo fica na fila
    h.start(0);                                             // STATUS de 0 s
    h.run_until(45'000);                                    // amostra 1
    h.platform_log(LogLevel::kWarning, "antes da 2", 45'000);
    h.run_until(85'000);                                    // amostra 2, gerada depois do LOG
    h.ping(5, 85'000);                                      // PONG, o último a entrar
    TEST_ASSERT_EQUAL(0, link.messages().size());

    link.set_stuck(false);
    h.run_until(90'000);                                    // a leitura de 90 ms esvazia a fila

    const auto sent = link.messages();
    TEST_ASSERT_EQUAL(5, sent.size());
    TEST_ASSERT_EQUAL(MessageType::kPong, sent[0].type);
    TEST_ASSERT_EQUAL(MessageType::kStatus, sent[1].type);
    TEST_ASSERT_EQUAL(MessageType::kImu, sent[2].type);
    TEST_ASSERT_EQUAL_UINT64(sample_t_us(1), ImuView::parse(sent[2].body).t_us);
    TEST_ASSERT_EQUAL(MessageType::kImu, sent[3].type);
    TEST_ASSERT_EQUAL_UINT64(sample_t_us(2), ImuView::parse(sent[3].body).t_us);
    TEST_ASSERT_EQUAL(MessageType::kLog, sent[4].type);
    TEST_ASSERT_EQUAL_STRING("antes da 2", LogView::parse(sent[4].body).text.c_str());
    assert_seq_from_zero(sent);
}

static void test_a_full_queue_drops_the_oldest_log_first() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    Harness h(can, link, imu, sys);

    link.set_stuck(true);
    h.start(0);                                             // STATUS
    h.platform_log(LogLevel::kWarning, "a", 1'000);
    h.platform_log(LogLevel::kWarning, "b", 2'000);
    h.run_until(565'000);   // 14 amostras: com o STATUS e os 2 LOGs são 17, numa fila de 16

    link.set_stuck(false);
    h.run_until(570'000);

    const auto sent = link.messages();
    TEST_ASSERT_EQUAL(16, sent.size());
    TEST_ASSERT_EQUAL(MessageType::kStatus, sent[0].type);
    for (int n = 1; n <= 14; ++n) {
        TEST_ASSERT_EQUAL(MessageType::kImu, sent[n].type);
        TEST_ASSERT_EQUAL_UINT64(sample_t_us(n), ImuView::parse(sent[n].body).t_us);
    }
    TEST_ASSERT_EQUAL(MessageType::kLog, sent[15].type);
    TEST_ASSERT_EQUAL_STRING("b", LogView::parse(sent[15].body).text.c_str());  // o "a" saiu
    assert_seq_from_zero(sent);

    h.run_until(1'000'000);
    TEST_ASSERT_EQUAL(1, h.last_status().dropped);
}

static void test_then_the_oldest_sample_and_a_new_log_is_dropped_too() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    Harness h(can, link, imu, sys);

    link.set_stuck(true);
    h.start(0);
    h.run_until(645'000);   // 16 amostras e o STATUS: sai a amostra mais antiga
    h.platform_log(LogLevel::kError, "fila cheia de amostras", 645'000);  // sai o próprio LOG

    link.set_stuck(false);
    h.run_until(650'000);

    const auto sent = link.messages();
    TEST_ASSERT_EQUAL(16, sent.size());
    TEST_ASSERT_EQUAL(MessageType::kStatus, sent[0].type);
    for (int n = 2; n <= 16; ++n) {
        TEST_ASSERT_EQUAL(MessageType::kImu, sent[n - 1].type);
        TEST_ASSERT_EQUAL_UINT64(sample_t_us(n), ImuView::parse(sent[n - 1].body).t_us);
    }
    TEST_ASSERT_EQUAL(0, link.messages_of(MessageType::kLog).size());
    assert_seq_from_zero(sent);

    h.run_until(1'000'000);
    TEST_ASSERT_EQUAL(2, h.last_status().dropped);
}

static std::vector<std::string> log_texts(const FakeLink& link) {
    std::vector<std::string> out;
    for (const auto& msg : link.messages_of(MessageType::kLog)) {
        out.push_back(LogView::parse(msg.body).text);
    }
    return out;
}

// No máximo 10 em qualquer intervalo de 1 s, e não 10 por segundo do relógio: os 10 de 0,1 s
// ainda contam até 1,1 s.
static void test_at_most_10_logs_in_any_second_and_the_rest_count_as_dropped() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;  // sem IMU: só STATUS e LOG
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.start(0);

    char text[16];
    for (int i = 0; i < 15; ++i) {
        std::snprintf(text, sizeof(text), "rajada %d", i);
        h.platform_log(LogLevel::kWarning, text, 100'000);
    }
    h.platform_log(LogLevel::kError, "cedo demais", 1'099'999);
    h.platform_log(LogLevel::kError, "já pode", 1'100'000);
    h.run_until(2'000'000);

    std::vector<std::string> expected;
    for (int i = 0; i < 10; ++i) {
        expected.push_back("rajada " + std::to_string(i));
    }
    expected.push_back("já pode");
    const auto texts = log_texts(link);
    TEST_ASSERT_EQUAL(expected.size(), texts.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(expected[i].c_str(), texts[i].c_str());
    }
    TEST_ASSERT_EQUAL(6, h.last_status().dropped);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pong_then_status_then_the_samples_in_order_and_the_log_last);
    RUN_TEST(test_a_full_queue_drops_the_oldest_log_first);
    RUN_TEST(test_then_the_oldest_sample_and_a_new_log_is_dropped_too);
    RUN_TEST(test_at_most_10_logs_in_any_second_and_the_rest_count_as_dropped);
    return UNITY_END();
}
