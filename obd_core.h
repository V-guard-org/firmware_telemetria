#pragma once
#include <stdint.h>
#include <stddef.h>

// MCP_CAN 1.5.1: mascaras/filtros padrao usam SID nos bits 26..16.
// Os 16 bits inferiores representam filtros dos primeiros bytes de dados.
constexpr uint32_t obdStandardFilter(uint16_t id) {
  return uint32_t(id & 0x7FF) << 16;
}

// Intervalos menores que 2^31 ms; um instante anterior ao envio nao expira.
constexpr bool obdTimedOut(uint32_t now, uint32_t sent, uint32_t limit) {
  return int32_t(now - sent) >= int32_t(limit);
}

// ISO-TP Single Frame, servico 01. ID bruto inclui flags EFF/RTR da mcp_can.
constexpr bool obdPositive(unsigned long id, uint8_t dlc, const uint8_t *d,
                        unsigned long ecu, uint8_t pid, uint8_t &bytes) {
  if (id != ecu || dlc < 4 || dlc > 8 || (d[0] & 0xF0) ||
      d[0] < 3 || d[0] > 7 || d[0] + 1 > dlc ||
      d[1] != 0x41 || d[2] != pid) return false;
  bytes = d[0] - 2;
  return true;
}

constexpr bool obdSupported(uint32_t map, uint8_t offset) {
  return offset >= 1 && offset <= 32 &&
         (map & (uint32_t(1) << (32 - offset))) != 0;
}

constexpr uint32_t obdMap(const uint8_t *d) {
  return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) |
         (uint32_t(d[2]) << 8) | uint32_t(d[3]);
}

inline bool primaryPid(uint8_t pid) { return pid == 0x0C || pid == 0x0D || pid == 0x05; }
inline bool mapPid(uint8_t pid) { return pid == 0 || (pid % 32) == 0; }

constexpr uint8_t expectedBytes(uint8_t pid) {
  switch (pid) {
    case 0x0C: case 0x10: case 0x1F: case 0x21:
    case 0x31: case 0x42: case 0x5E: return 2;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x0B:
    case 0x0D: case 0x0E: case 0x0F: case 0x11: case 0x2F:
    case 0x33: case 0x46: case 0x52: case 0x5C: return 1;
    default: return 0;
  }
}

inline const char *pidName(uint8_t pid) {
  switch (pid) {
    case 0x04: return "Carga do motor";
    case 0x05: return "Arrefecimento";
    case 0x06: return "Correcao curta de combustivel";
    case 0x07: return "Correcao longa de combustivel";
    case 0x0B: return "Pressao absoluta da admissao";
    case 0x0C: return "Rotacao";
    case 0x0D: return "Velocidade";
    case 0x0E: return "Avanco da ignicao";
    case 0x0F: return "Temperatura da admissao";
    case 0x10: return "Fluxo de ar";
    case 0x11: return "Posicao da borboleta";
    case 0x1F: return "Tempo desde a partida";
    case 0x21: return "Distancia com luz de falha";
    case 0x2F: return "Nivel de combustivel";
    case 0x31: return "Distancia desde apagar falhas";
    case 0x33: return "Pressao atmosferica";
    case 0x42: return "Tensao informada pela ECU";
    case 0x46: return "Temperatura ambiente";
    case 0x52: return "Etanol no combustivel";
    case 0x5C: return "Temperatura do oleo";
    case 0x5E: return "Vazao de combustivel";
    default: return "Dados brutos";
  }
}

inline const char *pidUnit(uint8_t pid) {
  switch (pid) {
    case 0x0C: return "rpm";
    case 0x0D: return "km/h";
    case 0x05: case 0x0F: case 0x46: case 0x5C: return "C";
    case 0x0B: case 0x33: return "kPa";
    case 0x10: return "g/s";
    case 0x0E: return "graus";
    case 0x1F: return "s";
    case 0x21: case 0x31: return "km";
    case 0x42: return "V";
    case 0x5E: return "L/h";
    default: return expectedBytes(pid) ? "%" : "hex";
  }
}

constexpr bool obdDecode(uint8_t pid, const uint8_t *d, uint8_t bytes, float &v) {
  uint8_t expected = expectedBytes(pid);
  if (!expected || bytes != expected) return false;
  uint16_t ab = expected == 2 ? (uint16_t(d[0]) << 8) | d[1] : d[0];
  switch (pid) {
    case 0x0C: v = ab / 4.0f; break;
    case 0x0D: case 0x0B: case 0x33: v = d[0]; break;
    case 0x05: case 0x0F: case 0x46: case 0x5C: v = int(d[0]) - 40; break;
    case 0x06: case 0x07: v = d[0] / 1.28f - 100; break;
    case 0x0E: v = d[0] / 2.0f - 64; break;
    case 0x10: v = ab / 100.0f; break;
    case 0x1F: case 0x21: case 0x31: v = ab; break;
    case 0x42: v = ab / 1000.0f; break;
    case 0x5E: v = ab / 20.0f; break;
    default: v = d[0] * 100.0f / 255; break;
  }
  return true;
}
