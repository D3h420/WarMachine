#include <Arduino.h>
#include <TinyGPSPlus.h>

// C6 wiring:
// GPS TX -> D7 (C6 RX), GPS RX <- D6 (C6 TX)
// C6 -> C5 link: D0 (TX) -> C5 D1 (RX)
// Optional return: C6 D1 (RX) <- C5 D0 (TX)

static const int PIN_GPS_RX = D7;
static const int PIN_GPS_TX = D6;
static const int PIN_LINK_RX = D1;
static const int PIN_LINK_TX = D0;

static const uint32_t GPS_BAUD = 9600;
static const uint32_t LINK_BAUD = 115200;

HardwareSerial GPSUart(1);
HardwareSerial LinkUart(2);
TinyGPSPlus gps;

uint32_t lastPublishMs = 0;
uint32_t lastHeartbeatMs = 0;

String buildGpsLine() {
  bool validLocation = gps.location.isValid();
  bool validAlt = gps.altitude.isValid();
  bool validHdop = gps.hdop.isValid();
  bool validSats = gps.satellites.isValid();
  bool validDate = gps.date.isValid();
  bool validTime = gps.time.isValid();

  String line = "GPS,";
  line += String(millis());
  line += ",";
  line += (validLocation ? String(gps.location.lat(), 6) : "nan");
  line += ",";
  line += (validLocation ? String(gps.location.lng(), 6) : "nan");
  line += ",";
  line += (validAlt ? String(gps.altitude.meters(), 2) : "nan");
  line += ",";
  line += (validSats ? String(gps.satellites.value()) : "0");
  line += ",";
  line += (validHdop ? String(gps.hdop.hdop(), 2) : "nan");
  line += ",";
  line += (validDate ? String(gps.date.year()) : "0");
  line += "-";
  line += (validDate ? String(gps.date.month()) : "0");
  line += "-";
  line += (validDate ? String(gps.date.day()) : "0");
  line += ",";
  line += (validTime ? String(gps.time.hour()) : "0");
  line += ":";
  line += (validTime ? String(gps.time.minute()) : "0");
  line += ":";
  line += (validTime ? String(gps.time.second()) : "0");
  line += ",";
  line += (validLocation ? "1" : "0");
  return line;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  GPSUart.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  LinkUart.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

  Serial.println("[C6] GPS bridge started");
}

void loop() {
  while (GPSUart.available() > 0) {
    gps.encode(GPSUart.read());
  }

  uint32_t now = millis();

  if (now - lastPublishMs >= 1000) {
    lastPublishMs = now;
    String line = buildGpsLine();
    LinkUart.println(line);
    Serial.println("[C6->C5] " + line);
  }

  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    LinkUart.println("HB," + String(now));
  }
}
