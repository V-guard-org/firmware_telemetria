// Lê o corpo de uma amostra OBD pelos offsets do docs/PROTOCOLO.md §3.3, à parte do código do
// núcleo, para que os testes confiram o documento e não a própria implementação.
#pragma once

#include <unity.h>

#include <cstdint>
#include <vector>

namespace vguard::testing {

struct ObdView {
    struct Value {
        uint8_t pid;
        uint16_t age_ms;
        std::vector<uint8_t> raw;
    };

    uint64_t t_us;
    std::vector<Value> values;

    static ObdView parse(const std::vector<uint8_t>& body) {
        TEST_ASSERT_TRUE_MESSAGE(body.size() >= 9, "o corpo do OBD tem pelo menos 9 bytes");
        ObdView s{};
        for (size_t i = 0; i < 8; ++i) {
            s.t_us |= static_cast<uint64_t>(body[i]) << (8 * i);
        }
        size_t offset = 9;
        for (uint8_t n = 0; n < body[8]; ++n) {
            TEST_ASSERT_TRUE_MESSAGE(offset + 4 <= body.size(), "valor OBD cortado");
            Value v{};
            v.pid = body[offset];
            v.age_ms = static_cast<uint16_t>(body[offset + 1] | (body[offset + 2] << 8));
            const uint8_t len = body[offset + 3];
            TEST_ASSERT_TRUE_MESSAGE(len >= 1 && len <= 4, "tam vai de 1 a 4");
            offset += 4;
            TEST_ASSERT_TRUE_MESSAGE(offset + len <= body.size(), "bytes crus cortados");
            v.raw.assign(body.begin() + offset, body.begin() + offset + len);
            offset += len;
            s.values.push_back(v);
        }
        TEST_ASSERT_EQUAL_MESSAGE(body.size(), offset, "bytes a mais no corpo do OBD");
        return s;
    }

    const Value* find(uint8_t pid) const {
        for (const Value& v : values) {
            if (v.pid == pid) {
                return &v;
            }
        }
        return nullptr;
    }
};

}  // namespace vguard::testing
