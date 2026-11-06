// Voyage 2015 1.0: logger CAN OBD + ADXL345 + microSD
// Arquivo Unificado (1 unico CSV a 50Hz) | 2 SPIs Separadas (Pinagem MECU)
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <mcp_can.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "obd_core.h"
#include "obd_checks.h"

// --- Pinos SPI do CAN (Igual MECU) ---
constexpr int PIN_CAN_SCK = 14;
constexpr int PIN_CAN_MISO = 12;
constexpr int PIN_CAN_MOSI = 13;
constexpr int PIN_CAN_CS = 15;

// --- Pinos SPI do Módulo SD (Igual MECU) ---
constexpr int PIN_SD_SCK = 18;
constexpr int PIN_SD_MISO = 19;
constexpr int PIN_SD_MOSI = 23;
constexpr int PIN_SD_CS = 5;

// --- Pinos I2C ---
constexpr int PIN_I2C_SDA = 21;
constexpr int PIN_I2C_SCL = 22;

constexpr byte MCP_CRISTAL = MCP_8MHZ;
constexpr unsigned long ECU_RX = 0x7E8;
constexpr unsigned long ECU_TX = 0x7E0;
constexpr uint32_t INTERVALO_CAN_MS = 80;  
constexpr uint32_t TIMEOUT_OBD_MS = 1200;
constexpr uint32_t RETENTAR_MAPA_MS = 15000;
constexpr byte TENTATIVAS_MAPA = 3;

const byte PIDS[] = {0x0C, 0x0D, 0x05, 0x1F, 0x06, 0x04};
constexpr byte N_PIDS = sizeof(PIDS) / sizeof(PIDS[0]);

constexpr uint8_t ADXL_ADDR = 0x53;  
constexpr uint8_t ADXL_DEVID = 0x00;
constexpr uint8_t ADXL_BW_RATE = 0x2C;
constexpr uint8_t ADXL_POWER_CTL = 0x2D;
constexpr uint8_t ADXL_DATA_FORMAT = 0x31;
constexpr uint8_t ADXL_DATAX0 = 0x32;
constexpr uint32_t PERIODO_ADXL_MS = 20;  // 50 Hz
constexpr float ADXL_G_POR_LSB = 0.0039f; 

SPIClass sdSPI(HSPI); 
MCP_CAN canBus(&SPI, PIN_CAN_CS); 
File logFile;
QueueHandle_t accelQueue = nullptr;

struct AccelSample {
  uint32_t timestampMs;
  float xG;
  float yG;
  float zG;
};

float canValores[N_PIDS] = {};
bool canValido[N_PIDS] = {};
byte falhasPid[N_PIDS] = {};
byte cursorPid = 0;
byte pidPendente = 0;
int8_t indicePendente = -1;
uint32_t pidSuportados = 0;
uint32_t cicloVisto = 0;
byte tentativasMapa = 0;

bool canPronto = false;
bool obdAtivo = false;
bool descobrindo = false;
bool aguardando = false;
bool viuCAN = false;
bool sdPronto = false;
bool adxlPronto = false;

uint32_t ultimoCAN = 0;
uint32_t ultimaTentativa = 0;
uint32_t ultimoEnvio = 0;
uint32_t enviadoEm = 0;
uint32_t ultimoFlush = 0;
uint32_t ultimaEstatistica = 0;
uint32_t pedidosCAN = 0;
uint32_t respostasCAN = 0;
uint32_t timeoutsCAN = 0;
volatile uint32_t amostrasADXL = 0;
volatile uint32_t errosADXL = 0;
volatile uint32_t descartesADXL = 0;

char logNome[14] = {};
char bufferLog[4096] = {};
size_t usadosLog = 0;

const char *nomePID(byte pid) {
  switch (pid) {
    case 0x0C: return "RPM";
    case 0x0D: return "velocidade";
    case 0x05: return "temperatura_motor";
    case 0x1F: return "tempo_motor";
    case 0x06: return "correcao_combustivel_banco1";
    case 0x04: return "carga_motor";
    default: return "PID";
  }
}

bool canWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool canReadRegisters(uint8_t firstReg, uint8_t *data, size_t length) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(firstReg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((uint8_t)ADXL_ADDR, (uint8_t)length, (uint8_t)true);
  if (got != length) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < length; ++i) data[i] = Wire.read();
  return true;
}

bool iniciarADXL() {
  uint8_t id = 0;
  if (!canReadRegisters(ADXL_DEVID, &id, 1) || id != 0xE5) return false;
  if (!canWriteRegister(ADXL_POWER_CTL, 0x00)) return false; 
  if (!canWriteRegister(ADXL_DATA_FORMAT, 0x09)) return false;
  if (!canWriteRegister(ADXL_BW_RATE, 0x09)) return false; 
  if (!canWriteRegister(ADXL_POWER_CTL, 0x08)) return false; 
  return true;
}

