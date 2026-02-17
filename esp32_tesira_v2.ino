#include <WiFi.h>

// ====== CONFIGURATION ======
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* TELNET_HOST = "";
const uint16_t TELNET_PORT = 23;
const char* LEVEL_ALIAS = "Level3";

#define POT_PIN 34
#define MUTE_PIN 27

// ====== GLOBALS ======
WiFiClient client;
bool telnetReady = false;

float minLevel = -100.0;
float maxLevel = 12.0;
bool gotMin = false;
bool gotMax = false;

float lastSentLevel = -9999; // store as float for deadband
bool lastMuteState = false;

unsigned long lastReconnectAttempt = 0;
unsigned long lastCommandSent = 0;

const unsigned long COMMAND_INTERVAL = 150; // ms
const float LEVEL_DEADBAND = 1;            // dB change required to send

enum ExpectedResponse { NONE, EXPECT_MIN, EXPECT_MAX };
ExpectedResponse expectedResponse = NONE;

String telnetBuffer;

// ====== TELNET NEGOTIATION ======
void sendTelnetNegotiationResponse(uint8_t option) {
  client.write(0xFF); // IAC
  client.write(0xFC); // WONT
  client.write(option);
}

void handleTelnetNegotiation() {
  while (client.available()) {
    uint8_t byte = client.read();
    if (byte == 0xFF) { // IAC
      if (client.available() >= 2) {
        uint8_t command = client.read();
        uint8_t option = client.read();
        Serial.printf("Telnet negotiation: IAC %02X %02X\n", command, option);
        if (command == 0xFD || command == 0xFB) {
          sendTelnetNegotiationResponse(option);
        }
      }
    } else {
      if (byte == '\n') {
        processTelnetLine(telnetBuffer);
        telnetBuffer = "";
      } else if (byte != '\r') {
        telnetBuffer += (char)byte;
      }
    }
  }
}

void processTelnetLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  Serial.println("TELNET RX: " + line);

  if (!telnetReady) {
    telnetReady = true;
    Serial.println("Telnet handshake complete.");
    requestMinLevel();
  }

  if (line.startsWith("+OK")) {
    int idx = line.indexOf("\"value\":");
    if (idx != -1) {
      float val = line.substring(idx + 8).toFloat();
      if (expectedResponse == EXPECT_MIN) {
        minLevel = val;
        gotMin = true;
        Serial.printf("Fetched minLevel: %.2f\n", minLevel);
        expectedResponse = NONE;
        requestMaxLevel();
      } else if (expectedResponse == EXPECT_MAX) {
        maxLevel = val;
        gotMax = true;
        Serial.printf("Fetched maxLevel: %.2f\n", maxLevel);
        expectedResponse = NONE;
      }
    }
  }
}

// ====== NETWORK ======
void connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

bool connectTelnet() {
  Serial.printf("Connecting to Telnet server %s:%d...\n", TELNET_HOST, TELNET_PORT);
  if (client.connect(TELNET_HOST, TELNET_PORT)) {
    Serial.println("Telnet connected.");
    telnetReady = false;
    return true;
  }
  Serial.println("Telnet connection failed.");
  return false;
}

void ensureTelnetConnection() {
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 3000) {
      lastReconnectAttempt = now;
      Serial.println("Reconnecting to Telnet...");
      client.stop();
      connectTelnet();
    }
  }
}

// ====== COMMANDS ======
void requestMinLevel() {
  gotMin = false;
  expectedResponse = EXPECT_MIN;
  client.printf("%s get minLevel 1\r\n", LEVEL_ALIAS);
}

void requestMaxLevel() {
  gotMax = false;
  expectedResponse = EXPECT_MAX;
  client.printf("%s get maxLevel 1\r\n", LEVEL_ALIAS);
}

void sendLevelCommand(float level) {
  String cmd = String(LEVEL_ALIAS) + " set level 1 " + String(level, 1);
  Serial.printf("Sending level: %.1f\n", level);
  client.print(cmd + "\r\n");
}

void sendMuteCommand(bool mute) {
  String cmd = String(LEVEL_ALIAS) + " set mute 1 " + (mute ? "true" : "false");
  Serial.printf("Sending mute: %s\n", mute ? "true" : "false");
  client.print(cmd + "\r\n");
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  pinMode(MUTE_PIN, INPUT_PULLUP);
  connectWiFi();
  connectTelnet();
}

// ====== LOOP ======
void loop() {
  ensureTelnetConnection();
  handleTelnetNegotiation();

  if (!gotMin || !gotMax) return;

  unsigned long now = millis();
  if (now - lastCommandSent >= COMMAND_INTERVAL && client.connected()) {
    lastCommandSent = now;

    // Read potentiometer and map to dB range
    int potVal = analogRead(POT_PIN);
    float mappedLevel = map(potVal, 0, 4095, minLevel * 100, maxLevel * 100) / 100.0;

    // Only send if change is greater than deadband
    if (fabs(mappedLevel - lastSentLevel) >= LEVEL_DEADBAND) {
      lastSentLevel = mappedLevel;
      sendLevelCommand(mappedLevel);
    }

    // Read mute switch (reversed logic)
    bool muteState = (digitalRead(MUTE_PIN) == HIGH); // flipped
    if (muteState != lastMuteState) {
      lastMuteState = muteState;
      sendMuteCommand(muteState);
    }
  }
}