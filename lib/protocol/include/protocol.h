// Moldura do protocolo ESP32 ↔ Pi (docs/PROTOCOLO.md §2): CRC, COBS e mensagens.
// Código portátil (docs/PLATAFORMA.md §2.2): sem alocação, sem exceções, sem plataforma.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vguard {

// Códigos das mensagens (PROTOCOLO.md, resumo). De 0x00 a 0x7F, do ESP32 para a Pi;
// de 0x80 a 0xFF, da Pi para o ESP32.
enum class MessageType : uint8_t {
    kStatus = 0x01,
    kImu = 0x02,
    kObd = 0x03,
    kPong = 0x04,
    kLog = 0x05,
    kPing = 0x81,
    kTaxas = 0x82,
    kImuSim = 0x83,
};

constexpr size_t kHeaderSize = 5;                            // tipo, boot e seq (§2.1)
constexpr size_t kCrcSize = 2;
constexpr size_t kMaxMessage = 255;                          // cabeçalho, corpo e CRC
constexpr size_t kMaxBody = kMaxMessage - kHeaderSize - kCrcSize;
constexpr size_t kMaxEncoded = kMaxMessage + 2;              // depois do COBS, sem o 0x00
constexpr size_t kMaxWire = kMaxEncoded + 1;                 // com o 0x00 final

// CRC-16/CCITT-FALSE: polinômio 0x1021, valor inicial 0xFFFF, sem reflexão, sem XOR final.
uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF);


// Corpos das mensagens do ESP32 (§3). Cada build_*_body escreve o corpo em out, que precisa
// de kMaxBody bytes, e devolve o tamanho. Os números vão em little-endian.

// IMU (§3.2): eixos na ordem ax, ay, az, gx, gy, gz, em 0,01 m/s² e 0,1 °/s.
struct ImuSampleBody {
    uint64_t t_us;
    int16_t mean[6];
    int16_t peak[6];
};

size_t build_imu_body(const ImuSampleBody& sample, uint8_t* out);

// PONG (§3.4): o seq do PING e o instante em que ele chegou inteiro.
size_t build_pong_body(uint16_t seq_ping, uint64_t t_us, uint8_t* out);

// STATUS (§3.1).
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kMaxFirmwareText = 32;

// Os contadores do STATUS, na ordem do protocolo. Somam desde o boot e nunca zeram.
enum Counter : size_t {
    kDropped,       // descartadas: fila de saída cheia
    kRejected,      // rejeitadas: mensagens da Pi com CRC errado, tipo desconhecido…
    kImuFailures,   // falhas_imu
    kObdSent,       // obd_enviadas
    kObdAnswered,   // obd_respondidas
    kObdTimeouts,   // obd_sem_resposta
    kBlocked,       // bloqueadas pela lista fechada
    kCanErrors,     // erros_can
    kBusOff,        // bus_off
    kCounterCount,
};

struct StatusBody {
    uint64_t t_us;
    uint8_t platform;      // 0 notebook, 1 ESP32
    uint8_t sources;       // bit 0: IMU simulada
    uint8_t reset_reason;
    uint8_t imu_rate;
    uint8_t obd_rate;
    uint8_t imu_state;     // 0 ok, 1 falha
    uint8_t obd_state;     // 0 ESPERANDO_PI … 8 BLOQUEADO
    uint8_t pids[12];
    uint32_t counters[kCounterCount];
    uint32_t heap_min;
    const char* firmware;  // cortado em kMaxFirmwareText bytes
};

size_t build_status_body(const StatusBody& status, uint8_t* out);

// Um valor da amostra OBD (§3.3): os bytes crus da resposta, sem o modo e sem o PID.
struct ObdValue {
    uint8_t pid;
    uint16_t age_ms;
    uint8_t len;  // de 1 a 4
    uint8_t data[4];
};

// OBD (§3.3): values já em ordem crescente de PID; n vai até 8 (D6).
size_t build_obd_body(uint64_t t_us, const ObdValue* values, size_t n, uint8_t* out);

// Escreve em out os bytes do cabo, COBS(cabeçalho | corpo | CRC16) seguido de 0x00, e
// devolve quantos são. out precisa de kMaxWire bytes; body_len vai até kMaxBody.
size_t encode_message(MessageType type, uint16_t boot, uint16_t seq, const uint8_t* body,
                      size_t body_len, uint8_t* out);


// A fila de saída do ESP32 (§6.5). Guarda o tipo e o corpo: o seq, o CRC e o COBS só são
// aplicados na hora de escrever, então uma mensagem descartada na fila não gasta seq.
// A ordem de saída é PONG, STATUS, IMU e OBD na ordem em que foram geradas, e LOG.
class OutputQueue {
public:
    static constexpr size_t kCapacity = 16;

    struct Entry {
        MessageType type;
        size_t body_len;
        uint8_t body[kMaxBody];
    };

    // Põe a mensagem na fila. Com a fila cheia, descarta a mais antiga da classe que sai
    // por último, contando a nova, e devolve true: primeiro o LOG mais antigo, depois a
    // amostra mais antiga.
    bool push(MessageType type, const uint8_t* body, size_t body_len);

    // A próxima mensagem a sair, ou nullptr com a fila vazia. Vale até o próximo push().
    const Entry* front() const;
    void pop();

private:
    struct Slot {
        bool used;
        uint32_t order;  // ordem de chegada
        Entry entry;
    };

    static constexpr size_t kNone = kCapacity;
    size_t front_index() const;
    size_t victim_index() const;

    Slot slots_[kCapacity] = {};
    uint32_t next_order_ = 0;
};

// Uma mensagem recebida e conferida. O corpo aponta para dentro do FrameReader.
struct Message {
    MessageType type;  // pode ser um código que ninguém conhece
    uint16_t boot;
    uint16_t seq;
    const uint8_t* body;
    size_t body_len;
};

// Remonta as mensagens que chegam, byte a byte, e confere o COBS, o tamanho e o CRC (§2).
// Depois de lixo na linha, ou de começar no meio de uma mensagem, ele se realinha no
// próximo 0x00.
class FrameReader {
public:
    enum class Result : uint8_t {
        kPending,   // no meio de uma mensagem, ou um 0x00 sem nada antes, que é ignorado
        kMessage,   // chegou uma mensagem inteira: message() vale até o próximo kMessage
        kRejected,  // COBS inválido, menos de 7 bytes, CRC errado ou mais de 257 bytes sem 0x00
    };

    Result push(uint8_t byte);
    const Message& message() const { return message_; }

private:
    Result finish();

    uint8_t encoded_[kMaxEncoded];
    size_t encoded_len_ = 0;
    bool overflowed_ = false;
    uint8_t decoded_[kMaxEncoded];
    Message message_{};
};

}  // namespace vguard
