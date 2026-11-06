#pragma once
#include "obd_core.h"

// Verificacao executada pelo compilador sobre as funcoes usadas no firmware.
namespace obd_checks {
static_assert(obdStandardFilter(0x7FF) == 0x07FF0000UL, "Mascara SID sem filtrar dados");
static_assert(obdStandardFilter(0x7E8) == 0x07E80000UL, "Filtro SID da ECU");
static_assert(obdStandardFilter(0x7E8) != 0x000007E8UL, "Rejeitar formato incorreto");
static_assert(!obdTimedOut(100, 150, 1200), "Nao expirar com horario anterior ao envio");
static_assert(!obdTimedOut(1349, 150, 1200), "Nao expirar antes do limite");
static_assert(obdTimedOut(1350, 150, 1200), "Expirar no limite");
static_assert(obdTimedOut(20, 0xFFFFFFF0UL, 30), "Timeout atravessa rollover millis");
constexpr uint8_t rpm[] = {4, 0x41, 0x0C, 0x0D, 0xB0, 0, 0, 0};
constexpr uint8_t negativa[] = {3, 0x7F, 1, 0x11};
constexpr uint8_t multiframe[] = {0x10, 0x0A, 0x41, 0x0C, 0, 0, 0, 0};
constexpr bool quadro(unsigned long id, uint8_t dlc, const uint8_t *d, uint8_t pid) {
  uint8_t bytes = 0;
  return obdPositive(id, dlc, d, 0x7E8, pid, bytes);
}
constexpr float valor(uint8_t pid, uint8_t a, uint8_t b, uint8_t n) {
  uint8_t dados[] = {a, b};
  float v = 0;
  return obdDecode(pid, dados, n, v) ? v : -99999;
}
constexpr bool perto(float a, float b) { return a > b - 0.001f && a < b + 0.001f; }
static_assert(quadro(0x7E8, 8, rpm, 0x0C), "Resposta RPM valida");
static_assert(!quadro(0x7E9, 8, rpm, 0x0C), "Rejeitar outra ECU");
static_assert(!quadro(0x800007E8UL, 8, rpm, 0x0C), "Rejeitar frame estendido");
static_assert(!quadro(0x400007E8UL, 8, rpm, 0x0C), "Rejeitar RTR");
static_assert(!quadro(0x7E8, 4, rpm, 0x0C), "Rejeitar frame truncado");
static_assert(!quadro(0x7E8, 9, rpm, 0x0C), "Rejeitar DLC invalido");
static_assert(!quadro(0x7E8, 8, rpm, 0x0D), "Rejeitar PID diferente");
static_assert(!quadro(0x7E8, 4, negativa, 1), "Rejeitar resposta negativa");
static_assert(!quadro(0x7E8, 8, multiframe, 0x0C), "Rejeitar resposta segmentada");
static_assert(valor(0x0C, 0x0D, 0xB0, 2) == 876, "RPM lido no Voyage");
static_assert(valor(0x0C, 0x1A, 0xF8, 2) == 1726, "Exemplo RPM");
static_assert(valor(0x0D, 0, 0, 1) == 0, "Zero e leitura valida");
static_assert(valor(0x0D, 60, 0, 1) == 60, "Velocidade direta");
static_assert(valor(0x05, 87, 0, 1) == 47, "Temperatura lida no Voyage");
static_assert(valor(0x05, 0, 0, 1) == -40, "Temperatura negativa");
static_assert(perto(valor(0x42, 0x35, 0xE8, 2), 13.8f), "Tensao ECU");
static_assert(valor(0x04, 255, 0, 1) == 100, "Percentual");
static_assert(valor(0x06, 128, 0, 1) == 0, "Fuel trim neutro");
static_assert(valor(0x0C, 1, 0, 1) == -99999, "Rejeitar RPM incompleto");
static_assert(valor(0x01, 1, 0, 1) == -99999, "PID codificado fica bruto");
static_assert(obdSupported(0x80000001UL, 1), "Primeiro bit do mapa");
static_assert(obdSupported(0x80000001UL, 32), "Bit de proximo bloco");
static_assert(!obdSupported(0x80000001UL, 2), "Bit ausente");
static_assert(!obdSupported(0xFFFFFFFFUL, 0), "Offset zero invalido");
static_assert(!obdSupported(0xFFFFFFFFUL, 33), "Offset acima do mapa");
constexpr uint8_t mapa[] = {0x80, 0, 0, 1};
static_assert(obdMap(mapa) == 0x80000001UL, "Mapa em big-endian");
}
