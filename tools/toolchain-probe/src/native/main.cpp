// Programa do notebook (env native): prova que o mesmo projeto gera um executável Linux
// com as peças da camada de plataforma da fase 1: thread, pseudo-terminal e SocketCAN.
//   pio run -e native && .pio/build/native/program [vcan0]
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include "crc16.h"

int main(int argc, char** argv) {
    const char* ifname = argc > 1 ? argv[1] : "vcan0";

    // Tarefa -> std::thread
    std::thread t([] { std::printf("thread: crc16=0x%04X\n", probe::crc16("123456789")); });
    t.join();

    // UART -> pseudo-terminal (o outro lado abre o caminho impresso)
    int master = -1, slave = -1;
    char name[64] = {};
    if (openpty(&master, &slave, name, nullptr, nullptr) == 0) {
        std::printf("pty: %s\n", name);
        close(slave);
        close(master);
    } else {
        std::printf("pty: falhou (%s)\n", std::strerror(errno));
    }

    // CAN -> SocketCAN
    const int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0) {
        std::printf("socket(PF_CAN): falhou (%s)\n", std::strerror(errno));
        return 1;
    }
    std::printf("socket(PF_CAN, SOCK_RAW, CAN_RAW): ok, sem root\n");

    ifreq ifr = {};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        std::printf("%s: não existe (%s); crie com ip link add, como root\n", ifname,
                    std::strerror(errno));
        close(sock);
        return 2;
    }

    sockaddr_can addr = {};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::printf("bind(%s): falhou (%s)\n", ifname, std::strerror(errno));
        close(sock);
        return 3;
    }

    // Requisição OBD modo 01, PID 0x0C (RPM), no endereço funcional 0x7DF.
    can_frame f = {};
    f.can_id = 0x7DF;
    f.len = 8;
    const unsigned char req[8] = {0x02, 0x01, 0x0C, 0x55, 0x55, 0x55, 0x55, 0x55};
    std::memcpy(f.data, req, sizeof(req));
    const auto n = write(sock, &f, sizeof(f));
    std::printf("write(%s, 7DF#02010C5555555555): %s\n", ifname,
                n == static_cast<ssize_t>(sizeof(f)) ? "ok" : std::strerror(errno));
    close(sock);
    return 0;
}
