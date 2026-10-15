#include "protocol.h"

namespace vguard {
namespace {

constexpr size_t kMinMessage = kHeaderSize + kCrcSize;

// O inverso do COBS do decodificador, sem o 0x00 final. Devolve false se os bytes não
// são COBS: um código 0 ou um trecho que passa do fim.
bool cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t* out_len) {
    size_t written = 0;
    size_t i = 0;
    while (i < len) {
        const uint8_t code = in[i];
        const size_t end = i + code;
        if (code == 0 || end > len) {
            return false;
        }
        for (size_t j = i + 1; j < end; ++j) {
            out[written++] = in[j];
        }
        i = end;
        if (code < 0xFF && i < len) {
            out[written++] = 0;
        }
    }
    *out_len = written;
    return true;
}

}  // namespace

FrameReader::Result FrameReader::push(uint8_t byte) {
    if (byte == 0x00) {
        return finish();
    }
    if (encoded_len_ == kMaxEncoded) {
        overflowed_ = true;
        return Result::kPending;
    }
    encoded_[encoded_len_++] = byte;
    return Result::kPending;
}

FrameReader::Result FrameReader::finish() {
    const size_t len = encoded_len_;
    const bool overflowed = overflowed_;
    encoded_len_ = 0;
    overflowed_ = false;
    if (overflowed) {
        return Result::kRejected;
    }
    if (len == 0) {
        return Result::kPending;
    }

    size_t decoded_len = 0;
    if (!cobs_decode(encoded_, len, decoded_, &decoded_len) || decoded_len < kMinMessage ||
        decoded_len > kMaxMessage) {
        return Result::kRejected;
    }
    const size_t payload_len = decoded_len - kCrcSize;
    const auto crc = static_cast<uint16_t>(decoded_[payload_len] | (decoded_[payload_len + 1] << 8));
    if (crc != crc16(decoded_, payload_len)) {
        return Result::kRejected;
    }

    message_.type = static_cast<MessageType>(decoded_[0]);
    message_.boot = static_cast<uint16_t>(decoded_[1] | (decoded_[2] << 8));
    message_.seq = static_cast<uint16_t>(decoded_[3] | (decoded_[4] << 8));
    message_.body = decoded_ + kHeaderSize;
    message_.body_len = payload_len - kHeaderSize;
    return Result::kMessage;
}

}  // namespace vguard
