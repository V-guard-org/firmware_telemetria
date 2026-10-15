// O portão da lista fechada (docs/PLATAFORMA.md §3.2, D2, ADR-0001). Fica entre o OBD e a
// porta CAN da plataforma: o OBD recebe o portão, nunca a porta. Ele repassa listen() e
// talk() e só deixa sair requisições do modo 01 para os endereços padronizados, e só com o
// transceptor liberado.
#pragma once

#include <cstdint>

#include "ports.h"

namespace vguard {

// Quem o portão avisa a cada quadro barrado, o que só acontece por defeito no código: o
// núcleo, que manda o LOG de erro e, com o OBD (#25), o leva a BLOQUEADO. released diz se o
// transceptor estava liberado; se não estava, o quadro pode até ser da lista.
class GateListener {
public:
    virtual void on_frame_blocked(const CanFrame& frame, bool released) = 0;

protected:
    ~GateListener() = default;
};

class ClosedListGate final : public CanPort {
public:
    ClosedListGate(CanPort& port, GateListener& listener) : port_(port), listener_(listener) {}

    void listen() override;
    void talk() override;
    // Só passa o quadro da lista fechada, entre um talk() e o listen() seguinte. Um quadro
    // barrado não chega à porta, soma 1 em blocked() e é passado ao GateListener.
    void transmit(const CanFrame& frame) override;
    ListenSummary listen_summary() const override { return port_.listen_summary(); }

    // Os quadros barrados desde o boot: o contador bloqueadas do STATUS.
    uint32_t blocked() const { return blocked_; }

private:
    CanPort& port_;
    GateListener& listener_;
    bool released_ = false;  // o transceptor começa silencioso
    uint32_t blocked_ = 0;
};

}  // namespace vguard
