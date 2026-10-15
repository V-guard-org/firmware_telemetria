#include "protocol.h"

#include <array>

namespace vguard {
namespace {

constexpr std::array<uint16_t, 256> make_table() {
    std::array<uint16_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        auto crc = static_cast<uint16_t>(i << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto kTable = make_table();

}  // namespace

uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc) {
    for (size_t i = 0; i < len; ++i) {
        crc = static_cast<uint16_t>((crc << 8) ^ kTable[static_cast<uint8_t>((crc >> 8) ^ data[i])]);
    }
    return crc;
}

}  // namespace vguard
