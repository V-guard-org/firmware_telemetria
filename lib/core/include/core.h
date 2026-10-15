// O núcleo portátil do firmware (docs/PLATAFORMA.md §2, ADR-0008). É passivo: só age
// quando a plataforma o chama, sempre com o instante do evento, e ao fim de cada chamada
// informa o seu próximo prazo. Nunca espera, nunca cria tarefas e nunca pergunta a hora.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ports.h"
#include "protocol.h"

namespace vguard {

enum class LogLevel : uint8_t { kError = 1, kWarning = 2 };  // o nível da mensagem LOG

// O que é fixo durante o boot.
struct BootInfo {
    uint16_t boot;             // sorteado, de 1 a 65535
    ResetReason reset_reason;
    Platform platform;
    const char* firmware;      // versão do firmware; o núcleo guarda até 32 bytes
};

class Core {
public:
    Core(CanPort& can, LinkPort& link, ImuPort& imu, SystemPort& system, const BootInfo& boot);
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // As entradas do PLATAFORMA.md §2.1. Cada chamada traz o instante do evento.
    void start(uint64_t now_us);
    void on_can_frame(const CanFrame& frame, uint64_t t_us);
    void on_can_tx_result(TxResult result, uint64_t t_us);
    void on_can_bus_state(BusState state, uint64_t t_us);
    void on_link_bytes(const uint8_t* data, size_t len, uint64_t t_us);
    void on_dev_bytes(const uint8_t* data, size_t len, uint64_t t_us);  // fase 4, UART0 (§5)
    void on_platform_log(LogLevel level, const char* text, uint64_t t_us);
    void on_deadline(uint64_t now_us);

    // O instante mais próximo em que o núcleo quer ser chamado de novo.
    uint64_t next_deadline_us() const;

private:
    void on_link_message(const Message& msg, uint64_t t_us);
    void send_status(uint64_t t_us);
    void send(MessageType type, const uint8_t* body, size_t len);
    void flush(uint64_t now_us);

    LinkPort& link_;
    ImuPort& imu_;
    SystemPort& system_;

    uint16_t boot_;
    ResetReason reset_reason_;
    Platform platform_;
    char firmware_[kMaxFirmwareText + 1];

    MessageReader link_reader_;
    OutputQueue queue_;
    uint16_t tx_seq_ = 0;
    uint32_t counters_[kCounterCount] = {};
    uint64_t next_status_us_ = 0;
    uint64_t retry_write_us_ = UINT64_MAX;  // só enquanto a UART recusa a fila
};

}  // namespace vguard
