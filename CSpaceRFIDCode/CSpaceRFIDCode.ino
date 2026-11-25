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
//    1. "Scan your card" – tap an RFID card/tag. The patient's plan is loaded
//       from the website. Unknown cards show their UID. Press B1 on the scan
//       screen to start as a guest (not saved).
//    2. One step per set. Strength: turn the knob to the reps done. Cardio:
//       turn the knob to the minutes done. Mobility: marked done; turn the
//       knob to switch to skipped. B1 = next step, B2 = previous step.
//    3. Review screens, then Confirm (B1 = save, B2 = back).
//    4. "Saving..." until the website confirms. Then "Saved", or "Save failed"
//       with B1 = retry, B2 = discard. Scan a card or press B1 to start again.
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
const int MAX_STEPS = 16;             // most steps (one per set) a plan can have; same as python/main.py
const int DEFAULT_EX = 4;
const char* defaultName[DEFAULT_EX] = {"Chest press", "Squat", "Row", "Shoulder press"};
const int defaultTarget[DEFAULT_EX] = {10, 12, 10, 8};
const int MAX_REPS = 30;        // knob range for strength: 0..MAX_REPS reps over the full turn
const int MAX_MINUTES = 40;     // knob range for cardio: 0..MAX_MINUTES minutes
const unsigned long PLAN_WAIT_MS = 4000;   // how long to wait for the server before falling back
const unsigned long SAVE_WAIT_MS = 8000;   // how long to wait for the server to confirm a save

// ---------- Offline fallback users ----------
// Only used if the server can't be reached. Cards normally live in the website.
// Workouts from these sessions are NOT saved (they aren't tied to the patient's real plan).
struct User {
  uint8_t uidLen;               // 4 for most cards/fobs, 7 for some tags
  uint8_t uid[7];
  const char* name;             // max 13 chars so "Hi <name>" fits the LCD
  int target[DEFAULT_EX];
};

const User USERS[] = {
  { 4, {0xDE, 0xAD, 0xBE, 0xEF}, "Andre", {10, 12, 10, 8} },
  { 4, {0x12, 0x34, 0x56, 0x78}, "Guest2", {8, 10, 8, 6} },};
const int NUM_USERS = sizeof(USERS) / sizeof(USERS[0]);

// ---------- Session data ----------
// One "step" per set: a strength exercise with 2 sets is 2 steps.
int numSteps = DEFAULT_EX;
char exName[MAX_STEPS][17];     // max 16 chars
char exKind[MAX_STEPS];         // 's' strength (reps), 'c' cardio (minutes), 'm' mobility (done/skipped)
int dosageId[MAX_STEPS];        // the exercise's ID on the website (0 = not from the website)
int setNo[MAX_STEPS];           // set number the website logs this step as
int setIdx[MAX_STEPS];          // shown as "Set setIdx/setsOf"
int setsOf[MAX_STEPS];
int target[MAX_STEPS];          // prescribed reps / minutes (1 for mobility)
int actual[MAX_STEPS];          // what was done: reps, minutes, or 1 = done / 0 = skipped
char userName[16] = "Guest";
char lastUid[22] = "";          // "DE AD BE EF" style text
int pendingUser = -1;           // offline fallback match for the card being loaded
bool noPlan = false;            // card is registered but the patient has no exercises yet
bool fromServer = false;        // plan came from the website, so the workout can be saved

enum SaveResult { SAVE_OK, SAVE_GUEST, SAVE_OFFLINE };
SaveResult saveResult = SAVE_OK;
char saveError[22] = "";        // reason shown on "Save failed"

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
enum State { S_SCAN, S_LOADING, S_UNKNOWN, S_EXERCISE, S_REVIEW, S_CONFIRM, S_SAVING, S_SAVE_FAILED, S_DONE };
State state = S_SCAN;
int idx = 0;
bool dirty = true;
unsigned long unknownSince = 0;
unsigned long loadingSince = 0;
unsigned long savingSince = 0;
const unsigned long UNKNOWN_SHOW_MS = 4000;

