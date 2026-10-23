// O OBD, pelo núcleo (costura 1 da spec da fase 1): só o que sai na porta CAN falsa, as
// amostras OBD e o STATUS (docs/MAQUINAS-DE-ESTADO.md §2). O carro falso responde às
// requisições como as centrais de um carro. Roda no notebook com: pio test -e native
#include <unity.h>

#include <cstdint>
#include <vector>

// O core.h vem antes do harness para o PlatformIO achar a lib/core (veja o test_core).
#include "core.h"
#include "support/harness.h"
#include "support/obd_view.h"

using namespace vguard;
using namespace vguard::testing;

namespace {

// Os 8 PIDs da D6, em ordem crescente.
const uint8_t kAllPids[] = {0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x11, 0x42};

// Uma central em 0x7E8 com os 8 PIDs, respondendo em 5 ms; uma escuta com tráfego.
struct OneCentralCar {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    FakeCar car;
    Harness h{can, link, imu, sys};

    OneCentralCar() {
        car.add_central(0x7E8, {0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x11, 0x42});
        can.summary = {250, 250, 0};
        h.connect(car);
    }
};

std::vector<ObdView> obd_samples(const FakeLink& link) {
    std::vector<ObdView> out;
    for (const SentMessage& m : link.messages_of(MessageType::kObd)) {
        out.push_back(ObdView::parse(m.body));
    }
    return out;
}

CanFrame answer(uint32_t id, std::initializer_list<uint8_t> bytes) {
    CanFrame f{id, false, false, 8, {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA}};
    uint8_t i = 0;
    for (uint8_t b : bytes) {
        f.data[i++] = b;
    }
    return f;
}

}  // namespace

void setUp() {}
void tearDown() {}

