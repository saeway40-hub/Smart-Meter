// =====================================================================
//  NEHALE SMART PREPAID METER  -  ESP32 firmware
//  DPJ621S  |  Simon E. Nehale (223019267)
//
//  What it does
//   * Measures the real load (V from the 27k/10k divider, I from the ACS712)
//   * Deducts kWh from the balance using  E = V x I x time
//   * Starts at 0 kWh: load is OFF until a 3 kWh top-up is made
//   * Top-up three ways:  keypad token  |  SMS to the SIM900  |  web page (Google Sheets backend)
//   * Logs every reading to Google Sheets (usage history for the Colab model)
//   * Predicts kWh/day from its own daily history -> Days Left + recommended NAD top-up
//   * Cuts the load at zero balance or on overload (> 0.7 A)
//
//  Board : ESP32 Dev Module (Keyestudio ESP32, 38-pin)
//  Libraries: "LiquidCrystal I2C" (Frank de Brabander), "Keypad" (Mark Stanley)
//             WiFi, HTTPClient, WiFiClientSecure, Preferences are built into the ESP32 core
// =====================================================================
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>

// =====================================================================
// 1. SETTINGS YOU MAY CHANGE
// =====================================================================
// WiFi, backend URL and owner phone live in secrets.h (not on GitHub - copy secrets_example.h)
#include "secrets.h"
const char* METER_ID      = "NEHALE-01";
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* BACKEND_URL   = SECRET_BACKEND_URL;
const char  OWNER_PHONE[] = SECRET_OWNER_PHONE;     // alerts are sent here

#define GSM_ENABLED        1      // 0 = no SIM900 connected
#define START_FROM_ZERO    1      // 1 = every power-up starts at 0 kWh (testing)
                                  // 0 = balance is kept in flash through power cuts

// --- DEMO TIME ---
// A real 6 W load uses 3 kWh in ~21 days. DEMO_MODE speeds up time so you can see it work:
// 1 real second = DEMO_FACTOR simulated seconds. Energy and days are both simulated
// consistently, so Days Left still shows real-world days.
#define DEMO_MODE          1
const float DEMO_FACTOR  = 10000.0;   // fast test: 3 kWh at 6 W lasts ~30 s (10000 = ~3 min)

const float TOPUP_KWH          = 3.0;   // each token adds 3 kWh
const float TARIFF_NAD_PER_KWH = 2.0;   // NAD per kWh (set to the real tariff)
const float TARGET_DAYS        = 30.0;  // recommendation: credit to last this many days
const float LOW_BALANCE_KWH    = 0.5;   // low-balance alert level
const float OVERCURRENT_A      = 1;   // 2 x 3 W LEDs = 0.5 A normal
const int   OVERLOAD_COUNT     = 5;     // readings in a row above the limit before tripping

// Valid one-time tokens (each gives its own kWh amount). Used tokens are remembered.
struct Token { const char* code; float kwh; };
const Token TOKENS[] = {
  {"1245",  3.0},    // N$  6  (first top-up)
  {"2468",  1.0},    // N$  2
  {"3579",  2.0},    // N$  4
  {"4680",  5.0},    // N$ 10
  {"5791", 10.0},    // N$ 20
  {"6802",  0.5},    // N$  1
  {"7913", 20.0},    // N$ 40
  {"8024", 50.0},    // N$100
};
const int N_TOKENS = sizeof(TOKENS) / sizeof(TOKENS[0]);

// --- Usage model (from the Colab notebook, section 8) ---
// predicted kWh/day = MB0 + MB1*yesterday + MB2*2 days ago + MB3*3 days ago + MB4*mean of last 7 days
// Trained on the UCI household dataset (1089 days, test error 17.0 % vs 17.3 % for the 7-day average).
const float MB0 = 0.0, MB1 = 0.2890, MB2 = -0.1343, MB3 = 0.1360, MB4 = 0.6960;

// =====================================================================
// 2. PINS (as soldered on the prototype board)
// =====================================================================
#define RELAY_PIN    23   // board R7  -> R1 1k -> 2N2222 -> relay      HIGH = load ON
#define BUZZER_PIN   18   // board R11 -> buzzer +
#define VOLT_PIN     34   // board R9  -> R2 27k / R3 10k divider (after relay NO)
#define CURR_PIN     35   // board R15 -> R4 10k / R5 10k divider from ACS712 OUT
#define I2C_SDA      21
#define I2C_SCL      22
#define GSM_RX       16   // ESP32 RX2 <- SIM900 TXD
#define GSM_TX       17   // ESP32 TX2 -> SIM900 RXD

const float VOLT_DIVIDER     = (27000.0 + 10000.0) / 10000.0;   // 3.7
const float ACS_DIVIDER      = (10000.0 + 10000.0) / 10000.0;   // 2.0
const float ACS_SENS_V_PER_A = 0.185;                            // ACS712-05B
// Load detection with hysteresis: the load counts as ON once the smoothed current stays above
// LOAD_ON_A for LOAD_DEBOUNCE_MS, and as OFF once it stays below LOAD_OFF_A. One 3 W bulb ~0.12 A.
const float LOAD_ON_A        = 0.06;
const float LOAD_OFF_A       = 0.035;
const unsigned long LOAD_DEBOUNCE_MS = 300;
const float RELAY_SHIFT_DEFAULT_V = -0.009;                      // ACS712 shift caused by the relay coil (measured)
const float RELAY_SHIFT_MAX_V     = 0.05;                        // a learned shift beyond +/-50 mV is wrong -> reset
const unsigned long OVERLOAD_MS   = 1000;                        // over-current must last this long to trip
const float MIN_LOAD_V       = 3.0;                              // divider below this = voltage not sensed
const float LOAD_NOMINAL_V   = 12.0;                             // load supply voltage, used when the divider reads nothing

// Keypad (plugged in reversed): keypad pin 1->GPIO4, 2->13, 3->14, 4->27, 5->26, 6->25, 7->33, 8->32
// If keys come out wrong, run Keypad_Test.ino and correct this map.
const byte ROWS = 4, COLS = 4;
// Corrected for this keypad as plugged in (rows/columns crossed vs the standard layout)
char keys[ROWS][COLS] = {
  {'D','C','B','A'},
  {'#','9','6','3'},
  {'0','8','5','2'},
  {'*','7','4','1'}
};
byte rowPins[ROWS] = {4, 13, 14, 27};    // keypad plug is reversed: R1..R4
byte colPins[COLS] = {26, 25, 33, 32};   // C1..C4
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
// Keys:  0-9 token digits   # enter   * clear (or silence alarm)   A show info   B re-zero current sensor   C test SMS   D reset overload