void tarefaAcelerometro(void *) {
  TickType_t acordar = xTaskGetTickCount();
  const TickType_t periodo = pdMS_TO_TICKS(PERIODO_ADXL_MS);
  for (;;) {
    vTaskDelayUntil(&acordar, periodo);
    uint8_t raw[6];
    if (!canReadRegisters(ADXL_DATAX0, raw, sizeof(raw))) {
      ++errosADXL;
      continue;
    }
    int16_t x = (int16_t)((uint16_t)raw[1] << 8 | raw[0]);
    int16_t y = (int16_t)((uint16_t)raw[3] << 8 | raw[2]);
    int16_t z = (int16_t)((uint16_t)raw[5] << 8 | raw[4]);
    AccelSample amostra{millis(), x * ADXL_G_POR_LSB, y * ADXL_G_POR_LSB, z * ADXL_G_POR_LSB};
    if (xQueueSend(accelQueue, &amostra, 0) == pdTRUE) ++amostrasADXL;
    else ++descartesADXL;
  }
}

bool iniciarCAN(bool comFiltros) {
  if (canBus.begin(MCP_STDEXT, CAN_500KBPS, MCP_CRISTAL) != CAN_OK) return false;
  if (comFiltros) {
    for (byte m = 0; m < 2; ++m)
      if (canBus.init_Mask(m, 0, obdStandardFilter(0x7FF)) != CAN_OK) return false;
    for (byte f = 0; f < 6; ++f)
      if (canBus.init_Filt(f, 0, obdStandardFilter(ECU_RX)) != CAN_OK) return false;
  }
  if (canBus.enOneShotTX() != CAN_OK) return false;
  return canBus.setMode(comFiltros ? MCP_NORMAL : MCP_LISTENONLY) == CAN_OK;
}

void adicionarLog(const char *linha) {
  size_t length = strlen(linha);
  if (!sdPronto || !logFile || length >= sizeof(bufferLog)) return;
  if (usadosLog + length >= sizeof(bufferLog)) {
    size_t pending = usadosLog;
    size_t written = logFile.write((const uint8_t *)bufferLog, pending);
    usadosLog = 0;
    if (written != pending) {
      sdPronto = false;
      Serial.println("[ERRO SD] Falha gravando log unificado no SD.");
      return;
    }
  }
  memcpy(bufferLog + usadosLog, linha, length);
  usadosLog += length;
}

void flushLogs(bool forcado = false) {
  if (!sdPronto || !logFile || usadosLog == 0) return;
  if (!forcado && uint32_t(millis() - ultimoFlush) < 500) return;
  size_t written = logFile.write((const uint8_t *)bufferLog, usadosLog);
  if (written != usadosLog) {
    sdPronto = false;
    Serial.println("[ERRO SD] Falha gravando buffer no SD.");
  } else {
    logFile.flush();
  }
  usadosLog = 0;
  ultimoFlush = millis();
}

void iniciarSD() {
  Serial.println("\n[DEBUG SD] Montando SD via HSPI (Pinos: SCK=18, MISO=19, MOSI=23, CS=5)...");
  
  if (!SD.begin(PIN_SD_CS, sdSPI, 4000000)) {
    Serial.println("[ERRO SD] SD.begin() falhou!");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[ERRO SD] Nenhum cartao detectado.");
    return;
  }
  
  sdPronto = true;
  bool nomesEncontrados = false;
  for (uint16_t n = 1; n <= 9999; ++n) {
    snprintf(logNome, sizeof(logNome), "/LOG%04u.CSV", n);
    if (!SD.exists(logNome)) {
      nomesEncontrados = true;
      break;
    }
  }
  
  if (!nomesEncontrados) {
    sdPronto = false;
    Serial.println("[ERRO SD] Sem nomes livres (LOG0001 a LOG9999).");
    return;
  }

  logFile = SD.open(logNome, FILE_WRITE);

  if (!logFile) {
    sdPronto = false;
    Serial.println("[ERRO SD] Falha criando CSV unificado. Formate o cartao em FAT32!");
    return;
  }

  logFile.println("timestamp_ms,ax_g,ay_g,az_g,rpm,velocidade_kmh,temperatura_motor_C,tempo_motor_s,correcao_combustivel_banco1_pct,carga_motor_pct");
  logFile.flush();
  Serial.printf("[DEBUG SD] Sucesso: %s unificado criado.\n", logNome);
}

