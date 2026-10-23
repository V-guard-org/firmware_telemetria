// Firmware nativo da fase 1 (docs/PLATAFORMA.md §4.2): o núcleo num laço com ppoll() no
// socket CAN e no pty, com o próximo prazo do núcleo como limite de espera.
//
//   pio run -e native && .pio/build/native/program [--can vcan0] [--path /tmp/vguard-esp32]
#include <poll.h>
#include <signal.h>
#include <time.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "core.h"
#include "imu_sim.h"
#include "pty_link.h"
#include "socket_can.h"

#ifndef VGUARD_FIRMWARE_VERSION
#define VGUARD_FIRMWARE_VERSION "0.0.0"
#endif

namespace {

using namespace vguard;

constexpr const char* kDefaultPath = "/tmp/vguard-esp32";

// Sem --can: um barramento sem ninguém, em que nada sai nem chega. O OBD descobre sem
// resposta, como com o carro desligado. Serve aos testes que rodam sem o vcan0.
class NoCan : public CanPort {
public:
    void listen() override {}
    void talk() override {}
    void transmit(const CanFrame&) override {}
    ListenSummary listen_summary() const override { return {}; }
};

class NotebookSystem : public SystemPort {
public:
    uint32_t heap_min() const override { return 0; }  // não se aplica no notebook
};

// CLOCK_MONOTONIC, contado desde a partida do programa.
class Clock {
public:
    Clock() : start_ns_(raw_ns()) {}
    uint64_t now_us() const { return (raw_ns() - start_ns_) / 1000; }

private:
    static uint64_t raw_ns() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
    }
    uint64_t start_ns_;
};

volatile sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

uint16_t draw_boot() {
    std::random_device rd;
    return static_cast<uint16_t>(std::uniform_int_distribution<unsigned>(1, 65535)(rd));
}

void log_line(const Clock& clock, const char* text) {
    std::fprintf(stderr, "[%10.3f] %s\n", static_cast<double>(clock.now_us()) / 1e6, text);
}

int usage(const char* program) {
    std::fprintf(stderr, "usage: %s [--can IFACE] [--path PATH]  (default path %s)\n", program,
                 kDefaultPath);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = kDefaultPath;
    std::string can_iface;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--path" && i + 1 < argc) {
            path = argv[++i];
        } else if (arg == "--can" && i + 1 < argc) {
            can_iface = argv[++i];
        } else {
            return usage(argv[0]);
        }
    }

    const Clock clock;

    // SIGINT e SIGTERM só chegam dentro do ppoll(), para o laço nunca perder um pedido de saída.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigset_t during_wait;
    sigprocmask(SIG_BLOCK, &blocked, &during_wait);
    sigdelset(&during_wait, SIGINT);
    sigdelset(&during_wait, SIGTERM);
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    native::PtyLink link;
    std::string error;
    if (!link.open(path, error)) {
        log_line(clock, error.c_str());
        return 1;
    }

    NoCan no_can;
    native::SocketCan socket_can;
    const bool has_can = !can_iface.empty();
    if (has_can && !socket_can.open(can_iface, error)) {
        log_line(clock, error.c_str());
        return 1;
    }
    CanPort& can = has_can ? static_cast<CanPort&>(socket_can) : no_can;
    ImuSim imu;  // o notebook só tem a IMU simulada (PLATAFORMA.md §5)
    NotebookSystem system;
    const BootInfo boot{draw_boot(), ResetReason::kPowerOn, Platform::kNotebook, VGUARD_FIRMWARE_VERSION};
    Core core(can, link, imu, system, boot);
    core.register_dev_handler(ImuSim::kMessageType, imu);

    char line[256];
    std::snprintf(line, sizeof(line), "native firmware %s on %s -> %s, CAN %s, boot 0x%04X",
                  VGUARD_FIRMWARE_VERSION, path.c_str(), link.pty_name().c_str(),
                  has_can ? can_iface.c_str() : "none", boot.boot);
    log_line(clock, line);

    core.start(clock.now_us());
    int status = 0;
    while (!g_stop) {
        const uint64_t deadline = core.next_deadline_us();
        const uint64_t now = clock.now_us();
        // O resultado de uma transmissão espera só até a volta seguinte.
        const uint64_t wait_us = socket_can.has_tx_result() || deadline <= now ? 0 : deadline - now;
        const timespec timeout{static_cast<time_t>(wait_us / 1'000'000),
                               static_cast<long>(wait_us % 1'000'000) * 1000};
        pollfd fds[2] = {
            {link.fd(), static_cast<short>(POLLIN | (link.has_pending() ? POLLOUT : 0)), 0},
            {has_can ? socket_can.fd() : -1, POLLIN, 0},  // um fd negativo o ppoll() ignora
        };
        pollfd& fd = fds[0];

        if (ppoll(fds, 2, &timeout, &during_wait) < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::snprintf(line, sizeof(line), "ppoll: %s", std::strerror(errno));
            log_line(clock, line);
            status = 1;
            break;
        }
        const uint64_t t_us = clock.now_us();
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            log_line(clock, "the pty closed");
            status = 1;
            break;
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            log_line(clock, "the CAN socket failed");
            status = 1;
            break;
        }
        TxResult result;
        int tx_error = 0;
        if (socket_can.take_tx_result(result, tx_error)) {
            if (tx_error != 0) {
                std::snprintf(line, sizeof(line), "can write: %s", std::strerror(tx_error));
                log_line(clock, line);
            }
            core.on_can_tx_result(result, t_us);
        }
        if (fds[1].revents & POLLIN) {
            // Um punhado por volta, para o pty e os prazos não esperarem uma rajada inteira.
            CanFrame frame;
            bool for_core = false;
            for (int i = 0; i < 64 && socket_can.read(frame, for_core); ++i) {
                if (for_core) {
                    core.on_can_frame(frame, t_us);
                }
            }
        }
        if (fd.revents & POLLIN) {
            uint8_t data[512];
            const ssize_t n = link.read(data, sizeof(data));
            if (n > 0) {
                core.on_link_bytes(data, static_cast<size_t>(n), t_us);
            }
        }
        if (fd.revents & POLLOUT) {
            link.flush();
        }
        if (t_us >= core.next_deadline_us()) {
            core.on_deadline(t_us);
        }
    }
    log_line(clock, "native firmware stopped");
    return status;
}
