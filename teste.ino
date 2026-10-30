#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// A7670SA + Arduino Mega 2560. Apenas consulta estado 4G/GNSS.
// Nao usa MQTT, nao publica telemetria e nao inclui credenciais.
// Mega Serial1: RX1=19, TX1=18. Monitor Serial USB: 115200 baud.

static const uint32_t USB_BAUD = 115200;
static const uint32_t MODEM_BAUD = 115200;
static const uint32_t INTERVALO_STATUS_MS = 10000;
static const uint32_t INTERVALO_PING_MS = 60000;
static const char APN[] = "claro.com.br";
static const char APN_USER[] = "claro";
static const char APN_PASSWORD[] = "claro";

static char responseBuf[640];
static int csq = 99;
static bool gpsFix = false;
static bool gnssReadySeen = false;
static bool gnssReady = false;
static bool socketOpen = false;
static char operador[20] = "desconhecida";
static char lat[20] = "";
static char latHem[3] = "";
static char lon[20] = "";
static char lonHem[3] = "";
static uint32_t ultimoStatus = 0;
static uint32_t ultimoPing = 0;

static void clearResponse() { responseBuf[0] = '\0'; }

static void appendResponse(char c) {
  size_t n = strlen(responseBuf);
  if (n + 1 < sizeof(responseBuf)) {
    responseBuf[n] = c;
    responseBuf[n + 1] = '\0';
  }
}

static bool waitATResult(uint32_t timeoutMs, bool printResponse = true) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeoutMs) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      appendResponse(c);
      if (strstr(responseBuf, "+CGNSSPWR: READY!")) gnssReadySeen = true;
      if (strstr(responseBuf, "\nOK") || strstr(responseBuf, "\rOK") ||
          strstr(responseBuf, "\nERROR") || strstr(responseBuf, "\rERROR")) {
        if (printResponse && responseBuf[0]) Serial.print(responseBuf);
        return strstr(responseBuf, "\nOK") || strstr(responseBuf, "\rOK");
      }
    }
  }
  if (printResponse && responseBuf[0]) Serial.print(responseBuf);
  if (!responseBuf[0]) Serial.println(F("[ERRO] Sem resposta do modem; confira TX/RX, GND e baud rate."));
  return false;
}

static bool at(const char *cmd, uint32_t timeoutMs = 5000) {
  while (Serial1.available()) Serial1.read();
  clearResponse();
  Serial.print(F("\n[AT] ")); Serial.println(cmd);
  Serial1.print(cmd); Serial1.print("\r\n");
  return waitATResult(timeoutMs);
}

static bool atCapture(const char *cmd, uint32_t timeoutMs = 5000) {
  while (Serial1.available()) Serial1.read();
  clearResponse();
  Serial1.print(cmd); Serial1.print("\r\n");
  return waitATResult(timeoutMs, false);
}

static bool waitGnssReady(uint32_t timeoutMs) {
  if (gnssReadySeen) {
    Serial.println(F("[GNSS] Receptor GNSS informou READY."));
    return true;
  }
  clearResponse();
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeoutMs) {
    while (Serial1.available()) {
      appendResponse((char)Serial1.read());
      if (strstr(responseBuf, "+CGNSSPWR: READY!")) {
        gnssReadySeen = true;
        Serial.println(F("[GNSS] Receptor GNSS informou READY."));
        return true;
      }
    }
  }
  Serial.println(F("[GNSS] Nao recebi READY; vou consultar o estado de alimentacao mesmo assim."));
  return false;
}

static void identifyOperator() {
  // Consulta IMSI para identificar a operadora, mas nunca imprime o IMSI.
  if (!atCapture("AT+CIMI")) {
    Serial.println(F("[OPERADORA] Nao foi possivel ler a identificacao do SIM."));
    return;
  }
  char imsi[20] = "";
  for (size_t i = 0; responseBuf[i]; ++i) {
    if (responseBuf[i] >= '0' && responseBuf[i] <= '9') {
      size_t j = 0;
      while (responseBuf[i] >= '0' && responseBuf[i] <= '9' && j < sizeof(imsi)-1)
        imsi[j++] = responseBuf[i++];
      imsi[j] = '\0';
      break;
    }
  }
  if (!strncmp(imsi, "72405", 5) || !strncmp(imsi, "72412", 5)) strcpy(operador, "Claro");
  else if (!strncmp(imsi, "72402", 5) || !strncmp(imsi, "72403", 5) || !strncmp(imsi, "72404", 5)) strcpy(operador, "TIM");
  else if (!strncmp(imsi, "72406", 5) || !strncmp(imsi, "72410", 5) || !strncmp(imsi, "72411", 5)) strcpy(operador, "Vivo");
  else strcpy(operador, "SIM/M2M ou outra");
  Serial.print(F("[OPERADORA do SIM] ")); Serial.println(operador);
}

