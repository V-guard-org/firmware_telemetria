#include "protocol.h"

#include <cstring>

#include "byte_writer.h"

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
    ByteWriter w(raw);
    w.u8(static_cast<uint8_t>(type));
    w.u16(boot);
    w.u16(seq);
    if (body_len > 0) {
        std::memcpy(raw + kHeaderSize, body, body_len);
    }
    const size_t len = kHeaderSize + body_len;
    ByteWriter crc(raw + len);
    crc.u16(crc16(raw, len));
    const size_t encoded = cobs_encode(raw, len + kCrcSize, out);
    out[encoded] = 0x00;
    return encoded + 1;
}

}  // namespace vguard
