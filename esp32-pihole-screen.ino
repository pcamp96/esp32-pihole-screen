// ESP32 Pi-hole screen + pause button
// Board: ESP32-S3 N16R8 (Arduino IDE board "ESP32S3 Dev Module")
// Displays: SSD1306 128x64 I2C OLED (0x3C) and/or I2C LCD1602 (0x27, sometimes 0x3F)
// Pi-hole: v6 REST API (/api/auth, /api/stats/summary, /api/dns/blocking)
//
// Button press while blocking is on  -> pause blocking for PAUSE_SECONDS
// Button press while paused          -> turn blocking back on now
//
// Setup: no passwords in the source. The install page writes your Wi-Fi and
// Pi-hole settings into the BAKED block below before flashing. Without them
// (or when the button is held while the board starts, or Wi-Fi fails) it opens
// a Wi-Fi network called "PiholeSetup" with a setup page at 192.168.4.1.
//
// Libraries (Arduino IDE > Library Manager):
//   ArduinoJson (Benoit Blanchon), Adafruit SSD1306, Adafruit GFX Library,
//   LiquidCrystal I2C (Frank de Brabander), WiFiManager (tzapu)

#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LiquidCrystal_I2C.h>

// ---------- settings (filled in from the setup page) ----------
String PIHOLE_URL  = "http://pi.hole";   // e.g. http://192.168.1.2:8080, no trailing slash
String PIHOLE_PASS = "";                 // Pi-hole app password
int PAUSE_SECONDS  = 300;                // 5 minutes
String WIFI_SSID, WIFI_PASS;
Preferences prefs;

// Settings block the install page patches into the firmware image.
// The layout must match the field sizes in docs/index.html.
struct BakedConfig {
  char magic[16];   // "PHSCFG1-ZZZZZZZ" marks the block for the patcher
  char id[16];      // changes on every patch, so new settings get applied once
  char ssid[64];
  char wpass[64];
  char url[128];
  char pass[128];
  char pause[16];
};
__attribute__((used)) volatile const BakedConfig BAKED = { "PHSCFG1-ZZZZZZZ", "", "", "", "", "", "" };

String bakedField(const volatile char* p, size_t n) {
  String s;
  for (size_t i = 0; i < n && p[i]; i++) s += (char)p[i];
  return s;
}

String cleanUrl(String url) {
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (url.length() && !url.startsWith("http")) url = "http://" + url;
  return url;
}

// Screens are detected at boot: an OLED at 0x3C/0x3D and an LCD1602 at
// 0x27/0x3F. Whichever answers gets used; a missing one is skipped.
const int I2C_SDA    = 8;           // ESP32-S3 default I2C pins
const int I2C_SCL    = 9;
const int BUTTON_PIN = 4;           // button between GPIO 4 and GND
// Many low-cost S3 boards have a poorly tuned antenna and work better at lower
// transmit power. Raise to WIFI_POWER_19_5dBm if your board has a good one.
const wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;
const int LED_PIN    = 48;          // onboard RGB LED (38 on DevKitC-1 v1.1 boards)
// -----------------------------------

const unsigned long POLL_MS = 10000;

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
LiquidCrystal_I2C* lcd = nullptr;
bool haveOled = false;

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Status LED: blue = setup page, green = blocking, amber = paused, red = error
void led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(LED_PIN, r / 8, g / 8, b / 8); }

String sid;                         // Pi-hole session ID, reused between requests

struct Stats {
  bool ok = false;
  bool blocking = true;
  long pauseEndsMs = 0;             // millis() when a pause timer ends, 0 = none
  long total = 0, blocked = 0, gravity = 0, clients = 0;
  float percent = 0;
  String lastBlocked;
  String error;
} st;

unsigned long lastPoll = 0, lastDraw = 0;
bool lastButton = HIGH;
unsigned long lastButtonChange = 0;

// ---------- Pi-hole API ----------

bool login() {
  HTTPClient http;
  http.begin(String(PIHOLE_URL) + "/api/auth");
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["password"] = PIHOLE_PASS;
  String payload;
  serializeJson(body, payload);
  int code = http.POST(payload);
  String resp = http.getString();
  http.end();

  JsonDocument doc;
  if (code != 200 || deserializeJson(doc, resp)) {
    st.error = "Login failed " + String(code);
    return false;
  }
  if (!doc["session"]["valid"].as<bool>()) {
    st.error = "Wrong password";
    return false;
  }
  sid = doc["session"]["sid"].as<String>();
  return true;
}

