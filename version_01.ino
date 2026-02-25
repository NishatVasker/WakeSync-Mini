/*
  ESP8266 (NodeMCU) + 0.96" OLED I2C + Buzzer = Real-life Alarm Clock
  - 12-hour clock with AM/PM
  - NTP time sync (Bangladesh UTC+6 by default)
  - Web UI to set alarm (HH:MM) + AM/PM + enable/disable + Snooze + Stop
  - Alarm melody (tone sweep) + escalating pattern
  - Snooze (default 5 minutes)
  - Saves alarm settings to EEPROM (persists after reboot)
  - WiFi auto-reconnect + NTP resync
  - OLED shows time, AM/PM, alarm status, WiFi IP (briefly), ringing status

  WIRING (NodeMCU):
  OLED: VCC->3.3V, GND->GND, SDA->D2(GPIO4), SCL->D1(GPIO5)
  Buzzer: + -> D5(GPIO14), - -> GND

  LIBS:
  - ESP8266 core for Arduino
  - Adafruit GFX
  - Adafruit SSD1306
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ================= OLED =================
// If your OLED is 128x64, set 64. If 128x32, set 32.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================= WIFI =================
const char* ssid = "qweiusdfigdfg";
const char* password = "lfghedfghdegikurhedghedugh";

// Bangladesh UTC+6 (change if needed)
const long gmtOffset_sec = 6 * 3600;
const int daylightOffset_sec = 0;

ESP8266WebServer server(80);

// ================= BUZZER =================
#define BUZZER_PIN D5

// ================= SETTINGS / EEPROM =================
struct AlarmSettings {
  uint8_t enabled;      // 0/1
  uint8_t hour12;       // 1..12
  uint8_t minute;       // 0..59
  uint8_t am;           // 1=AM, 0=PM
  uint8_t snoozeMin;    // e.g., 5
  uint32_t crc;         // simple checksum
};

AlarmSettings cfg;

// runtime
bool alarmRinging = false;
int lastTriggerKey = -1;        // prevent retriggering
time_t snoozeUntil = 0;         // epoch time for snooze end (0 = not snoozing)

// alarm sound pattern
unsigned long lastToneStepMs = 0;
int toneFreq = 1200;
int toneDir = 1;
bool toneOn = false;

// NTP / WiFi handling
unsigned long lastWiFiCheckMs = 0;
unsigned long lastNTPSyncCheckMs = 0;

// ----------------- utilities -----------------
uint32_t simpleCRC(const AlarmSettings& s) {
  // super-simple checksum (good enough for small config)
  uint32_t sum = 0;
  sum += s.enabled;
  sum += s.hour12;
  sum += s.minute;
  sum += s.am;
  sum += s.snoozeMin;
  sum = (sum * 2654435761UL) ^ 0xA5A5A5A5UL;
  return sum;
}

void loadSettings() {
  EEPROM.begin(64);
  EEPROM.get(0, cfg);

  if (cfg.hour12 < 1 || cfg.hour12 > 12 || cfg.minute > 59 || (cfg.am != 0 && cfg.am != 1) || cfg.snoozeMin < 1 || cfg.snoozeMin > 30) {
    // defaults
    cfg.enabled = 0;
    cfg.hour12 = 7;
    cfg.minute = 0;
    cfg.am = 1;        // AM
    cfg.snoozeMin = 5;
    cfg.crc = simpleCRC(cfg);
    EEPROM.put(0, cfg);
    EEPROM.commit();
    return;
  }

  uint32_t c = simpleCRC(cfg);
  if (cfg.crc != c) {
    // defaults if checksum mismatch
    cfg.enabled = 0;
    cfg.hour12 = 7;
    cfg.minute = 0;
    cfg.am = 1;
    cfg.snoozeMin = 5;
    cfg.crc = simpleCRC(cfg);
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }
}

void saveSettings() {
  cfg.crc = simpleCRC(cfg);
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

String htmlEsc(const String& x) {
  String s = x;
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String two(int v) {
  char b[3];
  snprintf(b, sizeof(b), "%02d", v);
  return String(b);
}

void to12h(int h24, int& hour12, bool& isAM) {
  isAM = (h24 < 12);
  int h = h24 % 12;
  if (h == 0) h = 12;
  hour12 = h;
}

int alarmHour24() {
  // cfg.hour12 + cfg.am => 24h
  // AM: 12 -> 0, 1..11 -> 1..11
  // PM: 12 -> 12, 1..11 -> 13..23
  if (cfg.am == 1) {
    return (cfg.hour12 == 12) ? 0 : cfg.hour12;
  } else {
    return (cfg.hour12 == 12) ? 12 : (cfg.hour12 + 12);
  }
}

bool timeSynced() {
  time_t now = time(nullptr);
  return now > 1700000000; // "recent" epoch guard
}

void showBoot(const String& l1, const String& l2) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(l1);
  display.println(l2);
  display.display();
}

void connectWiFiBlocking(unsigned long maxMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < maxMs) {
    delay(250);
  }
}

// ----------------- Web UI -----------------
String page() {
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>ESP8266 Alarm Clock</title></head>";
  s += "<body style='font-family:Arial;max-width:560px;margin:18px;line-height:1.35'>";
  s += "<h2>ESP8266 Alarm Clock</h2>";

  // current time
  if (timeSynced()) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    int h12; bool isAM;
    to12h(ti->tm_hour, h12, isAM);
    s += "<p><b>Current time:</b> ";
    s += String(h12) + ":" + two(ti->tm_min) + ":" + two(ti->tm_sec) + " " + (isAM ? "AM" : "PM");
    s += "</p>";
  } else {
    s += "<p><b>Current time:</b> syncing...</p>";
  }

  // alarm status
  s += "<p><b>Alarm:</b> ";
  s += (cfg.enabled ? "Enabled" : "Disabled");
  s += " — ";
  s += String(cfg.hour12) + ":" + two(cfg.minute) + " " + (cfg.am ? "AM" : "PM");
  s += "</p>";

  if (snoozeUntil > 0) {
    time_t now = time(nullptr);
    if (now < snoozeUntil) {
      int remain = (int)(snoozeUntil - now);
      s += "<p><b>Snoozing:</b> ";
      s += String(remain) + "s remaining</p>";
    }
  }

  if (alarmRinging) {
    s += "<p style='color:#b00020'><b>ALARM RINGING!</b></p>";
  }

  s += "<hr>";

  // Set alarm form
  s += "<form action='/set' method='POST'>";
  s += "<label><b>Set alarm</b></label><br><br>";

  s += "Hour (1-12): <input name='h' type='number' min='1' max='12' value='" + String(cfg.hour12) + "' style='padding:8px;font-size:16px;width:90px' required> ";
  s += "Minute (0-59): <input name='m' type='number' min='0' max='59' value='" + String(cfg.minute) + "' style='padding:8px;font-size:16px;width:110px' required><br><br>";

  s += "AM/PM: <select name='ap' style='padding:8px;font-size:16px'>";
  s += String("<option value='AM'") + (cfg.am ? " selected" : "") + ">AM</option>";
  s += String("<option value='PM'") + (!cfg.am ? " selected" : "") + ">PM</option>";
  s += "</select><br><br>";

  s += "<label><input type='checkbox' name='en' value='1' ";
  if (cfg.enabled) s += "checked";
  s += "> Enable alarm</label><br><br>";

  s += "Snooze minutes: <input name='sz' type='number' min='1' max='30' value='" + String(cfg.snoozeMin) + "' style='padding:8px;font-size:16px;width:120px' required><br><br>";

  s += "<button style='padding:10px 16px;font-size:16px'>Save</button>";
  s += "</form>";

  s += "<hr>";

  // controls
  s += "<form action='/stop' method='POST' style='display:inline-block;margin-right:10px;'>";
  s += "<button style='padding:10px 16px;font-size:16px'>Stop</button></form>";

  s += "<form action='/snooze' method='POST' style='display:inline-block;'>";
  s += "<button style='padding:10px 16px;font-size:16px'>Snooze</button></form>";

  s += "<hr><p><b>IP:</b> " + WiFi.localIP().toString() + "</p>";
  s += "</body></html>";
  return s;
}

void handleRoot() {
  server.send(200, "text/html", page());
}

void handleSet() {
  if (!server.hasArg("h") || !server.hasArg("m") || !server.hasArg("ap") || !server.hasArg("sz")) {
    server.send(400, "text/plain", "Missing fields");
    return;
  }

  int h = server.arg("h").toInt();
  int m = server.arg("m").toInt();
  String ap = server.arg("ap");
  int sz = server.arg("sz").toInt();

  if (h < 1 || h > 12 || m < 0 || m > 59 || sz < 1 || sz > 30) {
    server.send(400, "text/plain", "Invalid values");
    return;
  }

  cfg.hour12 = (uint8_t)h;
  cfg.minute = (uint8_t)m;
  cfg.am = (ap == "AM") ? 1 : 0;
  cfg.enabled = server.hasArg("en") ? 1 : 0;
  cfg.snoozeMin = (uint8_t)sz;

  saveSettings();

  // stop current alarm when updating
  alarmRinging = false;
  snoozeUntil = 0;
  noTone(BUZZER_PIN);
  lastTriggerKey = -1;

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStop() {
  alarmRinging = false;
  snoozeUntil = 0;
  noTone(BUZZER_PIN);
  lastTriggerKey = -1;

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSnooze() {
  // only snooze if currently ringing and time is synced
  if (alarmRinging && timeSynced()) {
    time_t now = time(nullptr);
    snoozeUntil = now + (time_t)cfg.snoozeMin * 60;
  }
  alarmRinging = false;
  noTone(BUZZER_PIN);
  lastTriggerKey = -1;

  server.sendHeader("Location", "/");
  server.send(303);
}

// ----------------- Alarm Sound (better for real life) -----------------
void alarmSoundUpdate() {
  if (!alarmRinging) {
    noTone(BUZZER_PIN);
    return;
  }

  // Pattern:
  // - Tone "warble" between ~1200 and ~2500 Hz
  // - Duty pulse: 350ms ON, 150ms OFF, repeating
  // - Frequency slowly increases to feel more urgent
  static unsigned long patternMs = 0;
  unsigned long nowMs = millis();

  if (patternMs == 0) patternMs = nowMs;

  unsigned long phase = (nowMs - patternMs) % 500; // 500ms cycle
  bool shouldOn = (phase < 350);

  // update frequency every ~60ms when ON
  if (shouldOn) {
    if (nowMs - lastToneStepMs > 60) {
      lastToneStepMs = nowMs;

      // warble
      toneFreq += toneDir * 120;
      if (toneFreq >= 2500) toneDir = -1;
      if (toneFreq <= 1200) toneDir = 1;

      tone(BUZZER_PIN, toneFreq);
    }
  } else {
    noTone(BUZZER_PIN);
  }
}

// ----------------- OLED UI -----------------
void drawOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Row 1: time big + AM/PM
  if (timeSynced()) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);

    int h12; bool isAM;
    to12h(ti->tm_hour, h12, isAM);

    // blinking colon
    bool colonOn = (ti->tm_sec % 2 == 0);
    char line1[16];
    snprintf(line1, sizeof(line1), "%2d%c%02d", h12, (colonOn ? ':' : ' '), ti->tm_min);

    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print(line1);

    display.setTextSize(1);
    display.setCursor(100, 6);
    display.print(isAM ? "AM" : "PM");

  } else {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Syncing time...");
  }

  // Row 2: alarm status + ringing/snooze
  display.setTextSize(1);
  display.setCursor(0, 20);

  char a[24];
  snprintf(a, sizeof(a), "A:%s %02d:%02d%s",
           cfg.enabled ? "ON " : "OFF",
           (int)cfg.hour12, (int)cfg.minute,
           cfg.am ? "A" : "P");
  display.print(a);

  if (alarmRinging) {
    display.setCursor(86, 20);
    display.print("RING");
  } else if (snoozeUntil > 0 && timeSynced()) {
    time_t now = time(nullptr);
    if (now < snoozeUntil) {
      display.setCursor(86, 20);
      display.print("SNZ");
    } else {
      snoozeUntil = 0;
    }
  }

  display.display();
}

// ----------------- Trigger Logic -----------------
void updateAlarmLogic() {
  if (!cfg.enabled) return;
  if (!timeSynced()) return;

  time_t now = time(nullptr);

  // Snooze has priority
  if (snoozeUntil > 0) {
    if (now >= snoozeUntil) {
      snoozeUntil = 0;
      alarmRinging = true;
      lastTriggerKey = -1;
    }
    return;
  }

  struct tm* ti = localtime(&now);
  if (!ti) return;

  int aH24 = alarmHour24();

  // Trigger at exact minute boundary; guard against multiple triggers
  if (ti->tm_hour == aH24 && ti->tm_min == (int)cfg.minute && ti->tm_sec == 0 && !alarmRinging) {
    int key = (ti->tm_yday * 24 * 60) + (ti->tm_hour * 60) + ti->tm_min;
    if (key != lastTriggerKey) {
      lastTriggerKey = key;
      alarmRinging = true;
      // reset sound state
      toneFreq = 1200;
      toneDir = 1;
      lastToneStepMs = 0;
    }
  }
}

// ----------------- WiFi / NTP robustness -----------------
void maintainWiFiAndTime() {
  unsigned long nowMs = millis();

  // check wifi every 5s
  if (nowMs - lastWiFiCheckMs > 5000) {
    lastWiFiCheckMs = nowMs;

    if (WiFi.status() != WL_CONNECTED) {
      connectWiFiBlocking(8000);
    }
  }

  // if time isn't synced yet, keep trying (every 10s)
  if (nowMs - lastNTPSyncCheckMs > 10000) {
    lastNTPSyncCheckMs = nowMs;

    if (!timeSynced() && WiFi.status() == WL_CONNECTED) {
      configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
    }
  }
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  Wire.begin(D2, D1); // SDA, SCL

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // 0x3C confirmed by your scanner
    while (true);
  }

  loadSettings();

  showBoot("ESP8266 Alarm", "WiFi connecting...");
  connectWiFiBlocking(15000);

  showBoot("WiFi status:", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("FAILED"));

  // NTP time
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
  }

  // wait up to ~15s for time (non-fatal if it doesn't)
  unsigned long start = millis();
  while (!timeSynced() && millis() - start < 15000) {
    delay(300);
  }

  // Web server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/snooze", HTTP_POST, handleSnooze);
  server.begin();
}

void loop() {
  server.handleClient();

  maintainWiFiAndTime();
  updateAlarmLogic();
  alarmSoundUpdate();
  drawOLED();

  delay(80);
}