// O cenário do MAQUINAS-DE-ESTADO.md §4: ESPERANDO_PI → ESCUTANDO → DESCOBRINDO → LENDO, com
// valores em cerca de 1,2 s depois do 1º PING.
static void test_the_car_starts_with_the_pi_ready() {
    OneCentralCar t;
    t.h.start(0);
    t.h.run_until(2'500'000);
    TEST_ASSERT_EQUAL_STRING("", t.can.letters().c_str());  // sem a Pi, nada no CAN
    TEST_ASSERT_EQUAL(0, t.h.last_status().obd_state);      // ESPERANDO_PI

    t.h.ping(1, 2'500'000);
    TEST_ASSERT_EQUAL_STRING("L", t.can.letters().c_str());  // escuta 1 s, silencioso
    t.h.run_until(3'499'999);
    TEST_ASSERT_EQUAL_STRING("L", t.can.letters().c_str());
    TEST_ASSERT_EQUAL(1, t.h.last_status().obd_state);       // ESCUTANDO, no STATUS de 3 s

    t.h.run_until(3'500'000);
    TEST_ASSERT_EQUAL('T', t.can.call(1).what);
    TEST_ASSERT_EQUAL_UINT64(3'500'000, t.can.call(1).t_us);
    // A descoberta: 01 00, 01 20 e 01 40 a todas as centrais, um a cada 50 ms.
    t.h.run_until(3'649'999);
    const auto discovery = t.can.transmitted();
    TEST_ASSERT_EQUAL(3, discovery.size());
    const uint8_t maps[] = {0x00, 0x20, 0x40};
    for (size_t i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_HEX32(0x7DF, discovery[i].frame.id);
        const uint8_t request[8] = {0x02, 0x01, maps[i], 0, 0, 0, 0, 0};
        TEST_ASSERT_EQUAL_HEX8_ARRAY(request, discovery[i].frame.data, 8);
        TEST_ASSERT_EQUAL_UINT64(3'500'000 + 50'000 * i, discovery[i].t_us);
    }

    t.h.run_until(5'000'000);
    const StatusView s = t.h.last_status();
    TEST_ASSERT_EQUAL(3, s.obd_state);  // LENDO
    const uint8_t pids[12] = {0x18, 0x3A, 0x80, 0x01, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pids, s.pids, 12);
    TEST_ASSERT_TRUE(s.obd_sent > 3);
    TEST_ASSERT_EQUAL(s.obd_sent, s.obd_answered);
    TEST_ASSERT_EQUAL(0, s.obd_timeouts);
    TEST_ASSERT_EQUAL(0, s.blocked);

    // Valores em cerca de 1,2 s, a 5 amostras/s, em ordem crescente de PID.
    const auto samples = obd_samples(t.link);
    TEST_ASSERT_TRUE(samples.size() >= 2);
    TEST_ASSERT_TRUE(samples[0].t_us > 3'650'000 && samples[0].t_us <= 3'850'000);
    for (size_t i = 1; i < samples.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT64(samples[0].t_us + 200'000 * i, samples[i].t_us);
    }
    const ObdView& last = samples.back();
    TEST_ASSERT_EQUAL(8, last.values.size());
    for (size_t i = 0; i < 8; ++i) {
        TEST_ASSERT_EQUAL_HEX8(kAllPids[i], last.values[i].pid);
        TEST_ASSERT_TRUE(last.values[i].age_ms < 1'500);  // 1 s, e algum atraso na agenda
    }
}

// O instante em que o OBD entra em LENDO com uma central que responde em 5 ms: o 1º PING em
// 2,5 s, a escuta até 3,5 s e os três mapas, de 50 ms cada.
constexpr uint64_t kReadingFrom = 3'650'000;

// MAQUINAS-DE-ESTADO.md §2.4: uma requisição por vez, no mínimo 50 ms entre elas; RPM e
// velocidade a cada 200 ms, os outros a cada 1 s, sempre o PID mais atrasado.
static void test_the_agenda_asks_the_most_overdue_pid_one_at_a_time() {
    OneCentralCar t;
    t.h.start(0);
    t.h.ping(1, 2'500'000);
    t.h.run_until(kReadingFrom + 1'100'000);

    // No começo, todos vencem juntos e saem em ordem crescente; depois, cada um volta um
    // período depois da sua última pergunta. No empate, o de menor número.
    const uint8_t first_pids[] = {0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x11, 0x42,
                                  0x0C, 0x0D, 0x0C, 0x0D, 0x0C, 0x0D, 0x04, 0x0C};
    const uint64_t first_ms[] = {0, 50, 100, 150, 200, 250, 300, 350,
                                 400, 450, 600, 650, 800, 850, 1000, 1050};
    const auto first = t.can.transmitted(kReadingFrom);
    TEST_ASSERT_TRUE(first.size() >= 16);
    for (size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_HEX8(first_pids[i], first[i].frame.data[2]);
        TEST_ASSERT_EQUAL_UINT64(kReadingFrom + first_ms[i] * 1000, first[i].t_us);
        TEST_ASSERT_EQUAL_HEX32(0x7E0, first[i].frame.id);  // 0x7E8 menos 8
    }

    // Em regime, 10 s: cerca de 5/s de RPM e de velocidade, 1/s dos outros, nunca acima de 20/s.
    t.h.run_until(kReadingFrom + 12'000'000);
    const auto steady = t.can.transmitted(kReadingFrom + 2'000'000);
    size_t count[256] = {};
    uint64_t previous = 0;
    for (const FakeCan::Call& c : steady) {
        if (c.t_us >= kReadingFrom + 12'000'000) {
            break;
        }
        ++count[c.frame.data[2]];
        TEST_ASSERT_TRUE_MESSAGE(previous == 0 || c.t_us - previous >= 50'000,
                                 "duas requisições com menos de 50 ms");
        previous = c.t_us;
    }
    for (uint8_t pid : kAllPids) {
        const bool fast = pid == 0x0C || pid == 0x0D;
        TEST_ASSERT_UINT_WITHIN_MESSAGE(fast ? 5 : 1, fast ? 48 : 10, count[pid], "pedidos em 10 s");
    }
    const StatusView s = t.h.last_status();
    TEST_ASSERT_EQUAL(s.obd_sent, s.obd_answered);
    TEST_ASSERT_EQUAL(0, s.obd_timeouts);
}

// Com uma 2ª e uma 3ª central: cada PID vai à de menor endereço que o tem, pelo ID da resposta
// menos 8, e só vale a resposta dela (§2.3 e §2.4). O mapa do STATUS é a união.
static void test_each_pid_goes_to_the_lowest_central_that_has_it() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    FakeCar car;
    car.add_central(0x7E8, {0x04, 0x05, 0x0B, 0x0C, 0x0F, 0x11, 0x42});  // o motor, sem velocidade
    car.add_central(0x7E9, {0x0C, 0x0D});                                // o câmbio
    car.add_central(0x7EA, {0x0C, 0x42});
    car.value[0x0C][0] = 0x1A;
    car.value[0x0C][1] = 0xF8;
    car.value[0x0D][0] = 0x32;
    Harness h(can, link, imu, sys);
    h.connect(car);
    h.start(0);
    h.ping(1, 2'500'000);
    h.run_until(4'999'000);

    for (const FakeCan::Call& c : can.transmitted(kReadingFrom)) {
        const uint32_t expected = c.frame.data[2] == 0x0D ? 0x7E1 : 0x7E0;
        TEST_ASSERT_EQUAL_HEX32(expected, c.frame.id);
    }
    const uint8_t pids[12] = {0x18, 0x3A, 0x80, 0x01, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pids, h.last_status().pids, 12);

    // Logo antes da amostra de 5 s: RPM da central errada e velocidade da central errada.
    h.can_frame(answer(0x7E9, {0x04, 0x41, 0x0C, 0xDE, 0xAD}), 4'999'000);
    h.can_frame(answer(0x7E8, {0x03, 0x41, 0x0D, 0xFF}), 4'999'100);
    h.run_until(5'000'000);
    const ObdView sample = obd_samples(link).back();
    TEST_ASSERT_EQUAL_UINT64(5'000'000, sample.t_us);
    const uint8_t rpm[] = {0x1A, 0xF8};
    TEST_ASSERT_EQUAL(2, sample.find(0x0C)->raw.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, sample.find(0x0C)->raw.data(), 2);
    TEST_ASSERT_EQUAL(1, sample.find(0x0D)->raw.size());
    TEST_ASSERT_EQUAL_HEX8(0x32, sample.find(0x0D)->raw[0]);
}

// Uma resposta que chega depois dos 50 ms: a requisição conta como sem resposta e a agenda não
// espera por ela, mas o valor ainda vale (§2.4).
static void test_a_late_answer_still_counts_as_the_value_of_its_pid() {
    OneCentralCar t;
    t.car.delay_us[0x0C] = 70'000;
    t.h.start(0);
    t.h.ping(1, 2'500'000);
    t.h.run_until(8'000'000);

    const auto requests = t.can.transmitted(kReadingFrom);
    size_t rpm_requests = 0;
    const StatusView s = t.h.last_status();
    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].frame.data[2] != 0x0C) {
            continue;
        }
        if (requests[i].t_us + 50'000 <= s.t_us) {
            ++rpm_requests;
        }
        if (i + 1 < requests.size() && requests[i].t_us == kReadingFrom + 150'000) {
            // O 1º RPM: a próxima sai 50 ms depois, sem esperar a resposta.
            TEST_ASSERT_EQUAL_UINT64(kReadingFrom + 200'000, requests[i + 1].t_us);
        }
    }
    TEST_ASSERT_TRUE(rpm_requests > 10);
    TEST_ASSERT_EQUAL(rpm_requests, s.obd_timeouts);

    const ObdView last = obd_samples(t.link).back();
    const ObdView::Value* rpm = last.find(0x0C);
    TEST_ASSERT_NOT_NULL(rpm);
    TEST_ASSERT_TRUE(rpm->age_ms < 300);
    TEST_ASSERT_EQUAL(8, last.values.size());
}