// ---------- Small helpers ----------
int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int maxForKind(char kind) {
  if (kind == 's') return MAX_REPS;
  if (kind == 'c') return MAX_MINUTES;
  return 1;                     // mobility: 0 = skipped, 1 = done
}

// n-th field (from 0) of a text split on sep; "" if there is no such field
String fieldAt(const String &s, char sep, int n) {
  int start = 0;
  for (int k = 0; k < n; k++) {
    start = s.indexOf(sep, start);
    if (start < 0) return "";
    start++;
  }
  int end = s.indexOf(sep, start);
  return end < 0 ? s.substring(start) : s.substring(start, end);
}

int countFields(const String &s, char sep) {
  int n = 1;
  for (unsigned int i = 0; i < s.length(); i++) if (s.charAt(i) == sep) n++;
  return n;
}

// ---------- Potentiometer ----------
// "Pick-up" behaviour: when you arrive on a step, the knob does nothing
// until you actually turn it, so the old value isn't overwritten by
// wherever the knob happened to be.
const int ADC_MAX = 1023;       // 10-bit (set in setup)
const int PICKUP_MARGIN = 40;   // how far the knob must move to "grab" the value
const int TOGGLE_MARGIN = 250;  // mobility: a turn this big switches done <-> skipped
int potBase = 0;
bool potLive = false;
unsigned long lastPotRead = 0;

int readPot() {
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_POT);
  return (int)(sum / 8);
}

// Raw reading -> 0..maxV, with hysteresis so the number doesn't flicker
int potToValue(int raw, int current, int maxV) {
  const float step = (float)(ADC_MAX + 1) / (maxV + 1);
  float centre = (current + 0.5f) * step;
  if (fabs(raw - centre) < step * 0.65f) return current;   // stay put
  return clampInt((int)(raw / step), 0, maxV);
}

void armPot() {
  potBase = readPot();
  potLive = false;
}

// ---------- Session control ----------
// The built-in 4 exercises (guests / offline fallback). These are never saved.
void loadDefaults(const int* targets) {
  numSteps = DEFAULT_EX;
  for (int i = 0; i < numSteps; i++) {
    strncpy(exName[i], defaultName[i], 16); exName[i][16] = '\0';
    exKind[i] = 's';
    dosageId[i] = 0;
    setNo[i] = 1; setIdx[i] = 1; setsOf[i] = 1;
    target[i] = clampInt(targets[i], 0, MAX_REPS);
    actual[i] = 0;
  }
  fromServer = false;
}

void startSession(const char* name, const int* targets) {
  strncpy(userName, name, sizeof(userName) - 1);
  userName[sizeof(userName) - 1] = '\0';
  loadDefaults(targets);
  idx = 0;
  state = S_EXERCISE;
  armPot();
  Monitor.print("Session started for ");
  Monitor.println(name);
}