// Sends a request with the session ID. Logs in again once if the session expired.
int api(const char* method, const String& path, const String& body, String& out) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (sid.isEmpty() && !login()) return -1;
    HTTPClient http;
    http.begin(String(PIHOLE_URL) + path);
    http.addHeader("X-FTL-SID", sid);
    if (body.length()) http.addHeader("Content-Type", "application/json");
    int code = http.sendRequest(method, body);
    out = http.getString();
    http.end();
    if (code == 401) { sid = ""; continue; }
    return code;
  }
  return 401;
}

void readBlocking(JsonDocument& doc) {
  st.blocking = String(doc["blocking"] | "enabled") == "enabled";
  if (doc["timer"].isNull()) st.pauseEndsMs = 0;
  else st.pauseEndsMs = millis() + (long)(doc["timer"].as<float>() * 1000);
}

void poll() {
  String resp;
  JsonDocument doc;

  int code = api("GET", "/api/stats/summary", "", resp);
  if (code != 200 || deserializeJson(doc, resp)) {
    st.ok = false;
    if (code != -1) st.error = "Stats error " + String(code);
    return;
  }
  st.total   = doc["queries"]["total"] | 0L;
  st.blocked = doc["queries"]["blocked"] | 0L;
  st.percent = doc["queries"]["percent_blocked"] | 0.0f;
  st.gravity = doc["gravity"]["domains_being_blocked"] | 0L;
  st.clients = doc["clients"]["active"] | 0L;

  if (api("GET", "/api/dns/blocking", "", resp) == 200 && !deserializeJson(doc, resp)) {
    readBlocking(doc);
  }
  if (api("GET", "/api/stats/recent_blocked?count=1", "", resp) == 200 && !deserializeJson(doc, resp)) {
    st.lastBlocked = doc["blocked"][0] | "";
  }
  st.ok = true;
  st.error = "";
}

void toggleBlocking() {
  // Paused -> blocking on (and clear the timer). On -> pause for PAUSE_SECONDS.
  String body = st.blocking
    ? "{\"blocking\":false,\"timer\":" + String(PAUSE_SECONDS) + "}"
    : String("{\"blocking\":true,\"timer\":null}");
  String resp;
  JsonDocument doc;
  if (api("POST", "/api/dns/blocking", body, resp) == 200 && !deserializeJson(doc, resp)) {
    readBlocking(doc);
  }
  lastPoll = millis();              // fresh stats on the next poll cycle
}

// ---------- display ----------

String fmtCount(long n) {
  if (n >= 1000000) return String(n / 1000000.0, 1) + "M";
  if (n >= 10000)   return String(n / 1000) + "k";
  return String(n);
}

String countdown() {
  long left = (st.pauseEndsMs - (long)millis()) / 1000;
  if (left < 0) left = 0;
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld:%02ld", left / 60, left % 60);
  return buf;
}

