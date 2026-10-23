#include "core.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace vguard {
namespace {

constexpr uint8_t kDefaultImuRate = 25;   // PROTOCOLO.md §4.2
constexpr uint8_t kDefaultObdRate = 5;
constexpr uint8_t kImuRates[] = {100, 50, 25, 20, 10};
constexpr uint8_t kObdRates[] = {1, 2, 5, 10};
constexpr uint64_t kStatusPeriodUs = 1'000'000;
// Com a UART cheia, nenhum evento avisa quando ela volta a aceitar: o núcleo tenta de novo.
constexpr uint64_t kWriteRetryUs = 10'000;
constexpr uint64_t kLogWindowUs = 1'000'000;  // no máximo 10 LOGs em qualquer intervalo de 1 s
constexpr uint64_t kNever = UINT64_MAX;

template <size_t N>
bool one_of(uint8_t value, const uint8_t (&accepted)[N]) {
    for (const uint8_t a : accepted) {
        if (value == a) {
            return true;
        }
    }
    return false;
}

}  // namespace

Core::Core(CanPort& can, LinkPort& link, ImuPort& imu, SystemPort& system, const BootInfo& boot)
    : gate_(can, *this),
      obd_(gate_, kDefaultObdRate),
      link_(link),
      imu_(imu),
      system_(system),
      imu_machine_(imu, kDefaultImuRate),
      boot_(boot.boot),
      reset_reason_(boot.reset_reason),
      platform_(boot.platform) {
    std::strncpy(firmware_, boot.firmware, kMaxFirmwareText);
    firmware_[kMaxFirmwareText] = '\0';
    for (uint64_t& t : log_times_us_) {
        t = kNever;
    }
}

void Core::start(uint64_t now_us) {
    now_us_ = now_us;
    imu_machine_.start(now_us);  // antes do STATUS, que já mostra o estado da IMU
    send_status(now_us);
    next_status_us_ = now_us + kStatusPeriodUs;
    flush(now_us);
}

void Core::on_can_frame(const CanFrame& frame, uint64_t t_us) {
    now_us_ = t_us;
    obd_.on_frame(frame, t_us);
}

void Core::on_can_tx_result(TxResult result, uint64_t t_us) {
    now_us_ = t_us;
    obd_.on_tx_result(result, t_us);
}

// O estado do barramento chega com as falhas do CAN (#26).
void Core::on_can_bus_state(BusState, uint64_t t_us) { now_us_ = t_us; }

// A UART0 da fase 4, com a IMU simulada e a Pi nos fios (PLATAFORMA.md §5). Remonta como a
// entrada da Pi, no seu próprio leitor, mas só entrega as mensagens de desenvolvimento: um
// PING ou um TAXAS por aqui soma em rejeitadas.
void Core::on_dev_bytes(const uint8_t* data, size_t len, uint64_t t_us) {
    now_us_ = t_us;
    for (size_t i = 0; i < len; ++i) {
        if (push_byte(dev_reader_, data[i]) && !deliver_dev_message(dev_reader_.message(), t_us)) {
            ++counters_[kRejected];
        }
    }
}

void Core::on_platform_log(LogLevel level, const char* text, uint64_t t_us) {
    now_us_ = t_us;
    send_log(level, text, t_us);
    flush(t_us);
}

void Core::on_link_bytes(const uint8_t* data, size_t len, uint64_t t_us) {
    now_us_ = t_us;
    // Uma mensagem leva o instante do pedaço que contém o seu 0x00 final (PLATAFORMA.md §3.3).
    for (size_t i = 0; i < len; ++i) {
        if (push_byte(link_reader_, data[i])) {
            on_link_message(link_reader_.message(), t_us);
        }
    }
    flush(t_us);
}

// true quando o byte fecha uma mensagem. Uma que a moldura rejeita soma em rejeitadas.
bool Core::push_byte(MessageReader& reader, uint8_t byte) {
    switch (reader.push(byte)) {
        case MessageReader::Result::kMessage:
            return true;
        case MessageReader::Result::kRejected:
            ++counters_[kRejected];
            return false;
        case MessageReader::Result::kPending:
            return false;
    }
    return false;
}

void Core::on_link_message(const Message& msg, uint64_t t_us) {
    switch (msg.type) {
        case MessageType::kPing: {
            uint8_t body[kMaxBody];
            send(MessageType::kPong, body, build_pong_body(msg.seq, t_us, body));
            obd_.on_ping(t_us);
            break;
        }
        case MessageType::kTaxas:
            apply_taxas(msg, t_us);
            break;
        default:
            if (!deliver_dev_message(msg, t_us)) {
                ++counters_[kRejected];  // tipo desconhecido, ou mensagem de desenvolvimento sem registro
            }
            break;
    }
}

// Cada campo vale sozinho: um valor fora da lista deixa o seu campo como estava e soma 1 em
// rejeitadas (PROTOCOLO.md §4.2). Um corpo curto soma 1; bytes além dos dois campos são
// ignorados, como o corpo de um PING.
void Core::apply_taxas(const Message& msg, uint64_t t_us) {
    if (msg.body_len < 2) {
        ++counters_[kRejected];
        return;
    }
    const uint8_t imu = msg.body[0];
    const uint8_t obd = msg.body[1];
    if (one_of(imu, kImuRates)) {
        imu_machine_.set_rate(imu);
    } else {
        ++counters_[kRejected];
    }
    if (one_of(obd, kObdRates)) {
        obd_.set_rate(obd, t_us);
    } else {
        ++counters_[kRejected];
    }
}

