#include "crc16.h"

#include <array>

namespace probe {
namespace {

// Tabela gerada em tempo de compilação (exercita constexpr do C++17).
constexpr std::array<std::uint16_t, 256> make_table() {
    std::array<std::uint16_t, 256> t{};
    for (std::uint16_t i = 0; i < 256; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(i << 8);
        for (int b = 0; b < 8; ++b) {
            if constexpr (true) {
                c = (c & 0x8000) ? static_cast<std::uint16_t>((c << 1) ^ 0x1021)
                                 : static_cast<std::uint16_t>(c << 1);
            }
        }
        t[i] = c;
    }
    return t;
}

constexpr auto kTable = make_table();
static_assert(kTable[1] == 0x1021, "tabela CRC inválida");

}  // namespace

std::uint16_t crc16(const std::uint8_t* data, std::size_t len, std::uint16_t crc) {
    for (std::size_t i = 0; i < len; ++i) {
        const auto idx = static_cast<std::uint8_t>((crc >> 8) ^ data[i]);
        crc = static_cast<std::uint16_t>((crc << 8) ^ kTable[idx]);
    }
    return crc;
}

}  // namespace probe
