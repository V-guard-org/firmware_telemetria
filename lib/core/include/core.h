// O núcleo portátil do firmware (docs/PLATAFORMA.md §2, ADR-0008). É passivo: só age
// quando a plataforma o chama, sempre com o instante do evento, e ao fim de cada chamada
// informa o seu próximo prazo. Nunca espera, nunca cria tarefas e nunca pergunta a hora.
#pragma once

#include <cstddef>
#include <cstdint>

#include "closed_list_gate.h"
#include "imu_machine.h"
#include "ports.h"
#include "protocol.h"

namespace vguard {

// Quem recebe uma mensagem de desenvolvimento, como a IMU simulada recebe a IMU_SIM
// (docs/PLATAFORMA.md §5). O núcleo não conhece a simulação: só entrega o corpo.
class DevMessageHandler {
public:
    // Devolve false se o corpo não serve; a mensagem soma 1 em rejeitadas.
    virtual bool on_dev_message(const uint8_t* body, size_t len, uint64_t t_us) = 0;

protected:
    ~DevMessageHandler() = default;
};

// O que é fixo durante o boot.
struct BootInfo {
    uint16_t boot;             // sorteado, de 1 a 65535
    ResetReason reset_reason;
    Platform platform;
    const char* firmware;      // versão do firmware; o núcleo guarda até 32 bytes
};

class Core : private GateListener {
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

    // Entrega as mensagens de desenvolvimento desse tipo (de 0x80 a 0xFF, menos as da Pi)
    // ao handler. Chamado na montagem, antes do start(); false se o tipo não serve ou se
    // já há kMaxDevHandlers registrados.
    bool register_dev_handler(uint8_t type, DevMessageHandler& handler);
    static constexpr size_t kMaxDevHandlers = 4;

private:
    bool push_byte(MessageReader& reader, uint8_t byte);
    void on_link_message(const Message& msg, uint64_t t_us);
    void apply_taxas(const Message& msg);
    bool deliver_dev_message(const Message& msg, uint64_t t_us);
    void send_status(uint64_t t_us);
    void send(MessageType type, const uint8_t* body, size_t len);
    void flush(uint64_t now_us);
    void send_log(LogLevel level, const char* text, uint64_t t_us);
    bool take_log_slot(uint64_t t_us);
    void on_frame_blocked(const CanFrame& frame, bool released) override;

    ClosedListGate gate_;  // o OBD (#25) recebe o portão, nunca a porta CAN
    LinkPort& link_;
    ImuPort& imu_;
    SystemPort& system_;
    ImuMachine imu_machine_;      // tem a taxa de envio da IMU em uso
    uint8_t obd_rate_;            // a do OBD, que a máquina do OBD (#25) segue

    uint16_t boot_;
    ResetReason reset_reason_;
    Platform platform_;
    char firmware_[kMaxFirmwareText + 1];

    MessageReader link_reader_;
    MessageReader dev_reader_;  // a UART0 da fase 4, à parte da Pi (on_dev_bytes)
    OutputQueue queue_;
    uint16_t tx_seq_ = 0;
    uint32_t counters_[kCounterCount] = {};
    uint64_t next_status_us_ = 0;
    uint64_t retry_write_us_ = UINT64_MAX;  // só enquanto a UART recusa a fila
    uint64_t now_us_ = 0;                   // o instante da chamada em curso

    // Os instantes dos últimos 10 LOGs que saíram; next_log_ aponta o mais antigo deles.
    static constexpr size_t kMaxLogsPerSecond = 10;
    uint64_t log_times_us_[kMaxLogsPerSecond];
    size_t next_log_ = 0;

    struct DevRegistration {
        uint8_t type;
        DevMessageHandler* handler;
    };
    DevRegistration dev_handlers_[kMaxDevHandlers] = {};
    size_t dev_handler_count_ = 0;
};

}  // namespace vguard
