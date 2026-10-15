#include "protocol.h"

#include "byte_writer.h"

namespace vguard {

size_t build_pong_body(uint16_t seq_ping, uint64_t t_us, uint8_t* out) {
    ByteWriter w(out);
    w.u16(seq_ping);
    w.u64(t_us);
    return w.size();
}

size_t build_status_body(const StatusBody& status, uint8_t* out) {
    ByteWriter w(out);
    w.u8(kProtocolVersion);
    w.u64(status.t_us);
    w.u8(static_cast<uint8_t>(status.platform));
    w.u8(status.sources);
    w.u8(static_cast<uint8_t>(status.reset_reason));
    w.u8(status.imu_rate);
    w.u8(status.obd_rate);
    w.u8(status.imu_state);
    w.u8(status.obd_state);
    for (uint8_t b : status.pids) {
        w.u8(b);
    }
    for (uint32_t c : status.counters) {
        w.u32(c);
    }
    w.u32(status.heap_min);
    for (size_t i = 0; i < kMaxFirmwareText && status.firmware[i] != '\0'; ++i) {
        w.u8(static_cast<uint8_t>(status.firmware[i]));
    }
    return w.size();
}

size_t build_imu_body(const ImuSampleBody& sample, uint8_t* out) {
    ByteWriter w(out);
    w.u64(sample.t_us);
    for (int16_t v : sample.mean) {
        w.i16(v);
    }
    for (int16_t v : sample.peak) {
        w.i16(v);
    }
    return w.size();
}

size_t build_obd_body(uint64_t t_us, const ObdValue* values, size_t n, uint8_t* out) {
    ByteWriter w(out);
    w.u64(t_us);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.u8(values[i].pid);
        w.u16(values[i].age_ms);
        w.u8(values[i].len);
        for (uint8_t b = 0; b < values[i].len; ++b) {
            w.u8(values[i].data[b]);
        }
    }
    return w.size();
}

size_t build_log_body(uint64_t t_us, LogLevel level, const char* text, uint8_t* out) {
    ByteWriter w(out);
    w.u64(t_us);
    w.u8(static_cast<uint8_t>(level));
    for (size_t i = 0; i < kMaxLogText && text[i] != '\0'; ++i) {
        w.u8(static_cast<uint8_t>(text[i]));
    }
    return w.size();
}

}  // namespace vguard