void formatarCampo(char *destino, size_t tamanho, byte indice, uint8_t casas) {
  if (!canValido[indice]) {
    destino[0] = '\0';
    return;
  }
  if (casas == 0) snprintf(destino, tamanho, "%.0f", canValores[indice]);
  else snprintf(destino, tamanho, "%.*f", casas, canValores[indice]);
}

void imprimirRodadaCAN() {
  char valor[N_PIDS][20];
  for (byte i = 0; i < N_PIDS; ++i) formatarCampo(valor[i], sizeof(valor[i]), i, i == 0 || i == 3 ? 0 : 2);
  Serial.printf("CAN snapshot | RPM=%s vel=%s km/h temp=%s C tempo=%s s trim=%s%% carga=%s%%\n",
                valor[0], valor[1], valor[2], valor[3], valor[4], valor[5]);
}

void registrarRespostaPID(byte indice, const byte *dados, byte bytes) {
  float valor = 0;
  if (!obdDecode(PIDS[indice], dados, bytes, valor)) {
    canValido[indice] = false;
  } else {
    canValores[indice] = valor;
    canValido[indice] = true;
    falhasPid[indice] = 0;
    ++respostasCAN;
  }
  cicloVisto |= uint32_t(1) << indice;
  if (pidSuportados && (cicloVisto & pidSuportados) == pidSuportados) {
    imprimirRodadaCAN();
    cicloVisto = 0;
  }
}

void falhaPIDAtual(const char *motivo) {
  if (indicePendente < 0 || indicePendente >= N_PIDS) return;
  byte i = (byte)indicePendente;
  canValido[i] = false;
  if (falhasPid[i] < 255) ++falhasPid[i];
  cicloVisto |= uint32_t(1) << i;
  if (falhasPid[i] >= 3) {
    pidSuportados &= ~(uint32_t(1) << i);
    cicloVisto &= pidSuportados;
  }
  if (pidSuportados && (cicloVisto & pidSuportados) == pidSuportados) {
    imprimirRodadaCAN();
    cicloVisto = 0;
  }
}

void pararOBD(const char *motivo) {
  obdAtivo = descobrindo = aguardando = false;
  ultimaTentativa = millis();
  canPronto = iniciarCAN(false);
}

void pedirOBD(byte pid, int8_t indice) {
  byte dados[8] = {2, 1, pid, 0, 0, 0, 0, 0};
  pidPendente = pid;
  indicePendente = indice;
  aguardando = true;
  enviadoEm = ultimoEnvio = millis();
  ++pedidosCAN;
  unsigned long destino = (descobrindo && pid == 0) ? 0x7DF : ECU_TX;
  byte resultado = canBus.sendMsgBuf(destino, 0, 8, dados);
  if (resultado != CAN_OK) {
    aguardando = false;
    if (descobrindo) {
      ++tentativasMapa;
      if (tentativasMapa >= TENTATIVAS_MAPA) pararOBD("sem envio/resposta ao mapa 00");
    } else falhaPIDAtual("erro ao enviar pedido CAN");
  }
}

void iniciarLeituraOBD() {
  if (obdAtivo || !canPronto || !viuCAN || uint32_t(millis() - ultimoCAN) > 2000) return;
  if (!iniciarCAN(true)) {
    pararOBD("falha ao configurar os filtros");
    return;
  }
  memset(canValido, 0, sizeof(canValido));
  memset(falhasPid, 0, sizeof(falhasPid));
  cursorPid = 0;
  cicloVisto = pidSuportados = 0;
  tentativasMapa = 0;
  obdAtivo = descobrindo = true;
  aguardando = false;
  pedirOBD(0, -1);
}

int proximoIndicePID() {
  for (byte n = 0; n < N_PIDS; ++n) {
    byte i = cursorPid++;
    if (cursorPid >= N_PIDS) cursorPid = 0;
    if (pidSuportados & (uint32_t(1) << i)) return i;
  }
  return -1;
}

void processarMapa(const byte *dados) {
  uint32_t mapa = obdMap(dados);
  for (byte i = 0; i < N_PIDS; ++i) {
    if (obdSupported(mapa, PIDS[i])) pidSuportados |= uint32_t(1) << i;
  }
  descobrindo = false;
  obdAtivo = true;
  ultimoEnvio = millis();
}