void draw() {
  String status = st.blocking ? "BLOCKING" : (st.pauseEndsMs ? "PAUSED " + countdown() : "OFF");

  if (st.error.startsWith("Join")) led(0, 0, 255);   // setup network or joining Wi-Fi
  else if (!st.ok)                 led(255, 0, 0);
  else if (st.blocking)            led(0, 255, 0);
  else                             led(255, 120, 0);

  if (haveOled) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  if (!st.ok) {
    oled.setTextSize(1);
    oled.println("Pi-hole");
    oled.println(st.error.length() ? st.error : "Connecting...");
  } else {
    oled.setTextSize(2);
    oled.println(status);
    oled.setTextSize(1);
    oled.println();
    oled.printf("Queries  %s\n", fmtCount(st.total).c_str());
    oled.printf("Blocked  %s (%.1f%%)\n", fmtCount(st.blocked).c_str(), st.percent);
    oled.printf("Clients  %ld  List %s\n", st.clients, fmtCount(st.gravity).c_str());
    oled.println(st.lastBlocked.substring(0, 21));
  }
  oled.display();
  }

  if (lcd) {
  char l1[17], l2[17];
  if (!st.ok) {
    snprintf(l1, sizeof(l1), "%-16s", "Pi-hole");
    snprintf(l2, sizeof(l2), "%-16.16s", (st.error.length() ? st.error : String("Connecting...")).c_str());
  } else {
    snprintf(l1, sizeof(l1), "%-16.16s", status.c_str());
    snprintf(l2, sizeof(l2), "Blk %-5.5s %5.1f%%", fmtCount(st.blocked).c_str(), st.percent);
  }
  lcd->setCursor(0, 0); lcd->print(l1);
  lcd->setCursor(0, 1); lcd->print(l2);
  }
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(50);
  led(0, 0, 255);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(20);

  for (uint8_t a : {0x3C, 0x3D}) {
    if (i2cPresent(a) && oled.begin(SSD1306_SWITCHCAPVCC, a)) { haveOled = true; break; }
  }
  for (uint8_t a : {0x27, 0x3F}) {
    if (i2cPresent(a)) {
      lcd = new LiquidCrystal_I2C(a, 16, 2);
      lcd->init();
      lcd->backlight();
      break;
    }
  }
  Serial.printf("OLED %s, LCD %s\n", haveOled ? "found" : "not found", lcd ? "found" : "not found");

  prefs.begin("pihole", false);

  // Settings written in by the install page are copied into storage once
  String bakedId = bakedField(BAKED.id, sizeof(BAKED.id));
  String bakedSsid = bakedField(BAKED.ssid, sizeof(BAKED.ssid));
  if (bakedSsid.length() && bakedId != prefs.getString("bakedId", "")) {
    prefs.putString("ssid", bakedSsid);
    prefs.putString("wpass", bakedField(BAKED.wpass, sizeof(BAKED.wpass)));
    prefs.putString("url", cleanUrl(bakedField(BAKED.url, sizeof(BAKED.url))));
    prefs.putString("pass", bakedField(BAKED.pass, sizeof(BAKED.pass)));
    prefs.putInt("pause", max(10, (int)bakedField(BAKED.pause, sizeof(BAKED.pause)).toInt()));
    prefs.putString("bakedId", bakedId);
    Serial.println("Applied settings from the install page");
  }

  WIFI_SSID     = prefs.getString("ssid", "");
  WIFI_PASS     = prefs.getString("wpass", "");
  PIHOLE_URL    = prefs.getString("url", PIHOLE_URL);
  PIHOLE_PASS   = prefs.getString("pass", PIHOLE_PASS);
  PAUSE_SECONDS = prefs.getInt("pause", PAUSE_SECONDS);

  bool needSetup = digitalRead(BUTTON_PIN) == LOW || WIFI_SSID.isEmpty() || PIHOLE_PASS.isEmpty();

  if (!needSetup) {
    st.error = "Joining " + WIFI_SSID;
    draw();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());
    WiFi.setTxPower(WIFI_TX_POWER);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi failed; opening setup network");
      needSetup = true;
    }
  }

  if (needSetup) {
    WiFiManagerParameter pUrl("url", "Pi-hole address (http://IP:port)", PIHOLE_URL.c_str(), 127);
    WiFiManagerParameter pPass("pass", "Pi-hole app password", PIHOLE_PASS.c_str(), 127, "type='password'");
    WiFiManagerParameter pPause("pause", "Pause length (seconds)", String(PAUSE_SECONDS).c_str(), 6);
    WiFiManager wm;
    wm.addParameter(&pUrl);
    wm.addParameter(&pPass);
    wm.addParameter(&pPause);
    wm.setConfigPortalTimeout(600);       // retry Wi-Fi after 10 minutes
    wm.setWiFiAPChannel(6);
    wm.setAPCallback([](WiFiManager*) { WiFi.setTxPower(WIFI_TX_POWER); });
    st.error = "Join PiholeSetup";
    draw();
    WiFi.disconnect(true);
    delay(100);
    if (!wm.startConfigPortal("PiholeSetup")) ESP.restart();
    prefs.putString("ssid", wm.getWiFiSSID());
    prefs.putString("wpass", wm.getWiFiPass());
    PIHOLE_URL = cleanUrl(pUrl.getValue());
    PIHOLE_PASS = pPass.getValue();
    PAUSE_SECONDS = max(10, atoi(pPause.getValue()));
    prefs.putString("url", PIHOLE_URL);
    prefs.putString("pass", PIHOLE_PASS);
    prefs.putInt("pause", PAUSE_SECONDS);
  }
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  st.error = "";
  poll();
  draw();
  lastPoll = millis();
}

void loop() {
  // Button (active low, 50 ms debounce)
  bool b = digitalRead(BUTTON_PIN);
  if (b != lastButton && millis() - lastButtonChange > 50) {
    lastButtonChange = millis();
    lastButton = b;
    if (b == LOW) { toggleBlocking(); draw(); }
  }

  if (WiFi.status() != WL_CONNECTED) {
    st.ok = false; st.error = "Wi-Fi lost";
    WiFi.reconnect();
    delay(1000);
  }

  // A finished pause re-enables blocking on Pi-hole's side; poll to pick it up
  bool pauseJustEnded = !st.blocking && st.pauseEndsMs && (long)millis() > st.pauseEndsMs;
  if (millis() - lastPoll > POLL_MS || pauseJustEnded) {
    lastPoll = millis();
    poll();
    if (pauseJustEnded && !st.blocking) st.pauseEndsMs = 0;  // avoid a tight loop if Pi-hole lags
  }

  if (millis() - lastDraw > 1000) { lastDraw = millis(); draw(); }
  delay(10);
}
