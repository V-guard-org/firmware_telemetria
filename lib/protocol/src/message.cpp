#include "protocol.h"

#include <cstring>

namespace vguard {
namespace {

// O mesmo COBS do decodificador (host/esp32_decoder.py): um código antes de cada trecho
// sem zeros, 255 para um trecho de 254 bytes sem zero no fim. Não escreve o 0x00 final.
size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out) {
    size_t code_at = 0;
    size_t written = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; ++i) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = written++;
            code = 1;
            continue;
        }
        out[written++] = in[i];
        if (++code == 0xFF) {
            out[code_at] = code;
            code_at = written++;
            code = 1;
        }
    }
    out[code_at] = code;
    return written;
}

}  // namespace

size_t encode_message(MessageType type, uint16_t boot, uint16_t seq, const uint8_t* body,
                      size_t body_len, uint8_t* out) {
    uint8_t raw[kMaxMessage];
    raw[0] = static_cast<uint8_t>(type);
    raw[1] = static_cast<uint8_t>(boot);
    raw[2] = static_cast<uint8_t>(boot >> 8);
    raw[3] = static_cast<uint8_t>(seq);
    raw[4] = static_cast<uint8_t>(seq >> 8);
    if (body_len > 0) {
        std::memcpy(raw + kHeaderSize, body, body_len);
    }
    const size_t len = kHeaderSize + body_len;
    const uint16_t crc = crc16(raw, len);
    raw[len] = static_cast<uint8_t>(crc);
    raw[len + 1] = static_cast<uint8_t>(crc >> 8);
    const size_t encoded = cobs_encode(raw, len + kCrcSize, out);
    out[encoded] = 0x00;
    return encoded + 1;
}

}  // namespace vguard
