#include "closed_list_gate.h"

namespace vguard {
namespace {

constexpr uint32_t kFunctionalId = 0x7DF;      // a todas as centrais
constexpr uint32_t kFirstPhysicalId = 0x7E0;   // a uma central, de 0x7E0 a 0x7E7
constexpr uint32_t kLastPhysicalId = 0x7E7;
constexpr uint8_t kSingleFrameTwoBytes = 0x02;  // quadro único com 2 bytes úteis: o modo e o PID
constexpr uint8_t kMode01 = 0x01;

// A lista fechada (docs/TESTES.md §5): o PID, no byte 2, é livre, porque todo PID do modo 01
// só lê; os bytes 3 a 7 são preenchimento, e o byte 0 já diz que só 2 bytes valem.
bool on_closed_list(const CanFrame& frame) {
    const bool standard_id =
        frame.id == kFunctionalId || (frame.id >= kFirstPhysicalId && frame.id <= kLastPhysicalId);
    return !frame.extended && standard_id && !frame.remote && frame.dlc == 8 &&
           frame.data[0] == kSingleFrameTwoBytes && frame.data[1] == kMode01;
}

}  // namespace

void ClosedListGate::listen() {
    released_ = false;
    port_.listen();
}

void ClosedListGate::talk() {
    port_.talk();
    released_ = true;
}

void ClosedListGate::transmit(const CanFrame& frame) {
    if (!released_ || !on_closed_list(frame)) {
        ++blocked_;
        listener_.on_frame_blocked(frame, released_);
        return;
    }
    port_.transmit(frame);
}

}  // namespace vguard
