// A IMU, testada pelo núcleo, só pelo que sai nas portas falsas (costura 1 da spec da fase 1):
// as amostras na UART, o STATUS e as chamadas à porta da IMU.
#include <unity.h>

#include <optional>
#include <vector>

// O core.h vem antes do harness para o PlatformIO achar a lib/core (veja o test_core.cpp).
#include "core.h"
#include "support/harness.h"
#include "support/imu_view.h"

using namespace vguard;
using namespace vguard::testing;

void setUp() {}
void tearDown() {}

static ImuReading accel(float ax, float ay, float az) { return {{ax, ay, az}, {0, 0, 0}}; }

static std::vector<ImuView> imu_samples(const FakeLink& link) {
    std::vector<ImuView> out;
    for (const auto& msg : link.messages_of(MessageType::kImu)) {
        out.push_back(ImuView::parse(msg.body));
    }
    return out;
}

static void test_a_sample_has_the_mean_and_the_peak_of_its_4_readings() {
    // Os exemplos do PROTOCOLO.md §3.2, nas mesmas 4 leituras: um buraco em az e uma
    // frenagem em ax.
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    imu.script = {accel(-6.0f, 0, 9.8f), accel(-6.2f, 0, 25.1f), accel(-5.9f, 0, 9.6f),
                  accel(-6.1f, 0, 9.8f)};
    Harness h(can, link, imu, sys);

    h.start(0);
    h.run_until(40'000);  // leituras em 10, 20, 30 e 40 ms

    const auto samples = imu_samples(link);
    TEST_ASSERT_EQUAL(1, samples.size());
    TEST_ASSERT_EQUAL(-605, samples[0].mean[kAx]);   // −6,05 m/s²
    TEST_ASSERT_EQUAL(-620, samples[0].peak[kAx]);   // −6,2, com o sinal
    TEST_ASSERT_EQUAL(1358, samples[0].mean[kAz]);   // 13,575, arredondado
    TEST_ASSERT_EQUAL(2510, samples[0].peak[kAz]);
    TEST_ASSERT_EQUAL_UINT64(25'000, samples[0].t_us);  // o meio do intervalo
}

static void test_100_readings_a_second_make_25_samples() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    Harness h(can, link, imu, sys);

    h.start(0);
    TEST_ASSERT_EQUAL_UINT64(10'000, h.core().next_deadline_us());
    h.run_until(1'000'000);

    const auto samples = imu_samples(link);
    TEST_ASSERT_EQUAL(25, samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT64(25'000 + i * 40'000, samples[i].t_us);
        TEST_ASSERT_EQUAL(981, samples[i].mean[kAz]);  // parado e nivelado
        TEST_ASSERT_EQUAL(981, samples[i].peak[kAz]);
    }
    TEST_ASSERT_EQUAL(0, h.last_status().imu_state);
    TEST_ASSERT_EQUAL(0, h.last_status().imu_failures);
}

static void test_values_beyond_the_wire_range_stay_at_32767() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    imu.reading = {{400.0f, -400.0f, 9.81f}, {3300.0f, -3300.0f, 0}};
    Harness h(can, link, imu, sys);

    h.start(0);
    h.run_until(40'000);

    const ImuView s = imu_samples(link).at(0);
    TEST_ASSERT_EQUAL(32767, s.mean[kAx]);
    TEST_ASSERT_EQUAL(-32767, s.peak[kAy]);
    TEST_ASSERT_EQUAL(32767, s.mean[kGx]);
    TEST_ASSERT_EQUAL(-32767, s.peak[kGy]);
}

static void test_a_failed_reading_is_counted_and_left_out_of_the_sample() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    imu.script = {accel(1.0f, 0, 9.81f), std::nullopt, accel(3.0f, 0, 9.81f), std::nullopt};
    Harness h(can, link, imu, sys);

    h.start(0);
    h.run_until(1'000'000);

    const auto samples = imu_samples(link);
    TEST_ASSERT_EQUAL(25, samples.size());
    TEST_ASSERT_EQUAL(200, samples[0].mean[kAx]);           // a média das 2 válidas
    TEST_ASSERT_EQUAL(300, samples[0].peak[kAx]);
    TEST_ASSERT_EQUAL_UINT64(20'000, samples[0].t_us);      // o meio de 10 e 30 ms
    TEST_ASSERT_EQUAL(2, h.last_status().imu_failures);
    TEST_ASSERT_EQUAL(0, h.last_status().imu_state);
}

static void test_no_sample_when_every_reading_of_the_interval_fails() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    imu.script = {std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    Harness h(can, link, imu, sys);

    h.start(0);
    h.run_until(80'000);

    const auto samples = imu_samples(link);
    TEST_ASSERT_EQUAL(1, samples.size());                   // só a do 2º intervalo
    TEST_ASSERT_EQUAL_UINT64(65'000, samples[0].t_us);
}

static void test_10_failures_in_a_row_lead_to_failed_and_a_restart_each_second() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    imu.present = true;
    imu.script.assign(9, std::nullopt);                     // 9 seguidas não bastam
    imu.script.push_back(accel(0, 0, 9.81f));
    imu.script.insert(imu.script.end(), 10, std::nullopt);  // as leituras de 110 a 200 ms
    Harness h(can, link, imu, sys);

    h.start(0);
    h.run_until(1'000'000);
    const auto before = imu_samples(link);
    TEST_ASSERT_EQUAL(1, before.size());                    // de 90 a 120 ms, ainda em OK
    TEST_ASSERT_EQUAL_UINT64(100'000, before[0].t_us);
    TEST_ASSERT_EQUAL(1, h.last_status().imu_state);        // FALHA desde os 200 ms
    TEST_ASSERT_EQUAL(19, h.last_status().imu_failures);

    imu.present = false;                                    // os reinícios de 1,2 e 2,2 s falham
    h.run_until(3'000'000);
    TEST_ASSERT_EQUAL(3, imu.restarts);                     // o da partida e um por segundo
    TEST_ASSERT_EQUAL(1, imu_samples(link).size());
    TEST_ASSERT_EQUAL(1, h.last_status().imu_state);

    imu.present = true;                                     // o de 3,2 s dá certo
    h.run_until(4'000'000);
    const auto after = imu_samples(link);
    TEST_ASSERT_EQUAL(1 + 20, after.size());
    TEST_ASSERT_EQUAL_UINT64(3'225'000, after[1].t_us);
    TEST_ASSERT_EQUAL(0, h.last_status().imu_state);
    TEST_ASSERT_EQUAL(19, h.last_status().imu_failures);    // um reinício não é uma leitura
}

static void test_an_imu_that_does_not_answer_at_start_is_failed_from_the_first_status() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;  // ausente
    FakeSystem sys;
    Harness h(can, link, imu, sys);

    h.start(0);
    TEST_ASSERT_EQUAL(1, h.last_status().imu_state);
    h.run_until(2'500'000);

    TEST_ASSERT_EQUAL(3, imu.restarts);                     // em 0, 1 e 2 s
    TEST_ASSERT_EQUAL(0, imu_samples(link).size());
    TEST_ASSERT_EQUAL(0, h.last_status().imu_failures);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_sample_has_the_mean_and_the_peak_of_its_4_readings);
    RUN_TEST(test_100_readings_a_second_make_25_samples);
    RUN_TEST(test_values_beyond_the_wire_range_stay_at_32767);
    RUN_TEST(test_a_failed_reading_is_counted_and_left_out_of_the_sample);
    RUN_TEST(test_no_sample_when_every_reading_of_the_interval_fails);
    RUN_TEST(test_10_failures_in_a_row_lead_to_failed_and_a_restart_each_second);
    RUN_TEST(test_an_imu_that_does_not_answer_at_start_is_failed_from_the_first_status);
    return UNITY_END();
}
