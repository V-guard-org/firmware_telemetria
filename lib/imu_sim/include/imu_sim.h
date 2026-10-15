// A IMU simulada (docs/PLATAFORMA.md §5): a porta da IMU alimentada pela IMU_SIM (0x83) que o
// simulador do PC manda 100 vezes por segundo (docs/PROTOCOLO.md §4.3). Sai inteira quando a
// simulação for apagada (ADR-0003): esta pasta e as linhas que a montam nos main.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core.h"
#include "ports.h"

namespace vguard {

class ImuSim : public ImuPort, public DevMessageHandler {
public:
    static constexpr uint8_t kMessageType = 0x83;  // IMU_SIM

    // O último valor recebido, a menos que ele traga a marca de falha ou tenha mais de 1 s.
    bool read(ImuReading& out, uint64_t t_us) override;
    // Dá certo enquanto chegam IMU_SIM sem a marca de falha. Na partida, espera 1 s pela 1ª:
    // no notebook, o simulador abre a porta alguns milissegundos depois do firmware.
    bool restart(uint64_t t_us) override;
    bool simulated() const override { return true; }

    bool on_dev_message(const uint8_t* body, size_t len, uint64_t t_us) override;

private:
    bool silent(uint64_t t_us) const;

    ImuReading last_{};
    bool has_reading_ = false;
    bool failing_ = false;          // a marca de falha da última IMU_SIM
    uint64_t last_message_us_ = 0;  // começa na partida
};

}  // namespace vguard
