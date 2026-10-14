// Teste Unity da biblioteca portátil. Roda no notebook com: pio test -e native
#include <unity.h>

#include <optional>
#include <string_view>

#include "crc16.h"

void setUp() {}
void tearDown() {}

static void test_valor_de_verificacao() {
    TEST_ASSERT_EQUAL_HEX16(0x29B1, probe::crc16(std::string_view{"123456789"}));
}

static void test_entrada_vazia_devolve_valor_inicial() {
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, probe::crc16(nullptr, 0));
}

static void test_crc_incremental_igual_ao_de_uma_vez() {
    const std::uint8_t dados[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    const auto parte1 = probe::crc16(dados, 4);
    TEST_ASSERT_EQUAL_HEX16(0x29B1, probe::crc16(dados + 4, 5, parte1));
}

static void test_recursos_cpp17() {
    // Só compila em C++17: optional, if com inicializador, structured bindings.
    std::optional<int> v = 7;
    if (auto x = v; x.has_value()) {
        auto [a, b] = std::pair{*x, 1};
        TEST_ASSERT_EQUAL_INT(8, a + b);
    }
    TEST_ASSERT_TRUE(__cplusplus >= 201703L);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_valor_de_verificacao);
    RUN_TEST(test_entrada_vazia_devolve_valor_inicial);
    RUN_TEST(test_crc_incremental_igual_ao_de_uma_vez);
    RUN_TEST(test_recursos_cpp17);
    return UNITY_END();
}
