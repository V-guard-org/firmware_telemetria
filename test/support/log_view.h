// Lê o corpo de um LOG pelos offsets do docs/PROTOCOLO.md §3.5, à parte do código do
// núcleo, para que os testes confiram o documento e não a própria implementação.
#pragma once

#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vguard::testing {

struct LogView {
    uint64_t t_us;
    uint8_t level;  // 1 erro, 2 aviso
    std::string text;

    static LogView parse(const std::vector<uint8_t>& body) {
        TEST_ASSERT_TRUE_MESSAGE(body.size() >= 9 && body.size() <= 209,
                                 "o corpo do LOG tem de 9 a 209 bytes");
        LogView s{};
        for (size_t i = 0; i < 8; ++i) {
            s.t_us |= static_cast<uint64_t>(body[i]) << (8 * i);
        }
        s.level = body[8];
        s.text.assign(body.begin() + 9, body.end());
        return s;
    }
};

}  // namespace vguard::testing
