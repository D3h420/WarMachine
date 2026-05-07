#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>

// C5 wiring:
// SD: SCK D8, MISO D9, MOSI D10, CS D2
// C5 <- C6 link: C5 D1 (RX) <- C6 D0 (TX)
// Optional return: C5 D0 (TX) -> C6 D1 (RX)

static const int PIN_SD_CS = D2;
static const int PIN_SD_SCK = D8;
static const int PIN_SD_MISO = D9;
static const int PIN_SD_MOSI = D10;

static const int PIN_LINK_RX = D1;
static const int PIN_LINK_TX = D0;

static const uint32_t LINK_BAUD = 115200;
static const uint32_t SCAN_INTERVAL_MS = 5000;
static const char *LOG_FILE = "/wardrive.csv";

HardwareSerial LinkUart(1);
SPIClass spiBus(FSPI);

struct GpsState {
  bool valid = false;
  uint32_t msgMs = 0;
  float lat = NAN;
  float lon = NAN;
  float alt = NAN;
  int sats = 0;
  float hdop = NAN;
  String dateStr = "0-0-0";
  String timeStr = "0:0:0";
};

GpsState gps;
uint32_t lastScanMs = 0;

String csvEscape(const String &in) {
  if (in.indexOf(',') < 0 && in.indexOf('"') < 0) {
    return in;
  }

  String out = "\"";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') out += '"';
    out += c;
  }
  out += "\"";
  return out;
}

void ensureLogHeader() {
  if (!SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (f) {
      f.println("device_ms,gps_msg_ms,gps_valid,lat,lon,alt_m,sats,hdop,date_utc,time_utc,ssid,bssid,rssi,channel,encryption");
      f.close();
      Serial.println("[C5] Created log header");
    }
  }
}

bool parseGpsLine(const String &line) {
  // Format:
  // GPS,msgMs,lat,lon,alt,sats,hdop,date,time,valid
  if (!line.startsWith("GPS,")) return false;

  String fields[10];
  int fieldIdx = 0;
  int start = 4;

  for (int i = 4; i <= (int)line.length(); i++) {
    bool atEnd = (i == (int)line.length());
    if (atEnd || line[i] == ',') {
      if (fieldIdx < 10) {
        fields[fieldIdx++] = line.substring(start, i);
      }
      start = i + 1;
    }
  }

  if (fieldIdx != 10) return false;

  gps.msgMs = (uint32_t)fields[0].toInt();
  gps.lat = fields[1].toFloat();
  gps.lon = fields[2].toFloat();
  gps.alt = fields[3].toFloat();
  gps.sats = fields[4].toInt();
  gps.hdop = fields[5].toFloat();
  gps.dateStr = fields[6];
  gps.timeStr = fields[7];
  gps.valid = (fields[9].toInt() == 1);

  return true;
}

void pollGpsUart() {
  static String line;

  while (LinkUart.available() > 0) {
    char c = (char)LinkUart.read();
    if (c == '\n') {
      line.trim();
      if (line.length() > 0 && parseGpsLine(line)) {
        Serial.println("[C5] GPS update: " + line);
      }
      line = "";
    } else {
      line += c;
      if (line.length() > 220) {
        line = "";
      }
    }
  }
}

String encTypeToText(wifi_auth_mode_t enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "UNKNOWN";
  }
}

void logScanOnce() {
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) {
    Serial.println("[C5] WiFi scan failed");
    return;
  }

  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[C5] Failed to open log file");
    return;
  }

  uint32_t nowMs = millis();
  for (int i = 0; i < n; i++) {
    String row;
    row.reserve(260);
    row += String(nowMs); row += ",";
    row += String(gps.msgMs); row += ",";
    row += (gps.valid ? "1" : "0"); row += ",";
    row += String(gps.lat, 6); row += ",";
    row += String(gps.lon, 6); row += ",";
    row += String(gps.alt, 2); row += ",";
    row += String(gps.sats); row += ",";
    row += String(gps.hdop, 2); row += ",";
    row += gps.dateStr; row += ",";
    row += gps.timeStr; row += ",";
    row += csvEscape(WiFi.SSID(i)); row += ",";
    row += WiFi.BSSIDstr(i); row += ",";
    row += String(WiFi.RSSI(i)); row += ",";
    row += String(WiFi.channel(i)); row += ",";
    row += encTypeToText(WiFi.encryptionType(i));
    f.println(row);
  }

  f.close();
  WiFi.scanDelete();
  Serial.printf("[C5] Logged %d AP entries\n", n);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  LinkUart.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(120);

  spiBus.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, spiBus, 8000000U)) {
    Serial.println("[C5] SD init failed");
  } else {
    Serial.println("[C5] SD init OK");
    ensureLogHeader();
  }

  Serial.println("[C5] Wardrive logger started");
}

void loop() {
  pollGpsUart();

  uint32_t now = millis();
  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    lastScanMs = now;
    logScanOnce();
  }
}
