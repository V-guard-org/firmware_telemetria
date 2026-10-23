// A porta CAN do notebook: SocketCAN no canal do --can, o vcan0 na fase 1
// (docs/PLATAFORMA.md §3.1). Não há transceptor nem ACK: todo quadro "sai".
#pragma once

#include <cstdint>
#include <string>

#include "ports.h"

namespace vguard::native {

class SocketCan : public CanPort {
public:
    SocketCan() = default;
    SocketCan(const SocketCan&) = delete;
    SocketCan& operator=(const SocketCan&) = delete;
    ~SocketCan();

    // Abre o canal, já escutando. Em caso de erro, devolve false e escreve o motivo em error.
    bool open(const std::string& iface, std::string& error);
    int fd() const { return fd_; }

    // Passa a só contar os quadros lidos, todos eles, num resumo da escuta novo.
    void listen() override;
    // Filtro do socket (CAN_RAW_FILTER) em 0x7E8–0x7EF, e os quadros passam ao núcleo.
    void talk() override;
    void transmit(const CanFrame& frame) override;
    // As transições são os quadros lidos, e não há linha presa.
    ListenSummary listen_summary() const override { return {frames_, frames_, 0}; }

    // Lê um quadro do socket; false quando não há mais nada agora. Na escuta, o quadro só é
    // contado, e for_core fica false.
    bool read(CanFrame& frame, bool& for_core);

    // O resultado da última transmissão, entregue uma vez, na volta seguinte do laço: nenhum
    // adaptador chama o núcleo de dentro de uma chamada do núcleo.
    bool take_tx_result(TxResult& result, int& error);
    bool has_tx_result() const { return tx_pending_; }

private:
    void set_filter(bool answers_only);

    int fd_ = -1;
    bool listening_ = true;
    uint32_t frames_ = 0;
    bool tx_pending_ = false;
    TxResult tx_result_ = TxResult::kOk;
    int tx_error_ = 0;
};

}  // namespace vguard::native
