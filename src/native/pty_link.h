// A UART do notebook: um par de pseudo-terminais, com um atalho de caminho fixo que o
// decodificador e o simulador abrem (docs/PLATAFORMA.md §4.2).
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "ports.h"

namespace vguard::native {

class PtyLink : public LinkPort {
public:
    PtyLink() = default;
    PtyLink(const PtyLink&) = delete;
    PtyLink& operator=(const PtyLink&) = delete;
    ~PtyLink();

    // Cria o pty e o atalho. Em caso de erro, devolve false e escreve o motivo em error.
    bool open(const std::string& link_path, std::string& error);

    int fd() const { return master_; }
    const std::string& pty_name() const { return pty_name_; }

    // Os bytes que a Pi escreveu: >0 bytes lidos, 0 nada agora, <0 erro (errno).
    ssize_t read(uint8_t* data, size_t len);

    // Escreve no pty o que ficou guardado. A plataforma chama quando o pty aceita mais.
    void flush();
    bool has_pending() const { return pending_len_ > 0; }

    // O que o pty recusou fica guardado aqui, e o espaço livre diminui (§3.3).
    size_t tx_free() const override { return sizeof(pending_) - pending_len_; }
    void write(const uint8_t* data, size_t len) override;

private:
    int master_ = -1;
    int slave_ = -1;
    std::string pty_name_;
    std::string link_path_;
    uint8_t pending_[4096];
    size_t pending_len_ = 0;
};

}  // namespace vguard::native
