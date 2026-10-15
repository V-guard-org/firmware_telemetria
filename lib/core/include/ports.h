// As quatro portas do núcleo (docs/PLATAFORMA.md §3). Cada uma tem um adaptador por
// plataforma e um falso para os testes. Nenhum adaptador chama o núcleo de dentro de uma
// chamada do núcleo.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vguard {

struct CanFrame {
    uint32_t id;
    bool extended;  // ID de 29 bits
    bool remote;
    uint8_t dlc;
    uint8_t data[8];
};

// O resultado de um transmit(), juntado pelo adaptador num só (§3.1).
enum class TxResult : uint8_t { kOk, kArbLost, kNoAck, kBusError };

enum class BusState : uint8_t { kErrorActive, kWarning, kErrorPassive, kBusOff };

// O que se viu no barramento desde o último listen() (§3.1).
struct ListenSummary {
    uint32_t frames;                // quadros válidos
    uint32_t edges;                 // transições no pino RX
    uint32_t longest_dominant_us;   // maior trecho dominante
};

class CanPort {
public:
    // Transceptor silencioso e controlador só escutando; começa um resumo da escuta novo.
    virtual void listen() = 0;
    // Transceptor liberado e controlador em modo normal, com tentativa única.
    virtual void talk() = 0;
    // Um quadro por vez; o resultado chega depois, como evento.
    virtual void transmit(const CanFrame& frame) = 0;
    virtual ListenSummary listen_summary() const = 0;

protected:
    ~CanPort() = default;
};

// A UART até a Pi (§3.3). Nunca espera.
class LinkPort {
public:
    virtual size_t tx_free() const = 0;                       // quantos bytes cabem agora
    virtual void write(const uint8_t* data, size_t len) = 0;  // só o que cabe

protected:
    ~LinkPort() = default;
};

// Uma leitura da IMU no referencial do sensor (§3.4).
struct ImuReading {
    float accel[3];  // m/s²
    float gyro[3];   // °/s
};

// O núcleo passa o instante de cada chamada. A IMU real o ignora; a IMU simulada o usa
// para a regra de 1 s sem IMU_SIM (§5).
class ImuPort {
public:
    virtual bool read(ImuReading& out, uint64_t t_us) = 0;  // false se a leitura falhou
    virtual bool restart(uint64_t t_us) = 0;                // reinicia o sensor; diz se deu certo
    virtual bool simulated() const = 0;                     // bit 0 de fontes no STATUS

protected:
    ~ImuPort() = default;
};

class SystemPort {
public:
    virtual uint32_t heap_min() const = 0;  // menor memória livre já registrada, em bytes

protected:
    ~SystemPort() = default;
};

}  // namespace vguard
