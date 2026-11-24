// =====================================================================
//  Workout tracker with RFID login – Arduino UNO Q (STM32 MCU side)
//
//  Wiring (matches pin chart):
//    D2..D7   LCD RS, E, DB4, DB5, DB6, DB7   (LCD VDD + backlight on 5V)
//    D9       RC522 RST
//    D10      RC522 SDA (SS)
//    D11..13  RC522 MOSI / MISO / SCK          (RC522 VCC on 3.3V only!)
//    A0       Rep potentiometer wiper          (pot ends on 3.3V and GND)
//    A2       Button 1  OK / NEXT              (other leg to GND)
//    A1       Button 2  BACK                   (other leg to GND)
//    A4 / A5  OLED 2 SDA / SCL  (progress)     (software I2C)
//    SDA/SCL header next to AREF: OLED 1 (target reps, hardware I2C)
//
//  Libraries (Library Manager): U8g2, MFRC522 (by GithubCommunity),
//  LiquidCrystal, Arduino_RouterBridge (for Monitor output on UNO Q).
//
//  How it works:
//    1. "Scan your card" – tap an RFID card/tag. Known cards load that
//       person's targets. Unknown cards show their UID (copy it into USERS).
//       Press B1 on the scan screen to start as a guest.
//    2. For each exercise, turn the knob to set reps done.
//       B1 = next exercise, B2 = previous exercise.
//    3. Review screens, then Confirm (B1 = save, B2 = back).
//    4. Done – summary is printed to the Monitor. Scan a card or press B1
//       to start again.
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
// Set USE_OLED to 0 to build without the two OLED screens (and without the U8g2 library).
#define USE_OLED 1
#if USE_OLED
#include <U8g2lib.h>
#endif
#include <LiquidCrystal.h>
#include "MFRC522.h"   // bundled in the sketch folder (App Lab picker does not offer it)
#include <Arduino_RouterBridge.h>   // UNO Q: Monitor = USB / App Lab console

// ---------- Pins ----------
const uint8_t PIN_POT   = A0;
const uint8_t PIN_B1    = A2;   // OK / next  (swapped to match the physical buttons)
const uint8_t PIN_B2    = A1;   // back
const uint8_t PIN_RC_SS = 10;
const uint8_t PIN_RC_RST = 9;

// ---------- Screens & RFID ----------
LiquidCrystal lcd(2, 3, 4, 5, 6, 7);
#if USE_OLED
U8G2_SSD1306_128X64_NONAME_1_HW_I2C oled1(U8G2_R0, U8X8_PIN_NONE);                 // target reps (header SDA/SCL)
U8G2_SSD1306_128X64_NONAME_1_SW_I2C oled2(U8G2_R0, /*clock=*/A5, /*data=*/A4, U8X8_PIN_NONE); // progress (A4/A5)
#endif
MFRC522 rfid(PIN_RC_SS, PIN_RC_RST);

// ---------- Exercises (edit these) ----------
// The normal plan comes from the website: after a card tap, python/main.py fetches the
// patient's plan from the server and sends it here (load_plan). The DEFAULT_* lists below
// are only used for guests and as an offline fallback (USERS[]) if the server doesn't answer.
const int MAX_EX = 8;                 // most exercises a plan can have
const int DEFAULT_EX = 4;
const char* defaultName[DEFAULT_EX] = {"Chest press", "Squat", "Row", "Shoulder press"};
const int defaultTarget[DEFAULT_EX] = {10, 12, 10, 8};
const int MAX_REPS = 12;        // knob range is 0..MAX_REPS over the full turn
const unsigned long PLAN_WAIT_MS = 4000;   // how long to wait for the server before falling back

// ---------- Offline fallback users ----------
// Only used if the server can't be reached. Cards normally live in the website.
struct User {
  uint8_t uidLen;               // 4 for most cards/fobs, 7 for some tags
  uint8_t uid[7];
  const char* name;             // max 13 chars so "Hi <name>" fits the LCD
  int target[DEFAULT_EX];
};