// Called from the Linux side (python/main.py) after a card tap.
//   "UNKNOWN"                card isn't registered on the website
//   "NOPLAN;name"            registered, but no exercises assigned yet
//   "OK;name;exercise|kind|dosageId|firstSet|sets|target;..."
//        kind: s = strength (target reps), c = cardio (target minutes), m = mobility
//        Each exercise becomes one step per set (up to MAX_STEPS steps).
// Runs via provide_safe, i.e. from loop() context, so it can touch the session state.
String loadPlan(String msg) {
  if (state != S_LOADING) return "ignored";       // answer arrived late (fallback already ran)
  if (msg.startsWith("NOPLAN;")) {
    strncpy(userName, msg.c_str() + 7, sizeof(userName) - 1);
    userName[sizeof(userName) - 1] = '\0';
    noPlan = true;
    state = S_UNKNOWN; unknownSince = millis(); dirty = true;
    return "ok";
  }
  if (!msg.startsWith("OK;")) {                    // "UNKNOWN" or garbled
    state = S_UNKNOWN; unknownSince = millis(); dirty = true;
    return msg.startsWith("UNKNOWN") ? "ok" : "bad";
  }

  String name = fieldAt(msg, ';', 1);
  int items = countFields(msg, ';');
  int n = 0;
  for (int k = 2; k < items && n < MAX_STEPS; k++) {
    String item = fieldAt(msg, ';', k);
    String ex = fieldAt(item, '|', 0);
    char kind = fieldAt(item, '|', 1).charAt(0);
    int id = fieldAt(item, '|', 2).toInt();
    int first = fieldAt(item, '|', 3).toInt();
    int sets = fieldAt(item, '|', 4).toInt();
    int tgt = fieldAt(item, '|', 5).toInt();
    if (ex.length() == 0 || id <= 0 || (kind != 's' && kind != 'c' && kind != 'm')) continue;
    if (first < 1) first = 1;
    if (sets < 1) sets = 1;
    for (int s = 0; s < sets && n < MAX_STEPS; s++) {
      strncpy(exName[n], ex.c_str(), 16); exName[n][16] = '\0';
      exKind[n] = kind;
      dosageId[n] = id;
      setNo[n] = first + s;
      setIdx[n] = s + 1;
      setsOf[n] = sets;
      target[n] = clampInt(tgt, 0, maxForKind(kind));
      actual[n] = (kind == 'm') ? 1 : 0;           // mobility starts as "done"
      n++;
    }
  }
  if (n == 0) { state = S_UNKNOWN; unknownSince = millis(); dirty = true; return "empty"; }
  strncpy(userName, name.c_str(), sizeof(userName) - 1);
  userName[sizeof(userName) - 1] = '\0';
  numSteps = n;
  fromServer = true;
  idx = 0;
  state = S_EXERCISE;
  armPot();
  dirty = true;
  return "ok";
}

// Send the workout to the Linux side, which POSTs it and answers with save_result.
// One string: "UID;name;dosageId:set:value,..."
void sendSave() {
  String msg = String(lastUid) + ";" + userName + ";";
  for (int i = 0; i < numSteps; i++) {
    if (i) msg += ",";
    msg += dosageId[i]; msg += ":"; msg += setNo[i]; msg += ":"; msg += actual[i];
  }
  state = S_SAVING;
  savingSince = millis();
  Bridge.notify("save_workout", msg);
}

// Called from the Linux side once the server answered: "OK" or "FAIL;reason".
String saveDone(String msg) {
  if (state != S_SAVING) return "ignored";         // too late: already timed out
  if (msg == "OK") {
    saveResult = SAVE_OK;
    state = S_DONE;
    Monitor.println("Workout saved.");
  } else {
    String reason = msg.startsWith("FAIL;") ? msg.substring(5) : msg;
    strncpy(saveError, reason.c_str(), sizeof(saveError) - 1);
    saveError[sizeof(saveError) - 1] = '\0';
    state = S_SAVE_FAILED;
    Monitor.print("Save failed: ");
    Monitor.println(saveError);
  }
  dirty = true;
  return "ok";
}

void finishWorkout() {
  printSummary();
  if (fromServer) { sendSave(); return; }
  // Guest or offline-fallback session: not tied to a plan on the website, so not saved
  saveResult = lastUid[0] ? SAVE_OFFLINE : SAVE_GUEST;
  state = S_DONE;
  Monitor.println("Not saved (guest / offline session).");
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
      if (idx < numSteps - 1) {
        idx++;
        if (exKind[idx] != 'm' && actual[idx] == 0 && exKind[idx] == exKind[idx - 1]) {
          // New step of the same kind: carry over the previous number and keep the knob
          // live, so the value doesn't drop to 0 when you press next.
          actual[idx] = actual[idx - 1];
          potBase = readPot();
          potLive = true;
        } else {
          armPot();            // protect the value until the knob is turned
        }
        return;
      }
      state = S_REVIEW; idx = 0;
      break;
    case S_REVIEW:
      if (idx < numSteps - 1) idx++;
      else state = S_CONFIRM;
      break;
    case S_CONFIRM:
      finishWorkout();
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
      else { state = S_EXERCISE; idx = numSteps - 1; }
      break;
    case S_CONFIRM:
      state = S_REVIEW;
      idx = numSteps - 1;
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
  for (int i = 0; i < numSteps; i++) if (actual[i] >= target[i]) n++;
  return n;
}