bool Core::register_dev_handler(uint8_t type, DevMessageHandler& handler) {
    const auto as_type = static_cast<MessageType>(type);
    if (type < 0x80 || as_type == MessageType::kPing || as_type == MessageType::kTaxas ||
        dev_handler_count_ == kMaxDevHandlers) {
        return false;
    }
    dev_handlers_[dev_handler_count_++] = {type, &handler};
    return true;
}

bool Core::deliver_dev_message(const Message& msg, uint64_t t_us) {
    for (size_t i = 0; i < dev_handler_count_; ++i) {
        if (dev_handlers_[i].type == static_cast<uint8_t>(msg.type)) {
            return dev_handlers_[i].handler->on_dev_message(msg.body, msg.body_len, t_us);
        }
    }
    return false;
}

void Core::on_deadline(uint64_t now_us) {
    now_us_ = now_us;
    ImuSampleBody sample;
    if (imu_machine_.on_deadline(now_us, sample)) {
        uint8_t body[kMaxBody];
        send(MessageType::kImu, body, build_imu_body(sample, body));
    }
    ObdSample obd;
    if (obd_.on_deadline(now_us, obd)) {
        uint8_t body[kMaxBody];
        send(MessageType::kObd, body, build_obd_body(obd.t_us, obd.values, obd.n, body));
    }
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
    const uint64_t next = next_status_us_ < retry_write_us_ ? next_status_us_ : retry_write_us_;
    const uint64_t imu = imu_machine_.next_deadline_us();
    const uint64_t obd = obd_.next_deadline_us();
    const uint64_t machines = imu < obd ? imu : obd;
    return machines < next ? machines : next;
}

void Core::send_status(uint64_t t_us) {
    StatusBody status{};
    status.t_us = t_us;
    status.platform = platform_;
    status.sources = imu_.simulated() ? kSourceImuSimulated : 0;
    status.reset_reason = reset_reason_;
    status.imu_rate = imu_machine_.rate();
    status.obd_rate = obd_.rate();
    status.imu_state = imu_machine_.failed() ? 1 : 0;
    status.obd_state = static_cast<uint8_t>(obd_.state());
    std::memcpy(status.pids, obd_.pids(), sizeof(status.pids));
    counters_[kImuFailures] = imu_machine_.failures();
    counters_[kObdSent] = obd_.sent();
    counters_[kObdAnswered] = obd_.answered();
    counters_[kObdTimeouts] = obd_.timeouts();
    for (size_t i = 0; i < kCounterCount; ++i) {
        status.counters[i] = counters_[i];
    }
    status.counters[kBlocked] = gate_.blocked();  // quem conta os quadros barrados é o portão
    status.heap_min = system_.heap_min();
    status.firmware = firmware_;
    uint8_t body[kMaxBody];
    send(MessageType::kStatus, body, build_status_body(status, body));
}

// Todo LOG passa por aqui, os da plataforma e os do próprio núcleo. Acima do limite, ele é
// descartado e soma em descartadas (PROTOCOLO.md §3.5).
void Core::send_log(LogLevel level, const char* text, uint64_t t_us) {
    if (!take_log_slot(t_us)) {
        ++counters_[kDropped];
        return;
    }
    uint8_t body[kMaxBody];
    send(MessageType::kLog, body, build_log_body(t_us, level, text, body));
}

// Um LOG passa pelo limite se o 10º antes dele passou há pelo menos 1 s, e então ocupa a vez.
// Conta o que passou, mesmo que a fila cheia o descarte depois.
bool Core::take_log_slot(uint64_t t_us) {
    uint64_t& tenth_before = log_times_us_[next_log_];
    if (tenth_before != kNever && t_us - tenth_before < kLogWindowUs) {
        return false;
    }
    tenth_before = t_us;
    next_log_ = (next_log_ + 1) % kMaxLogsPerSecond;
    return true;
}

// Um quadro fora da lista fechada, ou fora da liberação, que o portão barrou: só por defeito
// no código (PLATAFORMA.md §3.2). O OBD vai a BLOQUEADO, e sai um LOG de erro.
void Core::on_frame_blocked(const CanFrame& frame, bool released) {
    obd_.lock();
    // No máximo cerca de 105 bytes, sempre abaixo dos 200 do LOG.
    char text[kMaxLogText + 1];
    size_t len = static_cast<size_t>(std::snprintf(
        text, sizeof(text), "closed list: blocked %s%s frame 0x%" PRIX32 " dlc %u%s:",
        frame.extended ? "29-bit" : "11-bit", frame.remote ? " remote" : "", frame.id, frame.dlc,
        released ? "" : ", transceiver silent"));
    const uint8_t bytes = frame.dlc < 8 ? frame.dlc : 8;
    for (uint8_t i = 0; i < bytes; ++i) {
        len += static_cast<size_t>(
            std::snprintf(text + len, sizeof(text) - len, " %02X", frame.data[i]));
    }
    send_log(LogLevel::kError, text, now_us_);
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