// Uma resposta em vários quadros é ignorada: pedir a continuação exigiria um quadro de controle
// de fluxo, fora da lista fechada (§2.4). A requisição esgota o prazo, e o PID não aparece.
static void test_an_answer_in_several_frames_is_ignored() {
    OneCentralCar t;
    t.car.multi_frame[0x42] = true;
    t.h.start(0);
    t.h.ping(1, 2'500'000);
    t.h.run_until(8'000'000);

    const StatusView s = t.h.last_status();
    TEST_ASSERT_TRUE(s.obd_timeouts >= 4);  // uma pergunta por segundo
    size_t voltage_requests = 0;
    for (const FakeCan::Call& c : t.can.transmitted(kReadingFrom)) {
        voltage_requests += c.frame.data[2] == 0x42 && c.t_us + 50'000 <= s.t_us;
    }
    TEST_ASSERT_EQUAL(voltage_requests, s.obd_timeouts);
    for (const ObdView& sample : obd_samples(t.link)) {
        TEST_ASSERT_NULL(sample.find(0x42));
        if (sample.t_us >= kReadingFrom + 400'000) {  // depois da 1ª volta pela agenda
            TEST_ASSERT_EQUAL(7, sample.values.size());
        }
    }
}

// Um carro sem nenhum PID da lista: LENDO com a agenda vazia, sem requisições nem amostras, e
// o STATUS com o mapa do carro (§2.3, passo 5).
static void test_a_car_without_any_pid_of_the_list_reads_nothing() {
    FakeCan can;
    FakeLink link;
    FakeImu imu;
    FakeSystem sys;
    FakeCar car;
    car.add_central(0x7E8, {0x01, 0x03, 0x1C});
    Harness h(can, link, imu, sys);
    h.connect(car);
    h.start(0);
    h.ping(1, 2'500'000);
    h.run_until(10'000'000);

    TEST_ASSERT_EQUAL(1, can.transmitted().size());  // só o 01 00: o mapa não anuncia o 0x20
    const StatusView s = h.last_status();
    TEST_ASSERT_EQUAL(3, s.obd_state);
    const uint8_t pids[12] = {0xA0, 0x00, 0x00, 0x10};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pids, s.pids, 12);
    TEST_ASSERT_EQUAL(0, link.messages_of(MessageType::kObd).size());
}