static void readSignal() {
  if (!at("AT+CSQ")) return;
  char *p = strstr(responseBuf, "+CSQ:");
  if (!p) return;
  csq = atoi(p + 5);
  Serial.print(F("[SINAL] CSQ=")); Serial.print(csq); Serial.print(F("/31"));
  if (csq >= 0 && csq <= 31) {
    Serial.print(F(" (aprox. ")); Serial.print(-113 + 2 * csq); Serial.println(F(" dBm)"));
  } else Serial.println(F(" (indisponivel)"));
}

static bool isRegistered() {
  if (!at("AT+CGREG?")) return false;
  return strstr(responseBuf, ",1\r") || strstr(responseBuf, ",1\n") ||
         strstr(responseBuf, ",5\r") || strstr(responseBuf, ",5\n");
}

static bool ensureSocketNetwork() {
  if (at("AT+NETOPEN?")) {
    if (strstr(responseBuf, "+NETOPEN: 1")) {
      socketOpen = true;
      return true;
    }
  }

  socketOpen = false;
  Serial.println(F("[DADOS] Abrindo servico de sockets (sem MQTT/publicacao)..."));
  at("AT+NETOPEN", 15000);
  // NETOPEN responde de forma assincrona; aguarda a confirmacao de estado.
  for (uint8_t i = 0; i < 6; ++i) {
    delay(1000);
    if (at("AT+NETOPEN?", 5000) && strstr(responseBuf, "+NETOPEN: 1")) {
      socketOpen = true;
      Serial.println(F("[DADOS] Servico de dados IP aberto."));
      return true;
    }
  }
  Serial.println(F("[DADOS] Servico IP ainda fechado; IPADDR e ping nao estao disponiveis."));
  return false;
}

static void readGps() {
  lat[0] = latHem[0] = lon[0] = lonHem[0] = '\0';
  gpsFix = false;
  if (!at("AT+CGNSSINFO", 10000)) return;
  char *line = strstr(responseBuf, "+CGNSSINFO:");
  if (!line) { Serial.println(F("[GPS] Aguardando fix GNSS.")); return; }
  line += strlen("+CGNSSINFO:");
  char *fields[16];
  int count = 0;
  if (*line == '\r' || *line == '\n') { while (*line == '\r' || *line == '\n') ++line; }
  if (*line == '\0') { Serial.println(F("[GPS] Aguardando fix GNSS.")); return; }
  fields[count++] = line;
  for (char *p = line; *p && count < 16; ++p) {
    if (*p == ',') { *p = '\0'; fields[count++] = p + 1; }
    if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
  }
  // Formato observado no A7670SA: mode,GPS,GLONASS,BEIDOU,extra,lat,N/S,lon,E/W,...
  if (count >= 9 && fields[5][0] && fields[7][0] &&
      (fields[6][0] == 'N' || fields[6][0] == 'S') &&
      (fields[8][0] == 'E' || fields[8][0] == 'W')) {
    strncpy(lat, fields[5], sizeof(lat)-1); lat[sizeof(lat)-1] = '\0';
    strncpy(latHem, fields[6], sizeof(latHem)-1); latHem[sizeof(latHem)-1] = '\0';
    strncpy(lon, fields[7], sizeof(lon)-1); lon[sizeof(lon)-1] = '\0';
    strncpy(lonHem, fields[8], sizeof(lonHem)-1); lonHem[sizeof(lonHem)-1] = '\0';
    gpsFix = true;
    Serial.print(F("[GPS FIX] lat=")); Serial.print(lat); Serial.print(latHem);
    Serial.print(F(" lon=")); Serial.print(lon); Serial.println(lonHem);
    Serial.println(F("          Coordenadas em graus decimais."));
  } else Serial.println(F("[GPS] Sem fix. Deixe a antena GNSS com visao aberta do ceu."));
}