void lcdSet(int i) {
  lcd.print("Set ");
  lcd.print(setIdx[i]);
  lcd.print("/");
  lcd.print(setsOf[i]);
}

// Second LCD line while entering a step (fits 16 chars)
void lcdStepLine(int i) {
  if (exKind[i] == 's') {
    lcdSet(i);
    if (potLive) { lcd.print(" Reps:"); lcd.print(actual[i]); }
    else lcd.print(" Use knob");
  } else if (exKind[i] == 'c') {
    lcd.print("Minutes: ");
    if (potLive) lcd.print(actual[i]);
    else lcd.print("knob");
  } else {
    lcd.print(actual[i] ? "Done. Knob=skip" : "Skipped Knob=do");
  }
}

// Second LCD line on the review screens
void lcdReviewLine(int i) {
  if (exKind[i] == 's') {
    lcdSet(i);
    lcd.print(" ");
    lcd.print(actual[i]);
    lcd.print(" of ");
    lcd.print(target[i]);
  } else if (exKind[i] == 'c') {
    lcd.print(actual[i]);
    lcd.print(" of ");
    lcd.print(target[i]);
    lcd.print(" min");
  } else {
    lcd.print(actual[i] ? "Done" : "Skipped");
  }
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
      lcdStepLine(idx);
      break;
    case S_REVIEW:
      lcd.print(exName[idx]);
      lcd.setCursor(0, 1);
      lcdReviewLine(idx);
      break;
    case S_CONFIRM:
      lcd.print("Confirm entries?");
      lcd.setCursor(0, 1);
      lcd.print("B1=OK  B2=Back");
      break;
    case S_SAVING:
      lcd.print("Saving...");
      lcd.setCursor(0, 1);
      lcd.print(userName);
      break;
    case S_SAVE_FAILED:
      lcd.print("Save failed!");
      lcd.setCursor(0, 1);
      lcd.print("B1=retry B2=drop");
      break;
    case S_DONE:
      if (saveResult == SAVE_OK) { lcd.print("Saved, "); lcd.print(userName); }
      else if (saveResult == SAVE_GUEST) lcd.print("Guest: not saved");
      else lcd.print("Offline: unsaved");
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
        if (exKind[idx] == 'm') {
          oled1.drawStr(0, 14, "MOBILITY");
          oled1.setFont(u8g2_font_6x10_tr);
          oled1.drawStr(0, 36, "Do it, then B1.");
          oled1.drawStr(0, 48, "Turn knob = skip");
        } else {
          oled1.drawStr(0, 14, exKind[idx] == 'c' ? "TARGET MINUTES" : "TARGET REPS");
          oled1.setFont(u8g2_font_logisoso32_tn);
          oled1.setCursor(0, 58);
          oled1.print(target[idx]);
        }
        break;
      case S_CONFIRM:
        oled1.drawStr(0, 14, "REVIEW DONE");
        oled1.setFont(u8g2_font_6x10_tr);
        oled1.drawStr(0, 36, "Check the main");
        oled1.drawStr(0, 48, "screen to confirm");
        break;
      case S_SAVING:
        oled1.drawStr(0, 30, "SAVING...");
        break;
      case S_SAVE_FAILED:
        oled1.drawStr(0, 14, "SAVE FAILED");
        oled1.setFont(u8g2_font_6x10_tr);
        oled1.drawStr(0, 32, saveError);
        oled1.drawStr(0, 48, "B1 retry  B2 drop");
        break;
      case S_DONE:
        oled1.drawStr(0, 30, saveResult == SAVE_OK ? "SAVED" : "NOT SAVED");
        oled1.setFont(u8g2_font_6x10_tr);
        oled1.drawStr(0, 50, saveResult == SAVE_OK ? userName
                             : saveResult == SAVE_GUEST ? "Guest session" : "Server was offline");
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
        bool mob = exKind[idx] == 'm';
        oled2.drawStr(0, 14, mob ? "STATUS" : exKind[idx] == 'c' ? "MINUTES DONE" : "REPS DONE");
        if (mob) {
          oled2.drawStr(0, 44, actual[idx] ? "DONE" : "SKIP");
        } else {
          oled2.setFont(u8g2_font_logisoso32_tn);
          oled2.setCursor(0, 52);
          oled2.print(actual[idx]);
        }
        // progress bar
        oled2.drawFrame(66, 20, 60, 12);
        int tgt = target[idx] > 0 ? target[idx] : 1;
        int w = (int)((long)clampInt(actual[idx], 0, tgt) * 56 / tgt);
        if (w > 0) oled2.drawBox(68, 22, w, 8);
        oled2.setFont(u8g2_font_6x10_tr);
        oled2.setCursor(66, 46);
        oled2.print("Step ");
        oled2.print(idx + 1);
        oled2.print("/");
        oled2.print(numSteps);
        oled2.setCursor(66, 60);
        oled2.print(mob ? "B1 = next" : actual[idx] >= target[idx] ? "Goal met" : "Keep going");
        break;
      }
      case S_CONFIRM:
      case S_SAVING:
      case S_SAVE_FAILED:
      case S_DONE:
        oled2.drawStr(0, 14, state == S_DONE ? "COMPLETE" : "SUMMARY");
        oled2.setFont(u8g2_font_6x10_tr);
        oled2.setCursor(0, 36);
        oled2.print("Goals met: ");
        oled2.print(targetsMet());
        oled2.print("/");
        oled2.print(numSteps);
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
  for (int i = 0; i < numSteps; i++) {
    Monitor.print(exName[i]);
    Monitor.print(" set ");
    Monitor.print(setIdx[i]);
    Monitor.print(": ");
    Monitor.print(actual[i]);
    Monitor.print("/");
    Monitor.println(target[i]);
  }
  Monitor.print("Goals met: ");
  Monitor.print(targetsMet());
  Monitor.print("/");
  Monitor.println(numSteps);
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

  loadDefaults(defaultTarget);   // fill the step arrays with something sensible
  bool bound = Bridge.provide_safe("load_plan", loadPlan);
  bool boundSave = Bridge.provide_safe("save_result", saveDone);
  if (!bound || !boundSave) {
    Monitor.println("Could not register load_plan/save_result - server link will not work.");
    lcd.clear();
    lcd.print(bound ? "save_result fail" : "load_plan failed");
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
          Monitor.println("Server did not answer - using offline user list (will not be saved).");
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

    case S_SAVING:
      // save_result() moves us on when the server answers; no answer at all counts as a failure
      if (now - savingSince > SAVE_WAIT_MS) {
        strncpy(saveError, "No answer", sizeof(saveError));
        state = S_SAVE_FAILED;
        Monitor.println("Save failed: no answer from the Linux side.");
        dirty = true;
      }
      break;

    case S_SAVE_FAILED:
      if (p1) { sendSave(); dirty = true; }                // retry (the server ignores duplicates)
      else if (p2) { Monitor.println("Workout discarded."); goToScan(); dirty = true; }
      break;

    case S_DONE:
      if (checkCard()) dirty = true;                       // next person taps in
      else if (p1) { goToScan(); dirty = true; }
      break;

    case S_EXERCISE:
      if (now - lastPotRead >= 20) {
        lastPotRead = now;
        int raw = readPot();
        if (exKind[idx] == 'm') {
          // Mobility: done by default; a clear turn of the knob switches done <-> skipped
          if (abs(raw - potBase) > TOGGLE_MARGIN) { actual[idx] = actual[idx] ? 0 : 1; potBase = raw; dirty = true; }
        } else {
          // Strength / cardio: the knob sets reps / minutes
          if (!potLive && abs(raw - potBase) > PICKUP_MARGIN) { potLive = true; dirty = true; }
          if (potLive) {
            int r = potToValue(raw, actual[idx], maxForKind(exKind[idx]));
            if (r != actual[idx]) { actual[idx] = r; dirty = true; }
          }
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
