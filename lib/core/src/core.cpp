#include "core.h"

#include <cstring>

namespace vguard {
namespace {

constexpr uint8_t kDefaultImuRate = 25;   // PROTOCOLO.md §4.2
constexpr uint8_t kDefaultObdRate = 5;
constexpr uint64_t kStatusPeriodUs = 1'000'000;
// Com a UART cheia, nenhum evento avisa quando ela volta a aceitar: o núcleo tenta de novo.
constexpr uint64_t kWriteRetryUs = 10'000;
constexpr uint64_t kNever = UINT64_MAX;

}  // namespace

Core::Core(CanPort&, LinkPort& link, ImuPort& imu, SystemPort& system, const BootInfo& boot)
    : link_(link),
      imu_(imu),
      system_(system),
      boot_(boot.boot),
      reset_reason_(boot.reset_reason),
      platform_(boot.platform) {
    std::strncpy(firmware_, boot.firmware, kMaxFirmwareText);
    firmware_[kMaxFirmwareText] = '\0';
}

void Core::start(uint64_t now_us) {
    send_status(now_us);
    next_status_us_ = now_us + kStatusPeriodUs;
    flush(now_us);
}

// O OBD chega no ticket "OBD no vcan0: o caminho feliz" (#25).
void Core::on_can_frame(const CanFrame&, uint64_t) {}
void Core::on_can_tx_result(TxResult, uint64_t) {}
void Core::on_can_bus_state(BusState, uint64_t) {}

// A 2ª entrada de bytes e os LOGs chegam no ticket da fila de saída, TAXAS e LOG (#22).
void Core::on_dev_bytes(const uint8_t*, size_t, uint64_t) {}
void Core::on_platform_log(LogLevel, const char*, uint64_t) {}

void Core::on_link_bytes(const uint8_t* data, size_t len, uint64_t t_us) {
    // Uma mensagem leva o instante do pedaço que contém o seu 0x00 final (PLATAFORMA.md §3.3).
    for (size_t i = 0; i < len; ++i) {
        switch (link_reader_.push(data[i])) {
            case MessageReader::Result::kMessage:
                on_link_message(link_reader_.message(), t_us);
                break;
            case MessageReader::Result::kRejected:
                ++counters_[kRejected];
                break;
            case MessageReader::Result::kPending:
                break;
        }
    }
    flush(t_us);
}

void Core::on_link_message(const Message& msg, uint64_t t_us) {
    switch (msg.type) {
        case MessageType::kPing: {
            uint8_t body[kMaxBody];
            send(MessageType::kPong, body, build_pong_body(msg.seq, t_us, body));
            break;
        }
        case MessageType::kTaxas:
            break;  // as taxas mudam no ticket da fila de saída, TAXAS e LOG (#22)
        default:
            ++counters_[kRejected];  // tipo desconhecido, ou mensagem de desenvolvimento sem registro
            break;
    }
}

void Core::on_deadline(uint64_t now_us) {
    if (now_us >= next_status_us_) {
        send_status(now_us);
        // Mantém o ritmo de 1 s; depois de um atraso maior que 1 s, recomeça a partir de agora.
        next_status_us_ += kStatusPeriodUs;
        if (next_status_us_ <= now_us) {
            next_status_us_ = now_us + kStatusPeriodUs;
        }
    }
    flush(now_us);
}

uint64_t Core::next_deadline_us() const {
    return next_status_us_ < retry_write_us_ ? next_status_us_ : retry_write_us_;
}

void Core::send_status(uint64_t t_us) {
    StatusBody status{};
    status.t_us = t_us;
    status.platform = platform_;
    status.sources = imu_.simulated() ? kSourceImuSimulated : 0;
    status.reset_reason = reset_reason_;
    status.imu_rate = kDefaultImuRate;
    status.obd_rate = kDefaultObdRate;
    for (size_t i = 0; i < kCounterCount; ++i) {
        status.counters[i] = counters_[i];
    }
    status.heap_min = system_.heap_min();
    status.firmware = firmware_;
    uint8_t body[kMaxBody];
    send(MessageType::kStatus, body, build_status_body(status, body));
}

void Core::send(MessageType type, const uint8_t* body, size_t len) {
    if (queue_.push(type, body, len)) {
        ++counters_[kDropped];
    }
}

// Escreve o que couber na UART, na ordem da fila. O seq só é dado aqui (PROTOCOLO.md §2.1).
void Core::flush(uint64_t now_us) {
    while (const OutputQueue::Entry* entry = queue_.front()) {
        uint8_t wire[kMaxWire];
        const size_t len = encode_message(entry->type, boot_, tx_seq_, entry->body,
                                          entry->body_len, wire);
        if (link_.tx_free() < len) {
            retry_write_us_ = now_us + kWriteRetryUs;
            return;
        }
        link_.write(wire, len);
        ++tx_seq_;
        queue_.pop();
    }
    retry_write_us_ = kNever;
}

}  // namespace vguard