const User USERS[] = {
  { 4, {0xDE, 0xAD, 0xBE, 0xEF}, "Andre", {10, 12, 10, 8} },
  { 4, {0x12, 0x34, 0x56, 0x78}, "Guest2", {8, 10, 8, 6} },
};
const int NUM_USERS = sizeof(USERS) / sizeof(USERS[0]);

// ---------- Session data ----------
int numEx = DEFAULT_EX;
char exName[MAX_EX][17];        // max 16 chars
char userName[16] = "Guest";
int exTarget[MAX_EX];
int repsDone[MAX_EX];
char lastUid[22] = "";          // "DE AD BE EF" style text
int pendingUser = -1;           // offline fallback match for the card being loaded
bool noPlan = false;            // card is registered but the patient has no exercises yet

// ---------- Buttons ----------
const unsigned long DEBOUNCE_MS = 30;
struct Btn { uint8_t pin; bool stable; bool lastRead; unsigned long t; };
Btn b1 = {PIN_B1, HIGH, HIGH, 0};
Btn b2 = {PIN_B2, HIGH, HIGH, 0};

// Returns true once per press (debounced, INPUT_PULLUP so pressed = LOW)
bool pressed(Btn &b) {
  bool r = digitalRead(b.pin);
  unsigned long now = millis();
  if (r != b.lastRead) { b.lastRead = r; b.t = now; }
  if (now - b.t > DEBOUNCE_MS && r != b.stable) {
    b.stable = r;
    if (r == LOW) return true;
  }
  return false;
}

// ---------- State ----------
enum State { S_SCAN, S_LOADING, S_UNKNOWN, S_EXERCISE, S_REVIEW, S_CONFIRM, S_DONE };
State state = S_SCAN;
int idx = 0;
bool dirty = true;
unsigned long unknownSince = 0;
unsigned long loadingSince = 0;
const unsigned long UNKNOWN_SHOW_MS = 4000;

// ---------- Small helpers ----------
int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------- Potentiometer ----------
// "Pick-up" behaviour: when you arrive on an exercise, the knob does nothing
// until you actually turn it, so the old value isn't overwritten by
// wherever the knob happened to be.
const int ADC_MAX = 1023;       // 10-bit (set in setup)
const int PICKUP_MARGIN = 40;   // how far the knob must move to "grab" the value
int potBase = 0;
bool potLive = false;
unsigned long lastPotRead = 0;

int readPot() {
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_POT);
  return (int)(sum / 8);
}

// Raw reading -> reps, with hysteresis so the number doesn't flicker
int potToReps(int raw, int current) {
  const float step = (float)(ADC_MAX + 1) / (MAX_REPS + 1);
  float centre = (current + 0.5f) * step;
  if (fabs(raw - centre) < step * 0.65f) return current;   // stay put
  return clampInt((int)(raw / step), 0, MAX_REPS);
}

void armPot() {
  potBase = readPot();
  potLive = false;
}

// ---------- Session control ----------
// Start with the built-in 4 exercises (guests / offline fallback)
void startSession(const char* name, const int* targets) {
  strncpy(userName, name, sizeof(userName) - 1);
  userName[sizeof(userName) - 1] = '\0';
  numEx = DEFAULT_EX;
  for (int i = 0; i < numEx; i++) {
    strncpy(exName[i], defaultName[i], 16); exName[i][16] = '\0';
    exTarget[i] = targets[i];
    repsDone[i] = 0;
  }
  idx = 0;
  state = S_EXERCISE;
  armPot();
  Monitor.print("Session started for ");
  Monitor.println(name);
}

