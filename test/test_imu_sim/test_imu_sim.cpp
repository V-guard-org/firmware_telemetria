// A IMU simulada, montada no núcleo como no main e alimentada pela IMU_SIM que chega pela
// UART (costura 1 da spec da fase 1). Só se olha o que sai: as amostras e o STATUS.
#include <unity.h>

#include <cstdint>
#include <vector>

// O core.h e o imu_sim.h vêm antes do harness para o PlatformIO achar as bibliotecas.
#include "core.h"
#include "imu_sim.h"
#include "support/harness.h"
#include "support/imu_view.h"

using namespace vguard;
using namespace vguard::testing;

void setUp() {}
void tearDown() {}

// A IMU_SIM no cabo, montada pelo PROTOCOLO.md §4.3, à parte do código da IMU simulada:
// ax, ay, az em 0,01 m/s², gx, gy, gz em 0,1 °/s e a marca de falha.
struct ImuSimWire {
    uint8_t bytes[kMaxWire];
    size_t len;
};

static ImuSimWire imu_sim(const int16_t (&values)[6], uint8_t failing = 0, size_t body_len = 13) {
    uint8_t body[13];
    for (size_t i = 0; i < 6; ++i) {
        body[2 * i] = static_cast<uint8_t>(values[i] & 0xFF);
        body[2 * i + 1] = static_cast<uint8_t>((values[i] >> 8) & 0xFF);
    }
    body[12] = failing;
    ImuSimWire wire{};
    wire.len = encode_message(static_cast<MessageType>(0x83), 0, 0, body, body_len, wire.bytes);
    return wire;
}

static constexpr int16_t kStopped[6] = {0, 0, 981, 0, 0, 0};  // parado e nivelado

// Uma IMU_SIM a cada 10 ms, 5 ms antes de cada leitura, de from_us até antes de to_us.
static void send_every_10_ms(Harness& h, const ImuSimWire& wire, uint64_t from_us, uint64_t to_us) {
    for (uint64_t t = from_us; t < to_us; t += 10'000) {
        h.link_bytes(wire.bytes, wire.len, t);
    }
}

static std::vector<ImuView> imu_samples(const FakeLink& link) {
    std::vector<ImuView> out;
    for (const auto& msg : link.messages_of(MessageType::kImu)) {
        out.push_back(ImuView::parse(msg.body));
    }
    return out;
}