// =====================================================================
// 3. STATE
// =====================================================================
LiquidCrystal_I2C* lcdPtr = nullptr;
#define lcd (*lcdPtr)
Preferences prefs;

float balanceKWh   = 0.0;
float totalKWh     = 0.0;      // energy used since the meter was first started
float volts = 0, amps = 0, watts = 0;
bool  voltsEstimated = false;  // true = GPIO34 read nothing, volts is LOAD_NOMINAL_V
float acsZero = 2.5;           // ACS712 OUT at 0 A with the relay OFF (tracked while off)
float acsRelayShift = RELAY_SHIFT_DEFAULT_V;  // extra zero shift while the relay is ON (learned, kept in flash)
float acsRelayShiftSaved = RELAY_SHIFT_DEFAULT_V;
unsigned long tShiftSave = 0;
float acsVolts = 0;            // last ACS712 OUT reading (V, after undoing the divider)
float ampsFilt = 0;            // smoothed current
bool  loadOn = false;          // debounced: something is drawing current
unsigned long tLoadEdge = 0;   // when the smoothed current first crossed the threshold
bool  acsOk = true;            // false = sensor signal missing/wrong -> overload trip disabled
unsigned long tRelayOn = 0;    // when the relay last switched on
unsigned long tRelayOff = 0;   // when the relay last switched off
unsigned long tAcsRetry = 0;   // last automatic re-zero attempt of a faulty current sensor
int   httpFails = 0;           // backend requests failed in a row
const float ACS_MAX_VALID_A = 5.5;            // ACS712-05B cannot read more; above = bad signal
const unsigned long RELAY_SETTLE_MS = 1500;   // ignore current for this long after switch-on
bool  overload = false;
unsigned long tOverStart = 0;  // when the current first exceeded OVERCURRENT_A
unsigned long tOverSms = 0;    // last over-current SMS
bool  lowAlertSent = false, zeroAlertSent = false;
bool  alarmOn = false, alarmMuted = false;   // continuous zero-balance alarm
unsigned long tAlarm = 0;
bool  alarmToneOn = false;
int   beepLeft = 0, beepMs = 100;   // non-blocking beeps (served in loop)
unsigned long tBeep = 0;
bool  beepToneOn = false;
unsigned long lcdHoldUntil = 0;     // a message stays on the LCD until then
uint32_t usedTokenMask = 0;
String tokenBuffer = "";

// time (simulated in DEMO_MODE)
double simSeconds = 0;         // simulated seconds since start
time_t bootEpoch = 1767225600; // calendar at start-up (NTP when WiFi works, else 2026-01-01)
unsigned long lastLoopMs = 0;

// daily history for prediction
const int HIST = 14;
float dayHist[HIST];           // newest at index 0
int   histCount = 0;
long  currentDay = 0;
float dayKWh = 0;              // energy used so far in the current day
float dayOnSeconds = 0;        // simulated seconds the relay was ON today
float powerAvgW = 0;           // smoothed power for the live estimate

float predKWhDay = 0, daysLeft = 0, recNAD = 0;

// timers (real ms)
unsigned long tPrint = 0, tSave = 0, tLcd = 0;
const unsigned long LOG_MS  = 1000UL;   // send a reading every second so the web page is live
const unsigned long POLL_MS = 3000UL;   // ask the web app for web top-ups

// ---- background tasks (core 0): WiFi/HTTP and GSM never block the meter loop ----
struct Snapshot {                 // values the network task uploads
  float bal, tot, volts, amps, watts, dayKWh, pred, daysLeft, recNAD;
  int relay; long simday; double simSec;
};
Snapshot snap;
portMUX_TYPE snapMux = portMUX_INITIALIZER_UNLOCKED;
float webTopupKWh = 0;            // web top-up received by the network task, applied by the loop
struct EventMsg { char name[16]; };
struct SmsOut   { char to[20]; char text[161]; };
struct SmsIn    { char from[24]; char text[161]; };
QueueHandle_t eventQ, smsOutQ, smsInQ;
volatile int  smsDoneCount = 0;   // bumped by the GSM task after each send attempt
volatile bool smsLastOk = false;
volatile bool gsmRegisteredFlag = false;
bool waitingTestSms = false; int testSmsStartCount = 0;

// =====================================================================
// 4. HELPERS
// =====================================================================
float pinVolts(int pin) { return analogReadMilliVolts(pin) / 1000.0; }

float readACSVolts(int n) {
  float s = 0;
  for (int i = 0; i < n; i++) s += pinVolts(CURR_PIN);
  return s / n * ACS_DIVIDER;
}

void relayOn()  { if (!digitalRead(RELAY_PIN)) tRelayOn = millis(); digitalWrite(RELAY_PIN, HIGH); }
// ACS712 on 5 V sits at ~2.5 V with no current. Far outside that = wiring fault on R4/R5/GPIO35.
bool acsZeroValid(float z) { return z > 1.8 && z < 3.2; }
void relayOff() { if (digitalRead(RELAY_PIN)) tRelayOff = millis(); digitalWrite(RELAY_PIN, LOW); }
bool loadAllowed() { return balanceKWh > 0.0005 && !overload; }

// Non-blocking: queues n beeps, played by serviceBuzzer() from the loop
void beep(int n, int ms = 100) { beepLeft = n; beepMs = ms; tBeep = 0; }

