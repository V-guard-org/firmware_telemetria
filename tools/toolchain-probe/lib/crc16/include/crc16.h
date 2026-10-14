// Biblioteca portátil de teste do toolchain: CRC-16/CCITT-FALSE.
// Compila igual no notebook (env native) e no ESP32 (envs vguard e helper).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

static_assert(__cplusplus >= 201703L, "a biblioteca exige C++17 ou mais novo");

namespace probe {

// CRC-16/CCITT-FALSE: polinômio 0x1021, valor inicial 0xFFFF, sem reflexão, sem XOR final.
// Valor de verificação para "123456789": 0x29B1.
std::uint16_t crc16(const std::uint8_t* data, std::size_t len, std::uint16_t crc = 0xFFFF);

inline std::uint16_t crc16(std::string_view s) {
    return crc16(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

}  // namespace probe
