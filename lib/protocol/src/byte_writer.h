// Escrita de números em little-endian, como todos os do protocolo (PROTOCOLO.md §2).
#pragma once

#include <cstddef>
#include <cstdint>

namespace vguard {

class ByteWriter {
public:
    explicit ByteWriter(uint8_t* out) : out_(out) {}

    void u8(uint8_t v) { out_[size_++] = v; }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v >> 16));
    }
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }

    size_t size() const { return size_; }

private:
    uint8_t* out_;
    size_t size_ = 0;
};

}  // namespace vguard