static void runInternetPing() {
  // Apenas ICMP para teste de rota IP; nao envia telemetria nem dados MQTT.
  const char *cmd = "AT+CPING=\"1.1.1.1\",1,1,32,1000,10000,64";
  while (Serial1.available()) Serial1.read();
  clearResponse();
  Serial.println(F("\n[TESTE IP] Ping ICMP para 1.1.1.1 (sem publicar dados)."));
  Serial1.print(cmd); Serial1.print("\r\n");
  uint32_t start = millis();
  bool summary = false;
  while ((uint32_t)(millis() - start) < 20000) {
    while (Serial1.available()) {
      appendResponse((char)Serial1.read());
      char *summaryLine = strstr(responseBuf, "+CPING: 3,");
      if (summaryLine && strpbrk(summaryLine, "\r\n")) { summary = true; break; }
    }
    if (summary) break;
  }
  if (responseBuf[0]) Serial.print(responseBuf);
  if (strstr(responseBuf, "+CPING: 1,")) {
    Serial.println(F("[INTERNET] Ping respondeu: ha conectividade IP externa."));
  } else if (strstr(responseBuf, "+CPING: 3,")) {
    char *summaryLine = strstr(responseBuf, "+CPING: 3,");
    unsigned int sent = 0, received = 0, lost = 0;
    if (sscanf(summaryLine, "+CPING: 3,%u,%u,%u", &sent, &received, &lost) == 3 && received > 0) {
      Serial.print(F("[INTERNET] Ping respondeu: ")); Serial.print(received);
      Serial.print(F("/")); Serial.print(sent); Serial.println(F(" respostas; ha conectividade IP externa."));
    } else {
      Serial.println(F("[INTERNET] Ping sem resposta; ICMP pode estar bloqueado."));
    }
  } else Serial.println(F("[INTERNET] Sem resultado de ping; registro e IP mostram o estado da rede celular."));
}

static void setupCellular() {
  Serial.println(F("\n=== CONEXAO CELULAR CLARO ==="));
  while (!at("AT", 3000)) delay(2000);
  at("ATE0");
  at("AT+CMEE=2");
  at("AT+CPIN?");
  identifyOperator();
  at("AT+COPS?");
  readSignal();
  at("AT+CTZU=1");

  char cmd[100];
  snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
  at(cmd);
  snprintf(cmd, sizeof(cmd), "AT+CGAUTH=1,1,\"%s\",\"%s\"", APN_USER, APN_PASSWORD);
  at(cmd);

  bool registered = false;
  Serial.println(F("[REDE] Aguardando registro..."));
  for (uint8_t i = 0; i < 12 && !registered; ++i) {
    registered = isRegistered();
    if (!registered) delay(5000);
  }
  Serial.println(registered ? F("[REDE] Registrado na rede movel.") : F("[REDE] Ainda sem registro; confira SIM, cobertura e antena LTE."));
  at("AT+CGATT=1", 30000);
  at("AT+CGACT=1,1", 30000);
  at("AT+CGATT?");
  at("AT+CGACT?");
  if (ensureSocketNetwork()) at("AT+IPADDR");
  Serial.println(F("[GNSS] Ligando receptor e aguardando READY..."));
  at("AT+CGNSSPWR=1", 15000);
  gnssReady = waitGnssReady(30000);
  at("AT+CGNSSPWR?");
  if (gnssReady) {
    // Seleciona a porta GNSS usada pela sequência que funcionou no teste AT.
    at("AT+CGNSSPORTSWITCH=1,1");
    at("AT+CGNSSMODE?");
  } else {
    Serial.println(F("[GNSS] READY nao chegou; vou manter consultas periodicas a CGNSSINFO."));
  }
}

void setup() {
  Serial.begin(USB_BAUD);
  Serial1.begin(MODEM_BAUD);
  delay(1500);
  Serial.println(F("A7670SA + Mega | status 4G e GPS | sem MQTT/publicacao"));
  Serial.println(F("Monitor Serial USB: 115200 baud."));
  Serial.println(F("Feche o PuTTY da porta USB do modem antes de iniciar."));
  setupCellular();
  ultimoStatus = millis() - INTERVALO_STATUS_MS;
  ultimoPing = millis() - INTERVALO_PING_MS;
}

void loop() {
  uint32_t now = millis();
  if ((uint32_t)(now - ultimoStatus) >= INTERVALO_STATUS_MS) {
    ultimoStatus = now;
    Serial.println(F("\n=== STATUS 4G E GPS ==="));
    Serial.print(F("[OPERADORA do SIM] ")); Serial.println(operador);
    readSignal();
    bool registered = isRegistered();
    Serial.println(registered ? F("[REDE] Registrado") : F("[REDE] Nao registrado"));
    at("AT+CGATT?");
    at("AT+CGACT?");
    if (ensureSocketNetwork()) at("AT+IPADDR");
    readGps();
  }
  if ((uint32_t)(now - ultimoPing) >= INTERVALO_PING_MS) {
    ultimoPing = now;
    if (ensureSocketNetwork()) runInternetPing();
  }
}