// Called from the Linux side (python/main.py) after a card tap.
//   "UNKNOWN"                              card isn't registered on the website
//   "OK;name;ex1|reps1;ex2|reps2;..."      the patient's plan (up to MAX_EX exercises)
// Runs via provide_safe, i.e. from loop() context, so it can touch the session state.
String loadPlan(String msg) {
  if (state != S_LOADING) return "ignored";       // answer arrived late (fallback already ran)
  if (msg.startsWith("NOPLAN;")) {                 // registered card, but no exercises assigned yet
    strncpy(userName, msg.c_str() + 7, sizeof(userName) - 1);
    userName[sizeof(userName) - 1] = '\0';
    noPlan = true;
    state = S_UNKNOWN; unknownSince = millis(); dirty = true;
    return "ok";
  }
  if (msg.startsWith("UNKNOWN")) {
    state = S_UNKNOWN; unknownSince = millis(); dirty = true;
    return "ok";
  }
  // split on ';'
  int p = msg.indexOf(';');
  if (!msg.startsWith("OK;") || p < 0) { state = S_UNKNOWN; unknownSince = millis(); dirty = true; return "bad"; }
  int start = p + 1;
  int q = msg.indexOf(';', start);
  String name = (q < 0) ? msg.substring(start) : msg.substring(start, q);
  int n = 0;
  while (q >= 0 && n < MAX_EX) {
    start = q + 1;
    q = msg.indexOf(';', start);
    String item = (q < 0) ? msg.substring(start) : msg.substring(start, q);
    int bar = item.indexOf('|');
    if (bar <= 0) continue;
    String ex = item.substring(0, bar);
    strncpy(exName[n], ex.c_str(), 16); exName[n][16] = '\0';
    exTarget[n] = clampInt(item.substring(bar + 1).toInt(), 0, MAX_REPS);
    repsDone[n] = 0;
    n++;
  }
  if (n == 0) { state = S_UNKNOWN; unknownSince = millis(); dirty = true; return "empty"; }
  strncpy(userName, name.c_str(), sizeof(userName) - 1);
  userName[sizeof(userName) - 1] = '\0';
  numEx = n;
  idx = 0;
  state = S_EXERCISE;
  armPot();
  dirty = true;
  return "ok";
}

void goToScan() {
  state = S_SCAN;
  idx = 0;
  initReader();               // re-init reader (helps if it got stuck)
}

// ---------- Navigation ----------
void goNext() {
  switch (state) {
    case S_EXERCISE:
      if (idx < numEx - 1) {
        idx++;
        if (repsDone[idx] == 0) {
          // New exercise: carry over the previous number and keep the knob
          // live, so the value doesn't drop to 0 when you press next.
          repsDone[idx] = repsDone[idx - 1];
          potBase = readPot();
          potLive = true;
        } else {
          armPot();            // already has a value: protect it until the knob is turned
        }
        return;
      }
      state = S_REVIEW; idx = 0;
      break;
    case S_REVIEW:
      if (idx < numEx - 1) idx++;
      else state = S_CONFIRM;
      break;
    case S_CONFIRM:
      state = S_DONE;
      printSummary();
      break;
    default: break;
  }
}

void goPrev() {
  switch (state) {
    case S_EXERCISE:
      if (idx > 0) idx--;
      break;
    case S_REVIEW:
      if (idx > 0) idx--;
      else { state = S_EXERCISE; idx = numEx - 1; }
      break;
    case S_CONFIRM:
      state = S_REVIEW;
      idx = numEx - 1;
      break;
    default: break;
  }
  if (state == S_EXERCISE) armPot();
}

// ---------- RFID ----------
// Only the card's ID (UID) is read – no data blocks – so a read is ~10 ms.
unsigned long lastReaderInit = 0;
const unsigned long READER_REINIT_MS = 3000;   // cheap RC522 clones can "doze off"

void initReader() {
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);    // strongest antenna = longer range
  lastReaderInit = millis();
}

void uidToText(const MFRC522::Uid &u, char* out) {
  const char hex[] = "0123456789ABCDEF";
  int p = 0;
  for (byte i = 0; i < u.size && i < 7; i++) {
    if (i) out[p++] = ' ';
    out[p++] = hex[u.uidByte[i] >> 4];
    out[p++] = hex[u.uidByte[i] & 0x0F];
  }
  out[p] = '\0';
}

