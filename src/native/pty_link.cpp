#include "pty_link.h"

#include <fcntl.h>
#include <pty.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace vguard::native {

PtyLink::~PtyLink() {
    // Só apaga o atalho se ele ainda aponta para este pty: outro programa pode ter
    // criado o seu por cima.
    if (!symlink_path_.empty()) {
        char target[256] = {};
        const ssize_t n = readlink(symlink_path_.c_str(), target, sizeof(target) - 1);
        if (n > 0 && pty_name_ == target) {
            unlink(symlink_path_.c_str());
        }
    }
    if (slave_ >= 0) {
        close(slave_);
    }
    if (master_ >= 0) {
        close(master_);
    }
}

bool PtyLink::open(const std::string& symlink_path, std::string& error) {
    struct stat st {};
    if (lstat(symlink_path.c_str(), &st) == 0 && !S_ISLNK(st.st_mode)) {
        error = symlink_path + " exists and is not a symlink; choose another --path";
        return false;
    }

    char name[64] = {};
    if (openpty(&master_, &slave_, name, nullptr, nullptr) != 0) {
        error = std::string("openpty: ") + std::strerror(errno);
        return false;
    }
    pty_name_ = name;

    // Sem eco e sem tradução de bytes, antes de o leitor abrir. Sem isso, o pty devolveria
    // ao firmware o que ele mesmo escreveu.
    termios attrs{};
    tcgetattr(slave_, &attrs);
    cfmakeraw(&attrs);
    tcsetattr(slave_, TCSANOW, &attrs);
    // O firmware fica com o seu lado do escravo aberto: sem leitor, o pty não "desliga",
    // e o poll() não acorda sem parar.
    fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);

    // Cria ao lado e troca de uma vez, por cima de um atalho deixado por outra execução.
    const std::string temporary = symlink_path + "." + std::to_string(getpid());
    if (symlink(name, temporary.c_str()) != 0 || rename(temporary.c_str(), symlink_path.c_str()) != 0) {
        error = "symlink " + symlink_path + ": " + std::strerror(errno);
        unlink(temporary.c_str());
        return false;
    }
    symlink_path_ = symlink_path;
    return true;
}

ssize_t PtyLink::read(uint8_t* data, size_t len) {
    const ssize_t n = ::read(master_, data, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return n;
}

void PtyLink::write(const uint8_t* data, size_t len) {
    if (len > tx_free()) {
        len = tx_free();  // o núcleo confere o tx_free() antes; isto nunca deveria cortar
    }
    std::memcpy(pending_ + pending_len_, data, len);
    pending_len_ += len;
    flush();
}

void PtyLink::flush() {
    while (pending_len_ > 0) {
        const ssize_t n = ::write(master_, pending_, pending_len_);
        if (n <= 0) {
            return;  // o pty está cheio (EAGAIN): o resto sai quando ele aceitar mais
        }
        pending_len_ -= static_cast<size_t>(n);
        std::memmove(pending_, pending_ + n, pending_len_);
    }
}

}  // namespace vguard::native
