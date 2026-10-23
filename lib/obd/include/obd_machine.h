// A máquina do OBD (docs/MAQUINAS-DE-ESTADO.md §2): escuta, descoberta, leitura pela agenda e
// a amostra OBD (docs/PROTOCOLO.md §3.3). Recebe o portão da lista fechada, nunca a porta CAN
// da plataforma. O núcleo a chama nos eventos e nos prazos, com o instante; ela nunca
// pergunta a hora.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ports.h"
#include "protocol.h"

namespace vguard {

// O estado_obd do STATUS (PROTOCOLO.md §3.1).
enum class ObdState : uint8_t {
    kWaitingPi = 0,
    kListening = 1,
    kDiscovering = 2,
    kReading = 3,
    kNoResponse = 4,
    kWrongBitrate = 5,
    kLineStuck = 6,
    kCanFailure = 7,
    kLocked = 8,
};

// Uma amostra OBD pronta para o corpo do PROTOCOLO.md §3.3, em ordem crescente de PID.
struct ObdSample {
    static constexpr size_t kMaxValues = 8;  // os PIDs da D6
    uint64_t t_us;
    size_t n;
    ObdValue values[kMaxValues];
};

class ObdMachine {
public:
    // gate: o portão da lista fechada. rate: amostras por segundo, uma das do PROTOCOLO.md §4.2.
    ObdMachine(CanPort& gate, uint8_t rate);

    // O 1º PING tira o OBD de ESPERANDO_PI: começa a escuta de 1 s.
    void on_ping(uint64_t now_us);
    void on_frame(const CanFrame& frame, uint64_t t_us);
    void on_tx_result(TxResult result, uint64_t t_us);
    // O que venceu. Devolve true quando sai uma amostra OBD.
    bool on_deadline(uint64_t now_us, ObdSample& sample);
    uint64_t next_deadline_us() const;

    // O portão barrou um quadro, o que só acontece por defeito no código: BLOQUEADO até
    // reiniciar. Chega de dentro do transmit() do próprio OBD, pelo núcleo.
    void on_frame_blocked();

    // A taxa nova vale a partir da próxima amostra (PROTOCOLO.md §4.2): ela sai no próximo
    // múltiplo do período novo.
    void set_rate(uint8_t rate, uint64_t now_us);
    uint8_t rate() const { return rate_; }

    ObdState state() const { return state_; }
    const uint8_t* pids() const { return pids_; }  // os 12 bytes do STATUS
    uint32_t sent() const { return sent_; }        // obd_enviadas
    uint32_t answered() const { return answered_; }
    uint32_t timeouts() const { return timeouts_; }

private:
    static constexpr size_t kCentrals = 8;  // as respostas de 0x7E8 a 0x7EF
    static constexpr size_t kMaps = 3;      // 01 00, 01 20 e 01 40

    // Uma resposta do modo 01 num quadro único: a central (0 a 7), o PID e os bytes de dados,
    // sem o modo e sem o PID.
    struct Answer {
        uint8_t central;
        uint8_t pid;
        uint8_t len;
        const uint8_t* data;
    };

    // Um PID da agenda: a quem perguntar, quando, e o último valor.
    struct Entry {
        uint8_t pid;
        uint8_t central;      // 0 a 7: responde em 0x7E8 + central, pergunta em 0x7E0 + central
        uint64_t period_us;
        uint64_t due_us;
        bool awaiting;        // perguntado e ainda sem resposta, mesmo depois dos 50 ms
        bool has_value;
        uint8_t len;
        uint8_t raw[4];
        uint64_t answered_us;
    };

    void end_listening(uint64_t now_us);
    void ask_map(size_t map, uint64_t now_us);
    void end_map_window(uint64_t now_us);
    void start_reading(uint64_t now_us);
    static bool parse_answer(const CanFrame& frame, Answer& out);
    void on_reading_answer(const Answer& answer, uint64_t t_us);
    void ask_next_pid(uint64_t now_us);
    bool send_request(uint32_t id, uint8_t pid, uint64_t now_us);
    bool build_sample(uint64_t now_us, ObdSample& sample) const;
    uint64_t next_sample_after(uint64_t now_us) const;
    void enter_silent(ObdState state);

    CanPort& gate_;
    uint8_t rate_;
    ObdState state_ = ObdState::kWaitingPi;
    uint64_t listen_end_us_ = 0;

    // A descoberta
    size_t asked_map_ = 0;                         // 0, 1 ou 2: 01 00, 01 20 ou 01 40
    bool map_answered_ = false;
    uint8_t maps_[kCentrals][kMaps * 4] = {};      // o que cada central respondeu
    uint8_t pids_[kMaps * 4] = {};                 // a união, para o STATUS

    // A requisição em curso
    uint64_t last_request_us_ = 0;
    bool pending_ = false;
    uint8_t pending_pid_ = 0;

    Entry agenda_[ObdSample::kMaxValues] = {};
    size_t agenda_len_ = 0;
    uint64_t next_sample_us_ = 0;

    uint32_t sent_ = 0;
    uint32_t answered_ = 0;
    uint32_t timeouts_ = 0;
};

}  // namespace vguard