// Shows a message and keeps it on the LCD for holdMs (min 0.5 s) without pausing the meter
void lcdMsg(const String& a, const String& b, int holdMs = 0) {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(a.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(b.substring(0, 16));
  lcdHoldUntil = millis() + (holdMs > 500 ? holdMs : 500);
}

void checkAcsZero() {
  acsOk = acsZeroValid(acsZero);
  if (!acsOk) {
    Serial.print("[ACS712] FAULT: zero = "); Serial.print(acsZero, 3);
    Serial.println(" V (expected ~2.5 V). Check ACS OUT -> R4 -> R15 -> GPIO35 and R5 -> GND.");
    Serial.println("[ACS712] Overload trip DISABLED, load allowed, current shows 0 A.");
    lcdMsg("Current sensor", "FAULT-chk GPIO35", 2500);
  }
}

void saveState() {
  prefs.putFloat("bal", balanceKWh);
  prefs.putFloat("tot", totalKWh);
  prefs.putUInt("tok", usedTokenMask);
  prefs.putInt("hcnt", histCount);
  prefs.putBytes("hist", dayHist, sizeof(dayHist));
}

// Relay-coil shift of the current sensor: kept in its own namespace so START_FROM_ZERO keeps it
void loadCalib() {
  Preferences c; c.begin("calib", true);
  acsRelayShift = acsRelayShiftSaved = c.getFloat("rshift", RELAY_SHIFT_DEFAULT_V);
  c.end();
  if (fabs(acsRelayShift) > RELAY_SHIFT_MAX_V) {      // corrupt value (learned from bad readings)
    Serial.print("[ACS712] stored relay shift "); Serial.print(acsRelayShift * 1000, 1);
    Serial.println(" mV is invalid - reset to default");
    acsRelayShift = RELAY_SHIFT_DEFAULT_V; acsRelayShiftSaved = 99;   // forces a save
  }
}

void saveCalibIfChanged(unsigned long now) {
  if (fabs(acsRelayShift - acsRelayShiftSaved) < 0.002 || now - tShiftSave < 60000UL) return;
  Preferences c; c.begin("calib", false); c.putFloat("rshift", acsRelayShift); c.end();
  acsRelayShiftSaved = acsRelayShift; tShiftSave = now;
  Serial.print("[ACS712] relay shift saved: "); Serial.print(acsRelayShift * 1000, 1); Serial.println(" mV");
}

void loadState() {
  balanceKWh    = prefs.getFloat("bal", 0.0);
  totalKWh      = prefs.getFloat("tot", 0.0);
  usedTokenMask = prefs.getUInt("tok", 0);
  histCount     = prefs.getInt("hcnt", 0);
  if (prefs.getBytes("hist", dayHist, sizeof(dayHist)) != sizeof(dayHist)) { histCount = 0; }
}

// ---------------- GSM ----------------
// Reads the SIM900 reply until `expect` is seen or timeout. Everything is echoed to the
// Serial Monitor as  GSM> ...  so you can see what the module answers.
bool gsmWait(const char* expect, unsigned long timeoutMs) {
#if GSM_ENABLED
  String buf = "";
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (Serial2.available()) {
      char c = Serial2.read();
      buf += c;
      if (c == '\n') { buf.trim(); if (buf.length()) { Serial.print("  GSM> "); Serial.println(buf); } buf = ""; }
      if (buf.indexOf(expect) >= 0) return true;
      if (strcmp(expect, ">") != 0 && buf.indexOf("ERROR") >= 0) return false;
    }
    delay(5);
  }
  if (buf.length()) { Serial.print("  GSM> "); Serial.println(buf); }
  return false;
#else
  return false;
#endif
}

bool gsmCmd(const char* c, const char* expect = "OK", unsigned long timeoutMs = 1000) {
#if GSM_ENABLED
  while (Serial2.available()) Serial2.read();
  Serial.print("  GSM< "); Serial.println(c);
  Serial2.println(c);
  return gsmWait(expect, timeoutMs);
#else
  return false;
#endif
}

bool gsmReady = false;

// Checks the SIM900 and prints why SMS would fail
void gsmCheck() {
#if GSM_ENABLED
  Serial.println("[GSM] checking SIM900...");
  gsmReady = false;
  for (int i = 0; i < 5 && !gsmReady; i++) gsmReady = gsmCmd("AT", "OK", 800);   // also syncs the baud rate
  if (!gsmReady) {
    Serial.println("[GSM] NO REPLY from SIM900 -> check power (4 V / 5 V), PWR key, TX/RX crossed, common GND");
    return;
  }
  gsmCmd("ATE0");
  gsmCmd("AT+CMGF=1");
  gsmCmd("AT+CSCS=\"GSM\"");
  gsmCmd("AT+CNMI=2,1,0,0,0");                         // store new SMS on the SIM and announce +CMTI
  if (!gsmCmd("AT+CPIN?", "READY", 2000)) Serial.println("[GSM] SIM card not ready (inserted? PIN lock?)");
  gsmCmd("AT+CSQ", "OK", 1000);                         // +CSQ: 10..31 = OK signal, 99 = none
  if (!gsmCmd("AT+CREG?", "OK", 1000)) Serial.println("[GSM] no network reply");
  Serial.println("[GSM] look above: +CREG: 0,1 or 0,5 = registered (SMS will work)");
#endif
}

