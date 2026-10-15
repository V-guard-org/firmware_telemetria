#include "protocol.h"

#include <cstring>

namespace vguard {
namespace {

// Menor sai antes.
int rank(MessageType type) {
    switch (type) {
        case MessageType::kPong:
            return 0;
        case MessageType::kStatus:
            return 1;
        case MessageType::kLog:
            return 3;
        default:
            return 2;  // IMU e OBD, juntas, na ordem em que foram geradas
    }
}

}  // namespace

bool OutputQueue::push(MessageType type, const uint8_t* body, size_t body_len) {
    size_t free = kNone;
    for (size_t i = 0; i < kCapacity; ++i) {
        if (!slots_[i].used) {
            free = i;
            break;
        }
    }
    const bool full = free == kNone;
    if (full) {
        free = victim_index();
        if (rank(type) > rank(slots_[free].entry.type)) {
            return true;  // a nova é a que sai por último: ela mesma é descartada
        }
    }
    Slot& slot = slots_[free];
    slot.used = true;
    slot.order = next_order_++;
    slot.entry.type = type;
    slot.entry.body_len = body_len;
    if (body_len > 0) {
        std::memcpy(slot.entry.body, body, body_len);
    }
    return full;
}

const OutputQueue::Entry* OutputQueue::front() const {
    const size_t i = front_index();
    return i != kNone ? &slots_[i].entry : nullptr;
}

void OutputQueue::pop() {
    const size_t i = front_index();
    if (i != kNone) {
        slots_[i].used = false;
    }
}

// a sai antes de b: classe que sai antes, ou a mesma classe e chegou antes.
bool OutputQueue::leaves_before(const Slot& a, const Slot& b) {
    const int ra = rank(a.entry.type);
    const int rb = rank(b.entry.type);
    return ra < rb || (ra == rb && a.order < b.order);
}

size_t OutputQueue::front_index() const {
    size_t best = kNone;
    for (size_t i = 0; i < kCapacity; ++i) {
        if (slots_[i].used && (best == kNone || leaves_before(slots_[i], slots_[best]))) {
            best = i;
        }
    }
    return best;
}

// A mais antiga da classe que sai por último. Só é chamada com a fila cheia.
size_t OutputQueue::victim_index() const {
    size_t victim = 0;
    for (size_t i = 1; i < kCapacity; ++i) {
        const Slot& slot = slots_[i];
        const Slot& current = slots_[victim];
        if (rank(slot.entry.type) > rank(current.entry.type) ||
            (rank(slot.entry.type) == rank(current.entry.type) && slot.order < current.order)) {
            victim = i;
        }
    }
    return victim;
}

}  // namespace vguard