void tratarRespostaCAN(unsigned long id, byte dlc, const byte *dados) {
  if (!obdAtivo || !aguardando || id != ECU_RX) return;
  if (dlc >= 4 && dlc <= 8 && dados[0] == 3 && dados[1] == 0x7F && dados[2] == 1) {
    if (dados[3] == 0x78) return; 
    aguardando = false;
    if (descobrindo) {
      ++tentativasMapa;
      if (tentativasMapa >= TENTATIVAS_MAPA) pararOBD("ECU nao respondeu ao mapa 00 apos tres tentativas");
    } else falhaPIDAtual("ECU recusou o pedido");
    return;
  }
  byte bytes = 0;
  if (!obdPositive(id, dlc, dados, ECU_RX, pidPendente, bytes)) return;
  if (descobrindo) {
    if (bytes != 4) return;
    aguardando = false;
    tentativasMapa = 0;
    processarMapa(dados + 3);
    return;
  }
  if (indicePendente < 0 || bytes != expectedBytes(pidPendente)) return;
  aguardando = false;
  registrarRespostaPID((byte)indicePendente, dados + 3, bytes);
}

void processarLogUnificadoPendente() {
  if (!accelQueue) return;
  AccelSample amostra;
  
  while (xQueueReceive(accelQueue, &amostra, 0) == pdTRUE) {
    char val[N_PIDS][10];
    for (byte i = 0; i < N_PIDS; ++i) formatarCampo(val[i], sizeof(val[i]), i, i == 0 || i == 3 ? 0 : 2);
    
    char linha[150];
    snprintf(linha, sizeof(linha), "%lu,%.4f,%.4f,%.4f,%s,%s,%s,%s,%s,%s\n",
             (unsigned long)amostra.timestampMs, amostra.xG, amostra.yG, amostra.zG,
             val[0], val[1], val[2], val[3], val[4], val[5]);
             
    adicionarLog(linha);
  }
}

void imprimirEstatisticas() {
  if (uint32_t(millis() - ultimaEstatistica) < 5000) return;
  ultimaEstatistica = millis();
  Serial.printf("Status: OBD=%s CAN=%lu | ADXL=%lu | SD=%s\n",
                obdAtivo ? "ativo" : "escuta", (unsigned long)respostasCAN,
                (unsigned long)amostrasADXL, sdPronto ? "OK" : "falha");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nVOYAGE CAN + ADXL345 + microSD | Pinagem da MECU");

  pinMode(PIN_CAN_CS, OUTPUT);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_CAN_CS, HIGH);
  digitalWrite(PIN_SD_CS, HIGH);

  SPI.begin(PIN_CAN_SCK, PIN_CAN_MISO, PIN_CAN_MOSI, PIN_CAN_CS);
  sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  canPronto = iniciarCAN(false);
  if (!canPronto) Serial.println("MCP2515 indisponivel.");
  
  iniciarSD(); 

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  adxlPronto = iniciarADXL();
  if (adxlPronto) {
    accelQueue = xQueueCreate(256, sizeof(AccelSample));
    if (accelQueue) {
      BaseType_t criada = xTaskCreatePinnedToCore(tarefaAcelerometro,
          "leitura_ADXL345", 4096, nullptr, 2, nullptr, 0);
      if (criada != pdPASS) adxlPronto = false;
    } else adxlPronto = false;
  }
  
  ultimaTentativa = millis() - RETENTAR_MAPA_MS;
}

void loop() {
  if (canPronto) {
    for (byte n = 0; n < 24 && canBus.checkReceive() == CAN_MSGAVAIL; ++n) {
      unsigned long id = 0;
      byte dlc = 0;
      byte dados[8] = {};
      if (canBus.readMsgBuf(&id, &dlc, dados) != CAN_OK) break;
      viuCAN = true;
      ultimoCAN = millis();
      tratarRespostaCAN(id, dlc, dados);
    }

    uint32_t agora = millis();
    if (!obdAtivo && viuCAN && uint32_t(agora - ultimaTentativa) >= RETENTAR_MAPA_MS)
      iniciarLeituraOBD();

    if (obdAtivo && aguardando && obdTimedOut(millis(), enviadoEm, TIMEOUT_OBD_MS)) {
      ++timeoutsCAN;
      aguardando = false;
      if (descobrindo) {
        ++tentativasMapa;
        if (tentativasMapa >= TENTATIVAS_MAPA) pararOBD("ECU nao respondeu ao mapa 00");
      } else falhaPIDAtual("timeout");
    }
    if (obdAtivo && !aguardando && uint32_t(millis() - ultimoEnvio) >= INTERVALO_CAN_MS) {
      if (descobrindo) pedirOBD(0, -1);
      else {
        int indice = proximoIndicePID();
        if (indice >= 0) pedirOBD(PIDS[indice], (int8_t)indice);
      }
    }
  }

  processarLogUnificadoPendente();
  flushLogs();
  imprimirEstatisticas();
  delay(1);
}