// AT+CREG? -> "+CREG: 0,1" (home) or "0,5" (roaming) = registered on the network
bool gsmRegistered() {
#if GSM_ENABLED
  while (Serial2.available()) Serial2.read();
  Serial2.println("AT+CREG?");
  String buf = ""; unsigned long t0 = millis();
  while (millis() - t0 < 1500) {
    while (Serial2.available()) buf += (char)Serial2.read();
    if (buf.indexOf("OK") >= 0 || buf.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  bool reg = buf.indexOf(",1") >= 0 || buf.indexOf(",5") >= 0;
  if (reg != gsmRegisteredFlag) {
    gsmRegisteredFlag = reg;
    Serial.println(reg ? "[GSM] registered on the network" : "[GSM] NOT registered (SIM / signal / power)");
  }
  return reg;
#else
  return false;
#endif
}

// Runs in the GSM task only. Waits up to 30 s for the SIM900 to answer and be registered.
bool sendSMSNow(const String& to, const String& text) {
  Serial.print("[SMS to "); Serial.print(to); Serial.print("] "); Serial.println(text);
#if GSM_ENABLED
  unsigned long t0 = millis();
  while (true) {
    if (!gsmCmd("AT", "OK", 800)) gsmCheck();                // module silent: re-initialise it
    else if (gsmRegistered()) break;
    if (millis() - t0 > 30000UL) {
      Serial.println(gsmReady ? "[SMS] FAILED: not registered on the network (SIM / signal / power)"
                              : "[SMS] FAILED: SIM900 not answering");
      return false;
    }
    delay(2000);
  }
  if (!gsmCmd("AT+CMGF=1", "OK", 1000)) { Serial.println("[SMS] FAILED: SIM900 not answering"); gsmReady = false; return false; }
  String cmd = String("AT+CMGS=\"") + to + "\"";
  if (!gsmCmd(cmd.c_str(), ">", 5000)) { Serial.println("[SMS] FAILED: no '>' prompt (no network / bad number?)"); return false; }
  Serial2.print(text);
  Serial2.write(26);                                    // Ctrl+Z = send
  bool ok = gsmWait("+CMGS", 15000);                    // network confirms with +CMGS: <id>
  Serial.println(ok ? "[SMS] SENT OK" : "[SMS] FAILED: no +CMGS (no credit / no signal?)");
  gsmWait("OK", 1000);
  return ok;
#else
  return false;
#endif
}

// Called from the meter loop: queues the SMS for the GSM task and returns at once
void sendSMS(const String& to, const String& text) {
  SmsOut m;
  strlcpy(m.to, to.c_str(), sizeof(m.to));
  strlcpy(m.text, text.c_str(), sizeof(m.text));
  if (xQueueSend(smsOutQ, &m, 0) != pdTRUE) Serial.println("[SMS] queue full - message dropped");
}

// ---------------- Backend (Google Apps Script) ----------------
bool wifiReady() { return strlen(BACKEND_URL) > 0 && WiFi.status() == WL_CONNECTED; }

String urlEncode(const String& s) {
  String o; char b[4];
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.') o += c;
    else { snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

String httpGet(const String& query) {
  if (!wifiReady()) return "";
  WiFiClientSecure sclient; sclient.setInsecure();
  WiFiClient pclient;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(4000);
  String url = String(BACKEND_URL) + "?" + query;
  String body = "";
  bool ok = url.startsWith("https") ? http.begin(sclient, url) : http.begin(pclient, url);
  if (ok) {
    int code = http.GET();
    if (code == 200) body = http.getString();
    else { Serial.print("[HTTP] error "); Serial.println(code); }
    http.end();
  }
  // WiFi can look connected while the link is dead (hotspot dropped us): rejoin after 3 failures
  if (body.length()) httpFails = 0;
  else if (++httpFails >= 3) {
    httpFails = 0;
    Serial.println("[WiFi] backend unreachable - rejoining WiFi");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  return body;
}

// Called from the meter loop: queues the event for the network task and returns at once
void logToBackend(const char* event) {
  EventMsg e; strlcpy(e.name, event, sizeof(e.name));
  xQueueSend(eventQ, &e, 0);                           // queue full (long WiFi outage) = drop
}

// Network task only: uploads one reading built from the latest snapshot
void sendLog(const char* event) {
  Snapshot s;
  portENTER_CRITICAL(&snapMux); s = snap; portEXIT_CRITICAL(&snapMux);
  String q = "action=log&meter=" + urlEncode(METER_ID) +
             "&event="   + urlEncode(event) +
             "&balance=" + String(s.bal, 4) +
             "&total="   + String(s.tot, 5) +
             "&volts="   + String(s.volts, 2) +
             "&amps="    + String(s.amps, 3) +
             "&watts="   + String(s.watts, 2) +
             "&relay="   + String(s.relay) +
             "&simday="  + String(s.simday) +
             "&daykwh="  + String(s.dayKWh, 5) +
             "&pred="    + String(s.pred, 5) +
             "&daysleft="+ String(s.daysLeft, 1) +
             "&recnad="  + String(s.recNAD, 2) +
             "&demo="    + String(DEMO_MODE);
  // meter calendar (start-up date + simulated time) for the web app's daily history
  time_t t = bootEpoch + (time_t)s.simSec;
  struct tm tmv; localtime_r(&t, &tmv);
  char dbuf[20]; strftime(dbuf, sizeof(dbuf), "%Y-%m-%d %H:%M", &tmv);
  q += "&date=" + urlEncode(dbuf);
  String r = httpGet(q);
  if (strcmp(event, "READING") != 0 || !r.length()) {   // only events and failures, to keep the log readable
    Serial.print("[LOG] "); Serial.print(event); Serial.println(r.length() ? " -> sent" : " -> not sent");
  }
}

// ---- Network task (core 0): readings every LOG_MS, events, web top-up polling, WiFi upkeep ----
void netTask(void*) {
  unsigned long tLogN = 0, tPollN = 0, tWifiN = 0;
  for (;;) {
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (now - tWifiN > 10000UL) { tWifiN = now; WiFi.reconnect(); }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    EventMsg e;
    while (xQueueReceive(eventQ, &e, 0) == pdTRUE) sendLog(e.name);
    if (now - tPollN >= POLL_MS) {
      tPollN = now;
      String r = httpGet("action=poll&meter=" + urlEncode(METER_ID));   // reply: {"kwh":3.0}
      int p = r.indexOf("\"kwh\":");
      if (p >= 0) {
        float kwh = r.substring(p + 6).toFloat();
        if (kwh > 0) { portENTER_CRITICAL(&snapMux); webTopupKWh += kwh; portEXIT_CRITICAL(&snapMux); }
      }
    }
    if (now - tLogN >= LOG_MS) { tLogN = now; sendLog("READING"); }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---- GSM task (core 0): sends queued SMS, reads incoming SMS, keeps the SIM900 checked ----
void pollGSM();
void gsmTask(void*) {
  gsmCheck();
  unsigned long tReg = 0;
  for (;;) {
    SmsOut m;
    if (xQueueReceive(smsOutQ, &m, 0) == pdTRUE) {
      bool ok = sendSMSNow(m.to, m.text);
      smsLastOk = ok; smsDoneCount++;
    }
    pollGSM();
    if (gsmReady && millis() - tReg > 60000UL) { tReg = millis(); gsmRegistered(); }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------- Top-up ----------------
void applyTopUp(float kwh, const String& source) {
  balanceKWh += kwh;
  overload = false; tOverStart = 0;
  lowAlertSent = false; zeroAlertSent = false;
  alarmOn = false; alarmMuted = false; alarmToneOn = false;
  noTone(BUZZER_PIN);
  saveState();
  beep(2);
  lcdMsg("TOP-UP OK +" + String(kwh, 2), "Bal " + String(balanceKWh, 3) + " kWh", 1500);
  Serial.print("[TOP-UP] +"); Serial.print(kwh, 3); Serial.print(" kWh via "); Serial.println(source);
  logToBackend(("TOPUP_" + source).c_str());
}

// returns kWh credited, 0 if invalid or already used
float redeemToken(const String& t) {
  for (int i = 0; i < N_TOKENS; i++) {
    if (t == TOKENS[i].code) {
      if (usedTokenMask & (1UL << i)) return -1;      // already used
      usedTokenMask |= (1UL << i);
      return TOKENS[i].kwh;
    }
  }
  return 0;
}

void tokenEntered(const String& t, const String& source, const String& replyTo) {
  float k = redeemToken(t);
  if (k > 0) {
    applyTopUp(k, source);
    if (replyTo.length()) sendSMS(replyTo, "Nehale meter: token accepted, +" + String(k, 1) + " kWh. Balance " + String(balanceKWh, 2) + " kWh.");
  } else {
    beep(3);
    lcdMsg(k < 0 ? "Token used" : "Invalid token", t, 1500);
    if (replyTo.length()) sendSMS(replyTo, String("Nehale meter: token ") + (k < 0 ? "already used." : "invalid."));
  }
}

// ---------------- SMS receive (top-up by GSM) ----------------
// New SMS are stored on the SIM. The SIM900 announces them with "+CMTI:"; the meter then
// reads every stored SMS (AT+CMGL), acts on it and deletes it. A scan also runs every
// SMS_SCAN_MS in case an announcement was missed while the meter was busy.
//
// SMS commands (not case sensitive):
//   TOKEN 2468   or just  2468   -> one-time token, +3 kWh
//   BAL                          -> reply with balance, days left, recommended top-up
//   ADD 3                        -> owner only (OWNER_PHONE): add 3 kWh directly
const unsigned long SMS_SCAN_MS = 20000UL;
bool smsPending = true;            // scan once at start-up (SMS that arrived while off)
unsigned long tSmsScan = 0;
String gsmLine = "";

bool sameNumber(const String& a, const String& b) {          // compare the last 9 digits
  String da = "", db = "";
  for (unsigned int i = 0; i < a.length(); i++) if (isDigit(a[i])) da += a[i];
  for (unsigned int i = 0; i < b.length(); i++) if (isDigit(b[i])) db += b[i];
  if (da.length() < 9 || db.length() < 9) return false;
  return da.substring(da.length() - 9) == db.substring(db.length() - 9);
}

void handleSMS(const String& sender, String body) {
  body.trim(); body.toUpperCase();
  Serial.print("[SMS from "); Serial.print(sender); Serial.print("] "); Serial.println(body);

  // Only act on real meter commands from a real phone number. Network/operator messages
  // (e.g. "Destination is barred", promos, delivery reports) are logged to Serial only:
  // no LCD message and no reply, so they can't start an SMS reply loop.
  int senderDigits = 0;
  for (unsigned int i = 0; i < sender.length(); i++) if (isDigit(sender[i])) senderDigits++;
  String cmd = body;
  if (cmd.startsWith("TOKEN")) { cmd = cmd.substring(5); cmd.trim(); }
  bool isToken = cmd.length() >= 4;
  for (unsigned int i = 0; i < cmd.length(); i++) if (!isDigit(cmd[i])) isToken = false;
  bool isCommand = body == "BAL" || body == "BALANCE" || body.startsWith("ADD") || isToken;
  if (senderDigits < 9 || !isCommand) {
    Serial.println("[SMS] ignored (not a meter command)");
    return;
  }

  lcdMsg("SMS received", body, 800);   // short notification only, not part of the rotation

  if (body == "BAL" || body == "BALANCE") {
    sendSMS(sender, "Nehale meter: balance " + String(balanceKWh, 2) + " kWh, about " +
            String(daysLeft, 1) + " days left. Recommended top-up N$" + String(recNAD, 0) + ".");
    return;
  }
  if (body.startsWith("ADD")) {
    float kwh = body.substring(3).toFloat();
    if (!sameNumber(sender, OWNER_PHONE)) { sendSMS(sender, "Nehale meter: ADD is only allowed from the owner's phone."); return; }
    if (kwh <= 0 || kwh > 50) { sendSMS(sender, "Nehale meter: send ADD 3 (kWh, 0-50)."); return; }
    applyTopUp(kwh, "SMS");
    sendSMS(sender, "Nehale meter: +" + String(kwh, 2) + " kWh added. Balance " + String(balanceKWh, 2) + " kWh.");
    return;
  }
  tokenEntered(cmd, "SMS", sender);      // only token commands reach here
}

// Reads all stored SMS, deletes them, then acts on them
void scanSMS() {
#if GSM_ENABLED
  const int MAXM = 5;
  String snd[MAXM], txt[MAXM]; int idx[MAXM]; int n = 0;
  while (Serial2.available()) Serial2.read();
  Serial2.println("AT+CMGL=\"ALL\"");
  String line = ""; int pendingIdx = -1; String pendingSnd = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 4000) {
    if (!Serial2.available()) { delay(5); continue; }
    char c = Serial2.read();
    if (c == '\r') continue;
    if (c != '\n') { line += c; continue; }
    line.trim();
    if (line.length() == 0) { continue; }
    if (line == "OK" || line.indexOf("ERROR") >= 0) break;
    if (line.startsWith("+CMGL:")) {                 // +CMGL: 3,"REC UNREAD","+26481...","","26/10/17,10:00:00+08"
      pendingIdx = line.substring(6).toInt();
      int q1 = line.indexOf('"'); q1 = line.indexOf('"', q1 + 1); q1 = line.indexOf('"', q1 + 1);
      int q2 = line.indexOf('"', q1 + 1);
      pendingSnd = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : String("");
    } else if (pendingIdx >= 0) {                    // the line after +CMGL is the message text
      if (n < MAXM) { idx[n] = pendingIdx; snd[n] = pendingSnd; txt[n] = line; n++; }
      pendingIdx = -1;
    }
    line = "";
    t0 = millis();                                   // keep reading while data flows
  }
  for (int i = 0; i < n; i++) {                      // delete first, so a message is never used twice
    String del = "AT+CMGD=" + String(idx[i]);
    gsmCmd(del.c_str(), "OK", 2000);
  }
  for (int i = 0; i < n; i++) {                      // hand over to the meter loop (it owns balance/LCD)
    SmsIn m;
    strlcpy(m.from, snd[i].c_str(), sizeof(m.from));
    strlcpy(m.text, txt[i].c_str(), sizeof(m.text));
    xQueueSend(smsInQ, &m, 0);
  }
  if (n == 0) Serial.println("[SMS] no new messages");
#endif
}

void pollGSM() {
#if GSM_ENABLED
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\r') continue;
    if (c != '\n') { gsmLine += c; continue; }
    String line = gsmLine; gsmLine = "";
    line.trim();
    if (line.length() == 0) continue;
    Serial.print("  GSM> "); Serial.println(line);
    if (line.startsWith("+CMTI:")) smsPending = true;   // +CMTI: "SM",3  -> new SMS stored
    if (line.startsWith("RING")) Serial2.println("ATH"); // reject calls
  }
  unsigned long now = millis();
  if (gsmReady && (smsPending || now - tSmsScan > SMS_SCAN_MS)) {
    smsPending = false; tSmsScan = now;
    scanSMS();
  }
  if (!gsmReady && now - tSmsScan > 60000UL) { tSmsScan = now; gsmCheck(); }   // retry a silent SIM900
#endif
}

// ---------------- Prediction ----------------
void pushDay(float kwh) {
  for (int i = HIST - 1; i > 0; i--) dayHist[i] = dayHist[i - 1];
  dayHist[0] = kwh;
  if (histCount < HIST) histCount++;
}

void updatePrediction() {
  if (histCount >= 7) {
    float m7 = 0; for (int i = 0; i < 7; i++) m7 += dayHist[i]; m7 /= 7.0;
    predKWhDay = MB0 + MB1 * dayHist[0] + MB2 * dayHist[1] + MB3 * dayHist[2] + MB4 * m7;
  } else if (histCount > 0) {
    float s = 0; for (int i = 0; i < histCount; i++) s += dayHist[i];
    predKWhDay = s / histCount;
  } else {
    predKWhDay = powerAvgW * 24.0 / 1000.0;            // no history yet: live estimate
  }
  if (predKWhDay < 0) predKWhDay = 0;
  daysLeft = (predKWhDay > 0.00001) ? balanceKWh / predKWhDay : 999;
  if (daysLeft > 999) daysLeft = 999;
  recNAD = (TARGET_DAYS * predKWhDay - balanceKWh) * TARIFF_NAD_PER_KWH;
  if (recNAD < 0) recNAD = 0;
}

// =====================================================================
// 5. SETUP
// =====================================================================
void updateSnapshot() {
  Snapshot s = { balanceKWh, totalKWh, volts, amps, watts, dayKWh, predKWhDay, daysLeft, recNAD,
                 (int)digitalRead(RELAY_PIN), currentDay, simSeconds };
  portENTER_CRITICAL(&snapMux); snap = s; portEXIT_CRITICAL(&snapMux);
}

void setup() {
  eventQ  = xQueueCreate(10, sizeof(EventMsg));
  smsOutQ = xQueueCreate(4, sizeof(SmsOut));
  smsInQ  = xQueueCreate(5, sizeof(SmsIn));
  pinMode(RELAY_PIN, OUTPUT); relayOff();              // load OFF until credit is confirmed
  pinMode(BUZZER_PIN, OUTPUT); noTone(BUZZER_PIN);
  Serial.begin(115200);
#if GSM_ENABLED
  Serial2.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
#endif
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // LCD (I2C address found automatically)
  Wire.begin(I2C_SDA, I2C_SCL);
  byte addr = 0x27;
  for (byte a = 1; a < 127; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { addr = a; break; } }
  lcdPtr = new LiquidCrystal_I2C(addr, 16, 2);
  lcd.init(); lcd.backlight();
  lcdMsg("Nehale Meter", "Starting...");

  // stored state
  prefs.begin("nehale", false);
  if (START_FROM_ZERO) { prefs.clear(); }
  loadState();
  if (START_FROM_ZERO) { balanceKWh = 0; totalKWh = 0; usedTokenMask = 0; histCount = 0; }
  loadCalib();

  // WiFi
  if (strlen(BACKEND_URL) > 0) {
    lcdMsg("Nehale Meter", "WiFi connecting");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(250);
    Serial.println(WiFi.status() == WL_CONNECTED ? "[WiFi] connected" : "[WiFi] not connected (will retry)");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WiFi] IP "); Serial.print(WiFi.localIP());
      Serial.print("  gateway "); Serial.println(WiFi.gatewayIP());
      configTime(2 * 3600, 0, "pool.ntp.org");        // Namibia time (UTC+2)
      struct tm tmNow;
      if (getLocalTime(&tmNow, 5000)) bootEpoch = mktime(&tmNow);
      Serial.println(getLocalTime(&tmNow, 0) ? "[TIME] date from NTP" : "[TIME] no NTP - using 2026-01-01");
    }
  }

  // ACS712 zero (relay is OFF, so no load current). Measured after WiFi is up because the
  // radio's supply current shifts the sensor output; it is also tracked while the relay is off.
  delay(500);
  acsZero = readACSVolts(400);
  checkAcsZero();

  Serial.println("==========================================");
  Serial.println("  NEHALE SMART PREPAID METER - READY");
  Serial.print  ("  Meter ID: "); Serial.println(METER_ID);
  Serial.print  ("  ACS712 zero: "); Serial.print(acsZero, 3); Serial.print(" V, relay shift ");
  Serial.print(acsRelayShift * 1000, 1); Serial.println(" mV");
  Serial.print  ("  Demo mode: "); Serial.println(DEMO_MODE ? "ON" : "OFF");
  Serial.print  ("  Balance: "); Serial.print(balanceKWh, 3); Serial.println(" kWh");
  Serial.println("  Top up: keypad token + #,  SMS 'TOKEN 1245',  or the web page");
  Serial.println("==========================================");
  Serial.println("LOGHEADER,esp_ms,balance_kwh,used_kwh,simday,volts,amps,watts,relay,pred_kwh_day,days_left,rec_nad");

  beep(1);
  lcd.clear();
  lastLoopMs = millis();
  updateSnapshot();
  logToBackend("BOOT");
  if (strlen(BACKEND_URL) > 0) xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
#if GSM_ENABLED
  xTaskCreatePinnedToCore(gsmTask, "gsm", 8192, nullptr, 1, nullptr, 0);
#endif
}

// =====================================================================
// 6. LOOP
// =====================================================================
void loop() {
  unsigned long now = millis();
  float dtReal = (now - lastLoopMs) / 1000.0;           // real seconds since last loop
  lastLoopMs = now;
  float dtSim = DEMO_MODE ? dtReal * DEMO_FACTOR : dtReal;

  // ---------- 1. MEASURE ----------
  volts = pinVolts(VOLT_PIN) * VOLT_DIVIDER;
  acsVolts = readACSVolts(400);
  // Sensor flagged faulty (usually: not powered yet when the ESP32 started) but it now gives
  // a sane signal -> re-zero it like the B key, so nobody has to press B after power-up.
  if (!acsOk && acsZeroValid(acsVolts) && now - tAcsRetry > 3000) {
    tAcsRetry = now;
    relayOff(); delay(400); acsZero = readACSVolts(400);   // relay comes back on below if allowed
    acsOk = acsZeroValid(acsZero);
    Serial.print("[ACS712] auto re-zero: "); Serial.print(acsZero, 3);
    Serial.println(acsOk ? " V - sensor OK" : " V - still faulty");
    acsVolts = acsZero;
  }
  bool relayIsOn = digitalRead(RELAY_PIN);
  bool settled = relayIsOn && now - tRelayOn > RELAY_SETTLE_MS;
  // zero = relay-off zero, plus the learned coil shift while the relay is on
  // Load current raises the ACS712 output on this board (measured). Only that direction counts:
  // a drop below the zero is supply drift (5 V rail dipping), never usage.
  float aSigned = (acsVolts - acsZero - (relayIsOn ? acsRelayShift : 0)) / ACS_SENS_V_PER_A;
  float aRaw = fabs(aSigned);                          // distance from zero (for validity / learning)
  bool  signalOk = acsOk && acsZeroValid(acsVolts) && aRaw <= ACS_MAX_VALID_A;
  float a = (signalOk && settled && aSigned > 0) ? aSigned : 0;   // bad signal / just switched / wrong way: 0
  // relay off = no current can flow, so the sensor output is its zero: follow its drift
  if (!relayIsOn && now - tRelayOff > 500 && acsZeroValid(acsVolts))
    acsZero = 0.99 * acsZero + 0.01 * acsVolts;
  // relay on and nothing switched on: learn the coil shift slowly (kept in flash), so the relay
  // itself is never counted as a load. Only from VALID readings near zero, and bounded.
  if (settled && signalOk && !loadOn && aRaw < LOAD_ON_A) {
    acsRelayShift = 0.995 * acsRelayShift + 0.005 * (acsVolts - acsZero);
    acsRelayShift = constrain(acsRelayShift, -RELAY_SHIFT_MAX_V, RELAY_SHIFT_MAX_V);
  }
  saveCalibIfChanged(now);

  // smoothing + hysteresis + debounce: noise can't flip the load on/off
  ampsFilt += (a - ampsFilt) * 0.3;
  bool want = loadOn ? ampsFilt > LOAD_OFF_A : ampsFilt > LOAD_ON_A;
  if (!relayIsOn) { want = false; ampsFilt = 0; }
  if (want != loadOn) {
    if (tLoadEdge == 0) tLoadEdge = now;
    else if (now - tLoadEdge >= LOAD_DEBOUNCE_MS || !relayIsOn) {
      loadOn = want; tLoadEdge = 0;
      Serial.print(loadOn ? "[LOAD] ON  " : "[LOAD] OFF "); Serial.print(ampsFilt, 3); Serial.println(" A");
    }
  } else tLoadEdge = 0;
  amps = loadOn ? ampsFilt : 0;
  // GPIO34 divider reads nothing but current flows: use the nominal supply voltage
  voltsEstimated = volts < MIN_LOAD_V && amps > 0;
  if (voltsEstimated) volts = LOAD_NOMINAL_V;
  watts = volts * amps;
  powerAvgW = 0.9 * powerAvgW + 0.1 * watts;

  // ---------- 2. DEDUCT ENERGY  E = P x t ----------
  if (digitalRead(RELAY_PIN)) {
    float usedKWh = watts * dtSim / 3600000.0;          // W x s -> kWh
    if (usedKWh > balanceKWh) usedKWh = balanceKWh;
    balanceKWh -= usedKWh;
    totalKWh   += usedKWh;
    dayKWh     += usedKWh;
    dayOnSeconds += dtSim;
  }

  // ---------- 3. DAY BOUNDARY (for the prediction history) ----------
  simSeconds += dtSim;
  long d = (long)(simSeconds / 86400.0);
  if (d != currentDay) {
    if (dayOnSeconds > 0.5 * 86400.0) pushDay(dayKWh);  // only days with power on most of the day
    Serial.print("[DAY "); Serial.print(currentDay); Serial.print("] used "); Serial.print(dayKWh, 4);
    Serial.println(" kWh");
    currentDay = d; dayKWh = 0; dayOnSeconds = 0;
  }
  updatePrediction();

  // ---------- 4. PROTECTION ----------
  if (!overload && digitalRead(RELAY_PIN) && now - tRelayOn > RELAY_SETTLE_MS) {
    // over-current must persist for OVERLOAD_MS (a spike or a single bad reading never trips)
    if (ampsFilt > OVERCURRENT_A) { if (tOverStart == 0) tOverStart = now; }
    else tOverStart = 0;
    if (tOverStart && now - tOverStart >= OVERLOAD_MS) {
      overload = true; relayOff(); tOverStart = 0;
      beep(5);
      Serial.print("[OVERLOAD] "); Serial.print(ampsFilt, 2); Serial.print(" A (ACS ");
      Serial.print(acsVolts, 3); Serial.println(" V)");
      lcdMsg("OVERLOAD " + String(ampsFilt, 2) + "A", "Fix, then press D", 1500);
      if (tOverSms == 0 || now - tOverSms > 300000UL) {   // at most one SMS per 5 minutes
        tOverSms = now;
        sendSMS(OWNER_PHONE, "Nehale meter FAULT: over-current " + String(ampsFilt, 2) + " A. Load disconnected.");
      }
      logToBackend("OVERLOAD");
    }
  }

  if (balanceKWh <= 0.0005) {
    balanceKWh = 0;
    if (!zeroAlertSent) {
      zeroAlertSent = true;
      relayOff();
      if (totalKWh > 0) {                               // not on the very first start-up
        Serial.println("[ALERT] ZERO balance - load OFF, alarm ringing (press * to silence)");
        alarmOn = true; alarmMuted = false;
        sendSMS(OWNER_PHONE, "Nehale meter: balance 0 kWh. Power disconnected. Top up to restore.");
        logToBackend("CUTOFF");
      }
      saveState();
    }
  } else if (balanceKWh < LOW_BALANCE_KWH && !lowAlertSent) {
    lowAlertSent = true;
    Serial.println("[ALERT] LOW balance");
    beep(2, 300);
    sendSMS(OWNER_PHONE, "Nehale meter: LOW balance " + String(balanceKWh, 2) + " kWh (~" + String(daysLeft, 1) +
            " days). Recommended top-up N$" + String(recNAD, 0) + ".");
    logToBackend("LOW");
  }

  if (loadAllowed()) relayOn(); else relayOff();       // GPIO19 = 0 V whenever no credit

  // ---------- 4b. CONTINUOUS ALARM (zero balance or overload) ----------
  // 0.5 s ON / 0.5 s OFF until a top-up (or D after an overload). Press * to silence.
  bool ringing = (alarmOn || overload) && !alarmMuted;
  if (ringing) {
    if (now - tAlarm >= 500) {
      tAlarm = now;
      alarmToneOn = !alarmToneOn;
      if (alarmToneOn) tone(BUZZER_PIN, 2000); else noTone(BUZZER_PIN);
    }
  } else if (alarmToneOn) { noTone(BUZZER_PIN); alarmToneOn = false; }
  // queued beeps (non-blocking); the alarm has priority over them
  if (beepLeft > 0 && !ringing && now - tBeep >= (unsigned long)beepMs) {
    tBeep = now;
    if (!beepToneOn) { tone(BUZZER_PIN, 2000); beepToneOn = true; }
    else { noTone(BUZZER_PIN); beepToneOn = false; beepLeft--; }
  }

  // ---------- 5. KEYPAD ----------
  char k = keypad.getKey();
  if (k) {
    Serial.print("Key: "); Serial.println(k);
    beep(1, 40);
    if (k >= '0' && k <= '9') {
      if (tokenBuffer.length() < 8) tokenBuffer += k;
      lcdMsg("Token entry:", tokenBuffer);
    } else if (k == '*') {
      if (tokenBuffer.length() == 0 && alarmOn && !alarmMuted) {
        alarmMuted = true; noTone(BUZZER_PIN); alarmToneOn = false;
        lcdMsg("Alarm silenced", "Top up to restore", 800);
      } else { tokenBuffer = ""; lcdMsg("Cleared", "", 500); }
    } else if (k == '#') {
      if (tokenBuffer.length()) { lcdMsg("Checking...", tokenBuffer, 500); tokenEntered(tokenBuffer, "KEYPAD", ""); }
      tokenBuffer = "";
    } else if (k == 'A') {
      lcdMsg("Pred " + String(predKWhDay, 3) + "kWh/d", "Hist days: " + String(histCount), 2500);
    } else if (k == 'B') {
      relayOff(); delay(400); acsZero = readACSVolts(400);
      lcdMsg("Sensor re-zeroed", String(acsZero, 3) + " V", 1000);
      checkAcsZero();
    } else if (k == 'C') {
      testSmsStartCount = smsDoneCount; waitingTestSms = true;
      sendSMS(OWNER_PHONE, "Nehale meter: test SMS OK. Balance " + String(balanceKWh, 2) + " kWh.");
      lcdMsg("Sending SMS...", OWNER_PHONE, 60000);   // replaced by the result below
    } else if (k == 'D') {
      overload = false; tOverStart = 0; alarmMuted = false; lcdMsg("Overload reset", "", 800);
    }
  }

  // ---------- 6. SMS + WEB TOP-UP (received by the background tasks) ----------
  SmsIn in;
  while (xQueueReceive(smsInQ, &in, 0) == pdTRUE) handleSMS(in.from, in.text);
  float webKWh;
  portENTER_CRITICAL(&snapMux); webKWh = webTopupKWh; webTopupKWh = 0; portEXIT_CRITICAL(&snapMux);
  if (webKWh > 0) applyTopUp(webKWh, "WEB");
  if (waitingTestSms && smsDoneCount != testSmsStartCount) {
    waitingTestSms = false;
    lcdMsg(smsLastOk ? "SMS sent" : "SMS FAILED", smsLastOk ? "" : "see Serial Mon.", 2000);
  }
  updateSnapshot();

  // ---------- 7. SAVE ----------
  if (!START_FROM_ZERO && now - tSave > 30000) { tSave = now; saveState(); }

  // ---------- 8. SERIAL (every 1 s) + CSV log line ----------
  if (now - tPrint >= 1000) {
    tPrint = now;
    Serial.print(overload ? "[OVL] " : (balanceKWh <= 0 ? "[NO CREDIT] " : "[ON] "));
    Serial.print("V "); Serial.print(volts, 2); if (voltsEstimated) Serial.print(" (est)");
    Serial.print(" | I "); Serial.print(amps, 3);
    Serial.print(" A (ACS "); Serial.print(acsVolts, 3); Serial.print("V zero "); Serial.print(acsZero + (digitalRead(RELAY_PIN) ? acsRelayShift : 0), 3);
    Serial.print("V, filt "); Serial.print(ampsFilt, 3); Serial.print(" A, shift ");
    Serial.print(acsRelayShift * 1000, 1); Serial.print(" m");
    Serial.print(acsOk ? "V)" : "V FAULT)");
    Serial.print(" | P "); Serial.print(watts, 2);
    Serial.print(" W | Bal "); Serial.print(balanceKWh, 4);
    Serial.print(" kWh | Day "); Serial.print(currentDay);
    Serial.print(" | Pred "); Serial.print(predKWhDay, 4);
    Serial.print(" kWh/d | Left "); Serial.print(daysLeft, 1);
    Serial.print(" d | Rec N$"); Serial.print(recNAD, 2);
    Serial.print(" | GPIO19 "); Serial.println(digitalRead(RELAY_PIN) ? "HIGH" : "LOW");
    Serial.print("LOG,"); Serial.print(now); Serial.print(",");
    Serial.print(balanceKWh, 5); Serial.print(","); Serial.print(totalKWh, 5); Serial.print(",");
    Serial.print(currentDay); Serial.print(","); Serial.print(volts, 2); Serial.print(",");
    Serial.print(amps, 3); Serial.print(","); Serial.print(watts, 2); Serial.print(",");
    Serial.print(digitalRead(RELAY_PIN)); Serial.print(","); Serial.print(predKWhDay, 5); Serial.print(",");
    Serial.print(daysLeft, 1); Serial.print(","); Serial.println(recNAD, 2);
  }

  // ---------- 9. LCD (refresh every 0.5 s, 2nd line rotates every 3 s) ----------
  if (now - tLcd > 500 && (long)(now - lcdHoldUntil) >= 0 && tokenBuffer.length() == 0) {
    tLcd = now;
    lcd.setCursor(0, 0);
    lcd.print("Bal:"); lcd.print(balanceKWh, 3); lcd.print(" kWh    ");
    lcd.setCursor(0, 1);
    if (overload)              lcd.print("OVERLOAD-press D ");
    else if (balanceKWh <= 0)  lcd.print("NO CREDIT-TopUp ");
    else {
      int s = (now / 3000) % 4;
      if (s == 0)      { lcd.print("P:"); lcd.print(watts, 1); lcd.print("W I:"); lcd.print(amps, 2); lcd.print("A  "); }
      else if (s == 1) { lcd.print("Days left:"); lcd.print(daysLeft, 1); lcd.print("     "); }
      else if (s == 2) { lcd.print("Rec: N$"); lcd.print(recNAD, 0); lcd.print("        "); }
      else             { lcd.print("V:"); lcd.print(volts, 1); lcd.print(wifiReady() ? " WiFi OK " : " no WiFi "); }
    }
  }

  delay(5);
}
