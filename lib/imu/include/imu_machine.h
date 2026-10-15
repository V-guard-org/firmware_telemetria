// A máquina da IMU (docs/MAQUINAS-DE-ESTADO.md §3) e a amostra com a média e o pico de cada
// eixo (docs/PROTOCOLO.md §3.2). O núcleo a chama nos prazos, com o instante; ela nunca
// pergunta a hora.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ports.h"
#include "protocol.h"

namespace vguard {

class ImuMachine {
public:
    // rate: amostras por segundo, uma das taxas do PROTOCOLO.md §4.2.
    ImuMachine(ImuPort& port, uint8_t rate);

    // INICIANDO: OK se a IMU respondeu ao reinício, FALHA se não.
    void start(uint64_t now_us);
    // A leitura ou o reinício que venceu. Devolve true quando a leitura fecha uma amostra.
    bool on_deadline(uint64_t now_us, ImuSampleBody& sample);
    uint64_t next_deadline_us() const;

    bool failed() const { return failed_; }
    uint32_t failures() const { return failures_; }  // falhas_imu

private:
    void become_ok(uint64_t now_us);
    void become_failed(uint64_t now_us);
    void add(const ImuReading& reading, uint64_t t_us);
    void fill(ImuSampleBody& sample) const;
    void clear_interval();

    ImuPort& port_;
    uint8_t rate_;
    bool failed_ = false;
    uint64_t next_read_us_ = 0;
    uint64_t next_restart_us_ = 0;
    uint32_t failures_ = 0;
    uint8_t failures_in_a_row_ = 0;

    // O intervalo da amostra em curso
    uint32_t reads_ = 0;
    uint32_t valid_ = 0;
    uint64_t sum_t_us_ = 0;
    double sum_[6] = {};
    float peak_[6] = {};
};

}  // namespace vguard