int findUser(const MFRC522::Uid &u) {
  for (int k = 0; k < NUM_USERS; k++) {
    if (USERS[k].uidLen != u.size) continue;
    bool match = true;
    for (byte i = 0; i < u.size; i++) {
      if (USERS[k].uid[i] != u.uidByte[i]) { match = false; break; }
    }
    if (match) return k;
  }
  return -1;
}

// Returns true if a card was read and handled
bool checkCard() {
  // Re-wake the reader every few seconds while waiting for a card
  if (millis() - lastReaderInit > READER_REINIT_MS) initReader();
  if (!rfid.PICC_IsNewCardPresent()) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;

  uidToText(rfid.uid, lastUid);
  Monitor.print("Card UID: ");
  Monitor.println(lastUid);

  // Ask the Linux side to fetch this patient's plan from the server; it answers by calling
  // load_plan. If nothing arrives in PLAN_WAIT_MS, loop() falls back to USERS[].
  pendingUser = findUser(rfid.uid);
  noPlan = false;
  state = S_LOADING;
  loadingSince = millis();
  Bridge.notify("card_tap", String(lastUid));

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

// ---------- Drawing ----------
int targetsMet() {
  int n = 0;
  for (int i = 0; i < numEx; i++) if (repsDone[i] >= exTarget[i]) n++;
  return n;
}

void drawLCD() {
  lcd.clear();
  lcd.setCursor(0, 0);
  switch (state) {
    case S_SCAN:
      lcd.print("Scan your card");
      lcd.setCursor(0, 1);
      lcd.print("B1 = guest");
      break;
    case S_LOADING:
      lcd.print("Loading plan...");
      break;
    case S_UNKNOWN:
      lcd.print(noPlan ? "No plan yet for" : "Unknown card");
      lcd.setCursor(0, 1);
      lcd.print(noPlan ? userName : lastUid);
      break;
    case S_EXERCISE:
      lcd.print(exName[idx]);
      lcd.setCursor(0, 1);
      lcd.print("Ex ");
      lcd.print(idx + 1);
      lcd.print("/");
      lcd.print(numEx);
      lcd.print(potLive ? " Knob=reps" : " Turn knob");   // fits 16 chars
      break;
    case S_REVIEW:
      lcd.print(exName[idx]);
      lcd.setCursor(0, 1);
      lcd.print("Done:");
      lcd.print(repsDone[idx]);
      lcd.print(" Goal:");
      lcd.print(exTarget[idx]);
      break;
    case S_CONFIRM:
      lcd.print("Confirm entries?");
      lcd.setCursor(0, 1);
      lcd.print("B1=OK  B2=Back");
      break;
    case S_DONE:
      lcd.print("Done, ");
      lcd.print(userName);
      lcd.setCursor(0, 1);
      lcd.print("Scan/B1 = new");
      break;
  }
}

// OLED 1 is only drawn if it answered on the I2C bus at startup,
// so a missing/miswired screen can't freeze the whole sketch.
#if USE_OLED
bool oled1Ok = false;

bool i2cFound(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void drawOLED1() {
  if (!oled1Ok) return;
  oled1.firstPage();
  do {
    oled1.setFont(u8g2_font_7x13B_tr);
    switch (state) {
      case S_SCAN:
        oled1.drawStr(0, 14, "WELCOME");
        oled1.setFont(u8g2_font_6x10_tr);
        oled1.drawStr(0, 36, "Tap your card on");
        oled1.drawStr(0, 48, "the reader");
        break;
      case S_LOADING:
        oled1.drawStr(0, 30, "LOADING...");
        break;
      case S_UNKNOWN:
        oled1.drawStr(0, 14, "UNKNOWN CARD");
        oled1.setFont(u8g2_font_6x10_tr);
        oled1.drawStr(0, 36, "Not registered.");
        oled1.drawStr(0, 48, "UID on main screen");
        break;
      case S_EXERCISE:
      case S_REVIEW:
        oled1.drawStr(0, 14, "TARGET REPS");
        oled1.setFont(u8g2_font_logisoso32_tn);
        oled1.setCursor(0, 58);
        oled1.print(exTarget[idx]);
        break;
      case S_CONFIRM:
        oled1.drawStr(0, 14, "REVIEW DONE");
        oled1.setFont(u8g2_font_6x10_tr);
        oled1.drawStr(0, 36, "Check the main");
        oled1.drawStr(0, 48, "screen to confirm");
        break;
      case S_DONE:
        oled1.drawStr(0, 30, "SAVED");
        oled1.setFont(u8g2_font_6x10_tr);
        oled1.drawStr(0, 50, userName);
        break;
    }
  } while (oled1.nextPage());
}

void drawOLED2() {
  oled2.firstPage();
  do {
    oled2.setFont(u8g2_font_7x13B_tr);
    switch (state) {
      case S_SCAN:
        oled2.drawStr(0, 30, "READY");
        break;
      case S_LOADING:
        oled2.drawStr(0, 30, "PLEASE WAIT");
        break;
      case S_UNKNOWN:
        oled2.drawStr(0, 14, "CARD UID");
        oled2.setFont(u8g2_font_6x10_tr);
        oled2.drawStr(0, 36, lastUid);
        break;
      case S_EXERCISE:
      case S_REVIEW: {
        oled2.drawStr(0, 14, "REPS DONE");
        oled2.setFont(u8g2_font_logisoso32_tn);
        oled2.setCursor(0, 52);
        oled2.print(repsDone[idx]);
        // progress bar
        oled2.drawFrame(66, 20, 60, 12);
        int tgt = exTarget[idx] > 0 ? exTarget[idx] : 1;
        int w = (int)((long)clampInt(repsDone[idx], 0, tgt) * 56 / tgt);
        if (w > 0) oled2.drawBox(68, 22, w, 8);
        oled2.setFont(u8g2_font_6x10_tr);
        oled2.setCursor(66, 46);
        oled2.print("Ex ");
        oled2.print(idx + 1);
        oled2.print("/");
        oled2.print(numEx);
        oled2.setCursor(66, 60);
        oled2.print(repsDone[idx] >= exTarget[idx] ? "Goal met" : "Keep going");
        break;
      }
      case S_CONFIRM:
      case S_DONE:
        oled2.drawStr(0, 14, state == S_DONE ? "COMPLETE" : "SUMMARY");
        oled2.setFont(u8g2_font_6x10_tr);
        oled2.setCursor(0, 36);
        oled2.print("Goals met: ");
        oled2.print(targetsMet());
        oled2.print("/");
        oled2.print(numEx);
        oled2.setCursor(0, 50);
        oled2.print("User: ");
        oled2.print(userName);
        break;
    }
  } while (oled2.nextPage());
}
#else
void drawOLED1() {}
void drawOLED2() {}
#endif

void redrawAll() {
  drawLCD();
  drawOLED1();
  drawOLED2();
}

void printSummary() {
  Monitor.print("--- Workout summary: ");
  Monitor.print(userName);
  Monitor.println(" ---");
  for (int i = 0; i < numEx; i++) {
    Monitor.print(exName[i]);
    Monitor.print(": ");
    Monitor.print(repsDone[i]);
    Monitor.print("/");
    Monitor.println(exTarget[i]);
  }
  Monitor.print("Goals met: ");
  Monitor.print(targetsMet());
  Monitor.print("/");
  Monitor.println(numEx);

  // Send to the Linux side (python/main.py), which POSTs it to the server.
  // One string: "UID;name;reps1,reps2,...;target1,target2,..."  (guests have an empty UID)
  String msg = String(lastUid) + ";" + userName + ";";
  for (int i = 0; i < numEx; i++) { if (i) msg += ","; msg += repsDone[i]; }
  msg += ";";
  for (int i = 0; i < numEx; i++) { if (i) msg += ","; msg += exTarget[i]; }
  Bridge.notify("save_workout", msg);
}

// ---------- Setup ----------
void setup() {
  pinMode(PIN_B1, INPUT_PULLUP);
  pinMode(PIN_B2, INPUT_PULLUP);
  analogReadResolution(10);     // 0..1023, pot powered from 3.3V

  // Show progress on the LCD so a hang during startup is visible
  lcd.begin(16, 2);
  lcd.print("Booting...");

  bool bridgeOk = Bridge.begin();
  lcd.setCursor(0, 1);
  lcd.print(bridgeOk ? "Bridge ok" : "Bridge FAILED");
  Monitor.begin();

#if USE_OLED
  Wire.begin();
  if (i2cFound(0x3C)) {
    oled1Ok = true;
  } else if (i2cFound(0x3D)) {
    oled1.setI2CAddress(0x3D * 2);
    oled1Ok = true;
  }
  if (oled1Ok) oled1.begin();
  else {
    Monitor.println("OLED 1 not found on header SDA/SCL - check wiring.");
    lcd.print("OLED1 not found");
    delay(2000);
  }
  oled2.begin();
#endif

  SPI.begin();
  initReader();
  delay(50);

  for (int i = 0; i < DEFAULT_EX; i++) {
    strncpy(exName[i], defaultName[i], 16); exName[i][16] = '\0';
    exTarget[i] = defaultTarget[i]; repsDone[i] = 0;
  }
  bool bound = Bridge.provide_safe("load_plan", loadPlan);
  if (!bound) {
    Monitor.println("Could not register load_plan - server plans will not load.");
    lcd.clear();
    lcd.print("load_plan failed");
    delay(2000);
  }

  Monitor.println("Workout tracker ready. Scan a card.");
  redrawAll();
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();
  bool p1 = pressed(b1);
  bool p2 = pressed(b2);

  switch (state) {
    case S_SCAN:
      if (checkCard()) dirty = true;
      else if (p1) { lastUid[0] = '\0'; startSession("Guest", defaultTarget); dirty = true; }
      break;

    case S_LOADING:
      // load_plan() moves us on when the server answers; otherwise fall back after a timeout
      if (now - loadingSince > PLAN_WAIT_MS) {
        if (pendingUser >= 0) {
          Monitor.println("Server did not answer - using offline user list.");
          startSession(USERS[pendingUser].name, USERS[pendingUser].target);
        } else {
          Monitor.println("Server did not answer and card not in offline list.");
          state = S_UNKNOWN; unknownSince = now;
        }
        dirty = true;
      }
      break;

    case S_UNKNOWN:
      if (checkCard()) dirty = true;                       // try another card
      else if (p1 || p2 || now - unknownSince > UNKNOWN_SHOW_MS) { goToScan(); dirty = true; }
      break;

    case S_DONE:
      if (checkCard()) dirty = true;                       // next person taps in
      else if (p1) { goToScan(); dirty = true; }
      break;

    case S_EXERCISE:
      // Knob sets reps for the current exercise
      if (now - lastPotRead >= 20) {
        lastPotRead = now;
        int raw = readPot();
        if (!potLive && abs(raw - potBase) > PICKUP_MARGIN) { potLive = true; dirty = true; }
        if (potLive) {
          int r = potToReps(raw, repsDone[idx]);
          if (r != repsDone[idx]) { repsDone[idx] = r; dirty = true; }
        }
      }
      if (p1) { goNext(); dirty = true; }
      else if (p2) { goPrev(); dirty = true; }
      break;

    case S_REVIEW:
    case S_CONFIRM:
      if (p1) { goNext(); dirty = true; }
      else if (p2) { goPrev(); dirty = true; }
      break;
  }

  if (dirty) {
    // Clear the flag BEFORE drawing: a plan arriving from the server can change the state while
    // we are mid-redraw (the LCD clear has a delay in it), and that must trigger another redraw.
    dirty = false;
    redrawAll();
  }
}