// A amostra do PROTOCOLO.md §3.3: os bytes crus de cada resposta e a idade desde ela, na taxa de
// envio em uso; a idade trava em 65535.
static void test_the_sample_carries_the_raw_bytes_and_their_age_at_the_rate_in_use() {
    OneCentralCar t;
    t.h.start(0);
    t.h.ping(1, 2'500'000);
    t.h.run_until(6'000'000);

    // O carro falso responde a cada PID com o próprio número e, nos de 2 bytes, 0x01.
    const ObdView at_6s = obd_samples(t.link).back();
    TEST_ASSERT_EQUAL_UINT64(6'000'000, at_6s.t_us);
    const uint8_t sizes[] = {1, 1, 1, 2, 1, 1, 1, 2};
    for (size_t i = 0; i < 8; ++i) {
        const ObdView::Value& v = at_6s.values[i];
        TEST_ASSERT_EQUAL(sizes[i], v.raw.size());
        TEST_ASSERT_EQUAL_HEX8(kAllPids[i], v.raw[0]);
        if (sizes[i] == 2) {
            TEST_ASSERT_EQUAL_HEX8(0x01, v.raw[1]);
        }
    }
    // A idade do RPM: da resposta à última pergunta, 5 ms depois dela, até a amostra.
    uint64_t last_rpm = 0;
    for (const FakeCan::Call& c : t.can.transmitted(kReadingFrom)) {
        if (c.frame.data[2] == 0x0C && c.t_us + 5'000 <= 6'000'000) {
            last_rpm = c.t_us;
        }
    }
    TEST_ASSERT_EQUAL((6'000'000 - (last_rpm + 5'000)) / 1000, at_6s.find(0x0C)->age_ms);

    // A 10 amostras/s, e depois a 1/s.
    t.h.taxas(25, 10, 6'050'000);
    t.h.run_until(7'000'000);
    auto samples = obd_samples(t.link);
    TEST_ASSERT_EQUAL_UINT64(6'100'000, samples[samples.size() - 10].t_us);  // já na taxa nova
    TEST_ASSERT_EQUAL_UINT64(6'900'000, samples[samples.size() - 2].t_us);
    TEST_ASSERT_EQUAL_UINT64(7'000'000, samples.back().t_us);
    t.h.taxas(25, 1, 7'050'000);
    t.h.run_until(10'000'000);
    samples = obd_samples(t.link);
    TEST_ASSERT_EQUAL_UINT64(8'000'000, samples[samples.size() - 3].t_us);
    TEST_ASSERT_EQUAL_UINT64(9'000'000, samples[samples.size() - 2].t_us);
    TEST_ASSERT_EQUAL_UINT64(10'000'000, samples.back().t_us);

    // O notebook dormiu 70 s: a amostra seguinte traz os valores de antes, com a idade travada.
    t.h.late_deadline(80'000'000);
    const ObdView late = obd_samples(t.link).back();
    TEST_ASSERT_EQUAL_UINT64(80'000'000, late.t_us);
    TEST_ASSERT_EQUAL(8, late.values.size());
    for (const ObdView::Value& v : late.values) {
        TEST_ASSERT_EQUAL(65535, v.age_ms);
    }
}

// A escuta antes de falar (§2.1): com o barramento mudo, o OBD libera o transceptor e pergunta;
// com atividade sem quadros válidos, ou com a linha presa, nunca libera.
static void test_the_listen_decides_whether_the_obd_may_talk() {
    struct Case {
        ListenSummary summary;
        uint8_t state;
        const char* calls;
    };
    const Case cases[] = {
        {{0, 0, 0}, 3, "LTX"},          // mudo: DESCOBRINDO, e a 1ª requisição confirma
        {{0, 900, 0}, 5, "L"},          // outra velocidade: VELOCIDADE_ERRADA
        {{0, 3, 15'000}, 6, "L"},       // dominante por 15 ms: LINHA_PRESA
        {{120, 120, 10'000}, 3, "LTX"}, // 10 ms ainda não é linha presa
    };
    for (const Case& c : cases) {
        FakeCan can;
        FakeLink link;
        FakeImu imu;
        FakeSystem sys;
        FakeCar car;
        car.add_central(0x7E8, {0x0C});
        can.summary = c.summary;
        Harness h(can, link, imu, sys);
        h.connect(car);
        h.start(0);
        h.ping(1, 500'000);
        h.run_until(30'000'000);

        TEST_ASSERT_EQUAL(c.state, h.last_status().obd_state);
        TEST_ASSERT_EQUAL_STRING(c.calls, can.letters().substr(0, 3).c_str());
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_car_starts_with_the_pi_ready);
    RUN_TEST(test_the_agenda_asks_the_most_overdue_pid_one_at_a_time);
    RUN_TEST(test_each_pid_goes_to_the_lowest_central_that_has_it);
    RUN_TEST(test_a_late_answer_still_counts_as_the_value_of_its_pid);
    RUN_TEST(test_an_answer_in_several_frames_is_ignored);
    RUN_TEST(test_a_car_without_any_pid_of_the_list_reads_nothing);
    RUN_TEST(test_the_sample_carries_the_raw_bytes_and_their_age_at_the_rate_in_use);
    RUN_TEST(test_the_listen_decides_whether_the_obd_may_talk);
    return UNITY_END();
}
