#include "socket_can.h"

#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace vguard::native {
namespace {

constexpr canid_t kFirstAnswerId = 0x7E8;
// 0x7E8 a 0x7EF, só quadros de dados de 11 bits.
constexpr canid_t kAnswersMask = (CAN_SFF_MASK & ~0x7u) | CAN_EFF_FLAG | CAN_RTR_FLAG;

}  // namespace

SocketCan::~SocketCan() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool SocketCan::open(const std::string& iface, std::string& error) {
    fd_ = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
    if (fd_ < 0) {
        error = std::string("can socket: ") + std::strerror(errno);
        return false;
    }
    const unsigned index = if_nametoindex(iface.c_str());
    if (index == 0) {
        error = "can " + iface + ": " + std::strerror(errno) + " (see the README to create vcan0)";
        return false;
    }
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(index);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "can bind " + iface + ": " + std::strerror(errno);
        return false;
    }
    listen();
    return true;
}

void SocketCan::listen() {
    listening_ = true;
    frames_ = 0;
    set_filter(false);
}

void SocketCan::talk() {
    set_filter(true);
    // O filtro só vale para os quadros que chegam depois dele. Os da escuta que ainda estão no
    // socket são descartados, para que só 0x7E8–0x7EF chegue ao núcleo.
    can_frame queued{};
    while (::read(fd_, &queued, sizeof(queued)) > 0) {
    }
    listening_ = false;
}

void SocketCan::set_filter(bool answers_only) {
    // Sem filtro, o socket recebe tudo, como o nó em listen-only aceitando todos os quadros.
    const can_filter filter = answers_only ? can_filter{kFirstAnswerId, kAnswersMask} : can_filter{0, 0};
    if (setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) != 0) {
        std::perror("can filter");  // o núcleo ainda confere o ID de cada resposta
    }
}

void SocketCan::transmit(const CanFrame& frame) {
    can_frame out{};
    out.can_id = frame.id | (frame.extended ? CAN_EFF_FLAG : 0) | (frame.remote ? CAN_RTR_FLAG : 0);
    out.len = frame.dlc;
    std::memcpy(out.data, frame.data, sizeof(out.data));
    const ssize_t n = ::write(fd_, &out, sizeof(out));
    // O vcan não tem ACK nem erros: uma escrita que falha (o canal caiu) é o mais perto de um
    // erro de barramento.
    tx_result_ = n == static_cast<ssize_t>(sizeof(out)) ? TxResult::kOk : TxResult::kBusError;
    tx_error_ = n < 0 ? errno : 0;
    tx_pending_ = true;
}

bool SocketCan::read(CanFrame& frame, bool& for_core) {
    can_frame in{};
    const ssize_t n = ::read(fd_, &in, sizeof(in));
    if (n != static_cast<ssize_t>(sizeof(in))) {
        return false;  // EAGAIN: nada agora
    }
    if (listening_) {
        ++frames_;
        for_core = false;
        return true;
    }
    frame.extended = (in.can_id & CAN_EFF_FLAG) != 0;
    frame.remote = (in.can_id & CAN_RTR_FLAG) != 0;
    frame.id = in.can_id & (frame.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    frame.dlc = in.len;
    std::memcpy(frame.data, in.data, sizeof(frame.data));
    for_core = true;
    return true;
}

bool SocketCan::take_tx_result(TxResult& result, int& error) {
    if (!tx_pending_) {
        return false;
    }
    tx_pending_ = false;
    result = tx_result_;
    error = tx_error_;
    return true;
}

}  // namespace vguard::native