static void test_the_imu_sim_values_come_out_in_the_samples() {
    FakeCan can;
    FakeLink link;
    ImuSim imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    TEST_ASSERT_TRUE(h.core().register_dev_handler(ImuSim::kMessageType, imu));

    h.start(0);
    send_every_10_ms(h, imu_sim({150, -20, 981, 5, -12, 123}), 5'000, 1'000'000);
    h.run_until(1'000'000);

    const auto samples = imu_samples(link);
    TEST_ASSERT_EQUAL(25, samples.size());
    const int16_t expected[6] = {150, -20, 981, 5, -12, 123};  // o giro é a identidade até o T11
    TEST_ASSERT_EQUAL_INT16_ARRAY(expected, samples.back().mean, 6);
    TEST_ASSERT_EQUAL_INT16_ARRAY(expected, samples.back().peak, 6);
    const StatusView s = h.last_status();
    TEST_ASSERT_EQUAL(1, s.sources & 0x01);  // IMU simulada
    TEST_ASSERT_EQUAL(0, s.imu_state);
    TEST_ASSERT_EQUAL(0, s.imu_failures);
    TEST_ASSERT_EQUAL(0, s.rejected);
}

static void test_the_failure_mark_fails_the_readings_until_it_is_cleared() {
    FakeCan can;
    FakeLink link;
    ImuSim imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.core().register_dev_handler(ImuSim::kMessageType, imu);

    h.start(0);
    send_every_10_ms(h, imu_sim(kStopped), 5'000, 500'000);
    send_every_10_ms(h, imu_sim(kStopped, 1), 500'000, 3'000'000);  // "imu falha liga"
    h.run_until(3'000'000);
    const size_t samples_before = imu_samples(link).size();
    TEST_ASSERT_EQUAL(1, h.last_status().imu_state);  // os reinícios falham com a marca
    TEST_ASSERT_EQUAL(10, h.last_status().imu_failures);

    send_every_10_ms(h, imu_sim(kStopped), 3'000'000, 5'000'000);  // "imu falha desliga"
    h.run_until(5'000'000);
    TEST_ASSERT_EQUAL(0, h.last_status().imu_state);
    TEST_ASSERT_GREATER_THAN(samples_before + 20, imu_samples(link).size());
}

static void test_more_than_1_s_without_imu_sim_fails_the_readings() {
    FakeCan can;
    FakeLink link;
    ImuSim imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.core().register_dev_handler(ImuSim::kMessageType, imu);

    h.start(0);
    send_every_10_ms(h, imu_sim(kStopped), 5'000, 1'000'000);  // a última em 995 ms
    h.run_until(3'000'000);
    // Lê o último valor até 1 s depois dele (1,99 s); de 2,0 a 2,09 s, falha 10 vezes.
    TEST_ASSERT_EQUAL(1, h.last_status().imu_state);
    TEST_ASSERT_EQUAL(10, h.last_status().imu_failures);
    TEST_ASSERT_EQUAL_UINT64(1'980'000, imu_samples(link).back().t_us);  // 1,97 a 1,99 s

    h.run_until(4'000'000);  // sem IMU_SIM, os reinícios falham
    TEST_ASSERT_EQUAL(1, h.last_status().imu_state);

    send_every_10_ms(h, imu_sim(kStopped), 4'000'000, 6'000'000);
    h.run_until(6'000'000);
    TEST_ASSERT_EQUAL(0, h.last_status().imu_state);
}

static void test_at_start_it_waits_up_to_1_s_for_the_first_imu_sim() {
    // No notebook, o simulador abre a porta alguns milissegundos depois da partida.
    FakeCan can;
    FakeLink link;
    ImuSim imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.core().register_dev_handler(ImuSim::kMessageType, imu);

    h.start(0);
    TEST_ASSERT_EQUAL(0, h.last_status().imu_state);
    send_every_10_ms(h, imu_sim(kStopped), 35'000, 1'000'000);
    h.run_until(1'000'000);

    TEST_ASSERT_EQUAL(0, h.last_status().imu_state);
    TEST_ASSERT_EQUAL(3, h.last_status().imu_failures);  // as leituras de 10, 20 e 30 ms
    TEST_ASSERT_EQUAL(25, imu_samples(link).size());
}

static void test_without_any_imu_sim_it_stays_failed() {
    FakeCan can;
    FakeLink link;
    ImuSim imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.core().register_dev_handler(ImuSim::kMessageType, imu);

    h.start(0);
    h.run_until(10'000'000);

    TEST_ASSERT_EQUAL(1, h.last_status().imu_state);
    TEST_ASSERT_EQUAL(10, h.last_status().imu_failures);  // sem ir e voltar a cada reinício
    TEST_ASSERT_EQUAL(0, imu_samples(link).size());
}

static void test_an_imu_sim_of_the_wrong_size_is_rejected() {
    FakeCan can;
    FakeLink link;
    ImuSim imu;
    FakeSystem sys;
    Harness h(can, link, imu, sys);
    h.core().register_dev_handler(ImuSim::kMessageType, imu);

    h.start(0);
    const ImuSimWire short_one = imu_sim(kStopped, 0, 12);
    h.link_bytes(short_one.bytes, short_one.len, 5'000);
    h.run_until(1'000'000);

    TEST_ASSERT_EQUAL(1, h.last_status().rejected);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_imu_sim_values_come_out_in_the_samples);
    RUN_TEST(test_the_failure_mark_fails_the_readings_until_it_is_cleared);
    RUN_TEST(test_more_than_1_s_without_imu_sim_fails_the_readings);
    RUN_TEST(test_at_start_it_waits_up_to_1_s_for_the_first_imu_sim);
    RUN_TEST(test_without_any_imu_sim_it_stays_failed);
    RUN_TEST(test_an_imu_sim_of_the_wrong_size_is_rejected);
    return UNITY_END();
}
