#include "obd_machine.h"

#include <cstring>

namespace vguard {
namespace {

constexpr uint64_t kListenUs = 1'000'000;          // MAQUINAS-DE-ESTADO.md §2.7
constexpr uint32_t kLineStuckUs = 10'000;          // dominante contínuo por mais de 10 ms
constexpr uint64_t kAnswerTimeoutUs = 50'000;      // ISO 15765-4; também o intervalo mínimo (D2)
constexpr uint64_t kFastPeriodUs = 200'000;        // RPM e velocidade
constexpr uint64_t kSlowPeriodUs = 1'000'000;      // os outros
constexpr uint64_t kNever = UINT64_MAX;
constexpr uint16_t kMaxAgeMs = 65535;

constexpr uint32_t kFunctionalId = 0x7DF;          // a todas as centrais
constexpr uint32_t kFirstPhysicalId = 0x7E0;       // a uma central: o ID da resposta menos 8
constexpr uint32_t kFirstAnswerId = 0x7E8;
constexpr uint8_t kMode01 = 0x01;
constexpr uint8_t kMode01Answer = 0x41;
constexpr uint8_t kPadding = 0x00;
constexpr uint8_t kMapStep = 0x20;                 // 01 00, 01 20, 01 40

// A lista da D6, em ordem crescente: a ordem da amostra e o desempate da agenda.
constexpr uint8_t kListPids[] = {0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x11, 0x42};

// Os bytes que a fórmula do PID usa (PROTOCOLO.md §3.3): menos que isso não vale.
uint8_t needed_bytes(uint8_t pid) {
    return (pid == 0x0C || pid == 0x42) ? 2 : 1;
}

uint64_t period_of(uint8_t pid) {
    return (pid == 0x0C || pid == 0x0D) ? kFastPeriodUs : kSlowPeriodUs;
}

// O bit do PID nos 12 bytes dos mapas: no 1º byte de cada mapa, o bit 7 é o PID base + 1.
bool supports(const uint8_t* maps, uint8_t pid) {
    const unsigned index = pid - 1u;
    return (maps[index / 8] >> (7 - index % 8)) & 1;
}

// Uma resposta do modo 01 num quadro único, de 0x7E8 a 0x7EF: a central, o PID e os bytes de
// dados, sem o modo e sem o PID. Uma resposta em vários quadros não serve (§2.4).
struct Answer {
    uint8_t central;
    uint8_t pid;
    uint8_t len;
    const uint8_t* data;
};

bool parse_answer(const CanFrame& frame, Answer& out) {
    if (frame.extended || frame.remote || frame.id < kFirstAnswerId ||
        frame.id >= kFirstAnswerId + 8 || frame.dlc < 3 || frame.dlc > 8) {
        return false;
    }
    const uint8_t length = frame.data[0];  // quadro único: 0 no nibble alto e o tamanho útil
    if ((length & 0xF0) != 0 || length < 2 || length + 1u > frame.dlc ||
        frame.data[1] != kMode01Answer) {
        return false;
    }
    out = {static_cast<uint8_t>(frame.id - kFirstAnswerId), frame.data[2],
           static_cast<uint8_t>(length - 2), frame.data + 3};
    return true;
}

}  // namespace

ObdMachine::ObdMachine(CanPort& gate, uint8_t rate) : gate_(gate), rate_(rate) {}

void ObdMachine::on_ping(uint64_t now_us) {
    if (state_ != ObdState::kWaitingPi) {
        return;  // a presença da Pi, com a volta a ESPERANDO_PI, chega no #26
    }
    state_ = ObdState::kListening;
    gate_.listen();
    listen_end_us_ = now_us + kListenUs;
}

void ObdMachine::on_frame(const CanFrame& frame, uint64_t t_us) {
    Answer answer;
    if (!parse_answer(frame, answer)) {
        return;
    }
    if (state_ == ObdState::kDiscovering) {
        if (answer.pid == map_ * kMapStep && answer.len >= 4) {
            std::memcpy(&maps_[answer.central][map_ * 4], answer.data, 4);
            map_answered_ = true;
        }
    } else if (state_ == ObdState::kReading) {
        on_reading_answer(answer.central, answer.pid, answer.data, answer.len, t_us);
    }
}

void ObdMachine::on_tx_result(TxResult result, uint64_t) {
    if (result == TxResult::kOk) {
        ++sent_;  // as outras, com a tentativa única, chegam no #26
    }
}

bool ObdMachine::on_deadline(uint64_t now_us, ObdSample& sample) {
    switch (state_) {
        case ObdState::kListening:
            if (now_us >= listen_end_us_) {
                end_listening(now_us);
            }
            return false;
        case ObdState::kDiscovering:
            if (now_us >= last_request_us_ + kAnswerTimeoutUs) {
                end_map_window(now_us);
            }
            if (state_ != ObdState::kReading) {
                return false;
            }
            break;
        case ObdState::kReading:
            if (pending_ && now_us >= last_request_us_ + kAnswerTimeoutUs) {
                pending_ = false;
                ++timeouts_;
            }
            if (!pending_ && now_us >= last_request_us_ + kAnswerTimeoutUs) {
                ask_next_pid(now_us);
            }
            break;
        default:
            return false;
    }
    if (state_ != ObdState::kReading || now_us < next_sample_us_) {
        return false;
    }
    next_sample_us_ = next_sample_after(now_us);
    return build_sample(now_us, sample);
}

uint64_t ObdMachine::next_deadline_us() const {
    switch (state_) {
        case ObdState::kListening:
            return listen_end_us_;
        case ObdState::kDiscovering:
            return last_request_us_ + kAnswerTimeoutUs;
        case ObdState::kReading: {
            uint64_t request = kNever;
            if (pending_) {
                request = last_request_us_ + kAnswerTimeoutUs;
            } else {
                for (size_t i = 0; i < agenda_len_; ++i) {
                    request = agenda_[i].due_us < request ? agenda_[i].due_us : request;
                }
                const uint64_t gap = last_request_us_ + kAnswerTimeoutUs;
                request = request != kNever && request < gap ? gap : request;
            }
            return request < next_sample_us_ ? request : next_sample_us_;
        }
        default:
            return kNever;
    }
}

void ObdMachine::set_rate(uint8_t rate, uint64_t now_us) {
    rate_ = rate;
    if (state_ == ObdState::kReading) {
        next_sample_us_ = next_sample_after(now_us);
    }
}

void ObdMachine::lock() {
    enter_silent(ObdState::kLocked);
}

// A tabela do MAQUINAS-DE-ESTADO.md §2.1. Nos dois casos reprovados, o transceptor continua
// silencioso; a espera de 10 s e a volta a ESCUTANDO chegam no #26.
void ObdMachine::end_listening(uint64_t now_us) {
    const ListenSummary summary = gate_.listen_summary();
    if (summary.longest_dominant_us > kLineStuckUs) {
        state_ = ObdState::kLineStuck;
    } else if (summary.frames == 0 && summary.edges > 0) {
        state_ = ObdState::kWrongBitrate;
    } else {
        state_ = ObdState::kDiscovering;
        gate_.talk();
        std::memset(maps_, 0, sizeof(maps_));
        ask_map(0, now_us);
    }
}

void ObdMachine::ask_map(size_t map, uint64_t now_us) {
    map_ = map;
    map_answered_ = false;
    send_request(kFunctionalId, static_cast<uint8_t>(map * kMapStep), now_us);
}

// Fim dos 50 ms de um mapa: as respostas de todas as centrais já chegaram (§2.3).
void ObdMachine::end_map_window(uint64_t now_us) {
    if (map_answered_) {
        ++answered_;
    } else {
        ++timeouts_;
    }
    if (!map_answered_ && map_ == 0) {
        enter_silent(ObdState::kNoResponse);  // a nova pergunta, 2 s ou 10 s depois, chega no #26
        return;
    }
    // O último bit do mapa diz que o próximo existe; uma pergunta sem resposta encerra a
    // descoberta com o que já se tem.
    bool next = false;
    for (size_t c = 0; map_answered_ && map_ + 1 < kMaps && c < kCentrals; ++c) {
        next = next || (maps_[c][map_ * 4 + 3] & 0x01) != 0;
    }
    if (next) {
        ask_map(map_ + 1, now_us);
    } else {
        start_reading(now_us);
    }
}

// O mapa do STATUS é a união; cada PID da lista vai para a central de menor endereço que o
// tem. Sem nenhum PID da lista, a agenda fica vazia: LENDO, sem requisições nem amostras.
void ObdMachine::start_reading(uint64_t now_us) {
    std::memset(pids_, 0, sizeof(pids_));
    for (size_t c = 0; c < kCentrals; ++c) {
        for (size_t i = 0; i < sizeof(pids_); ++i) {
            pids_[i] |= maps_[c][i];
        }
    }
    agenda_len_ = 0;
    for (const uint8_t pid : kListPids) {
        for (uint8_t c = 0; c < kCentrals; ++c) {
            if (supports(maps_[c], pid)) {
                agenda_[agenda_len_++] = {pid, c, period_of(pid), now_us, false, 0, {}, 0};
                break;
            }
        }
    }
    state_ = ObdState::kReading;
    pending_ = false;
    next_sample_us_ = next_sample_after(now_us);
    ask_next_pid(now_us);
}

// Só vale a resposta da central escolhida, com o PID da agenda e bytes suficientes. Uma
// resposta atrasada ainda vale como valor, mas a requisição já contou como sem resposta.
void ObdMachine::on_reading_answer(uint8_t central, uint8_t pid, const uint8_t* data, uint8_t len,
                                   uint64_t t_us) {
    for (size_t i = 0; i < agenda_len_; ++i) {
        Entry& e = agenda_[i];
        if (e.pid != pid || e.central != central || len < needed_bytes(pid)) {
            continue;
        }
        e.len = len < sizeof(e.raw) ? len : sizeof(e.raw);
        std::memcpy(e.raw, data, e.len);
        e.has_value = true;
        e.answered_us = t_us;
        if (pending_ && pending_pid_ == pid) {
            pending_ = false;
            ++answered_;
        }
        return;
    }
}

// O PID mais atrasado em relação ao seu horário; no empate, o de menor número. Cada PID vence
// de novo um período depois da sua última pergunta.
void ObdMachine::ask_next_pid(uint64_t now_us) {
    Entry* next = nullptr;
    for (size_t i = 0; i < agenda_len_; ++i) {
        if (agenda_[i].due_us <= now_us && (next == nullptr || agenda_[i].due_us < next->due_us)) {
            next = &agenda_[i];
        }
    }
    if (next == nullptr ||
        !send_request(kFirstPhysicalId + next->central, next->pid, now_us)) {
        return;
    }
    next->due_us = now_us + next->period_us;
    pending_ = true;
    pending_pid_ = next->pid;
}

// false se o portão barrou o quadro e o OBD foi a BLOQUEADO.
bool ObdMachine::send_request(uint32_t id, uint8_t pid, uint64_t now_us) {
    const CanFrame frame{id, false, false, 8,
                         {0x02, kMode01, pid, kPadding, kPadding, kPadding, kPadding, kPadding}};
    last_request_us_ = now_us;
    gate_.transmit(frame);
    return state_ != ObdState::kLocked;
}

bool ObdMachine::build_sample(uint64_t now_us, ObdSample& sample) const {
    sample.t_us = now_us;
    sample.n = 0;
    for (size_t i = 0; i < agenda_len_; ++i) {
        const Entry& e = agenda_[i];
        if (!e.has_value) {
            continue;
        }
        const uint64_t age_ms = (now_us - e.answered_us) / 1000;
        ObdValue& v = sample.values[sample.n++];
        v.pid = e.pid;
        v.age_ms = age_ms > kMaxAgeMs ? kMaxAgeMs : static_cast<uint16_t>(age_ms);
        v.len = e.len;
        std::memcpy(v.data, e.raw, e.len);
    }
    return sample.n > 0;  // sem nenhum valor, não sai amostra
}

// As amostras saem nos múltiplos do período desde o boot, independentes da agenda (§2.4).
uint64_t ObdMachine::next_sample_after(uint64_t now_us) const {
    const uint64_t period = 1'000'000 / rate_;
    return (now_us / period + 1) * period;
}

// Transceptor silencioso; ao sair de LENDO, os valores são esquecidos.
void ObdMachine::enter_silent(ObdState state) {
    state_ = state;
    pending_ = false;
    agenda_len_ = 0;
    gate_.listen();
}

}  // namespace vguard
