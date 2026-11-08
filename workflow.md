# CCCARE Tap-In Exercise Log

## What it is

CCCARE runs supervised exercise programs for older adults, including cancer patients, stroke survivors, and program graduates. Each participant follows a personalized exercise plan called a **dosage**: which exercises to do, and how many sets, reps, and how much load. Right now participants record what they did on **paper logs**, which staff have to read through by hand before adjusting plans.

Our product replaces the paper log. Participants use an **ESP32 station**: they tap an RFID card, see the exercise they're supposed to do next, and confirm what they actually did. That data goes straight into a database, which staff view and edit in a simple **staff app**.

- **Main focus:** Sub Problem 1 (Scale Personalized Care): prescribe, adjust, store, and share exercise doses.
- **Also touches:** Sub Problem 3 (Remote Care), through home mode.

## Workflow

### Staff setup (once per participant)
1. At intake, staff add the participant and their dosage in the staff app.
2. Staff tap a blank RFID card on the station, and the app links that card's ID to the participant. The card stores no personal info.

### Participant (after each exercise)
1. Tap the card at the station.
2. The screen shows the participant's first name and their next exercise, filled in with the prescribed values:
   ```
   Hi Pat!
   Goblet squat   Set 1/2
   Reps: 15   Load: 10 lb
    −     +     ✓ Done
   ```
3. If they did exactly that, they press **✓**. If they did something different, they adjust with **− / +** and then press **✓**.
4. Repeat for each set. When the exercise is done, the station goes back to "Tap your card".

What gets logged for each type of exercise:
- **Strength:** reps and load for each set
- **Cardio:** minutes
- **Mobility / stretches:** ✓ done

### Staff app
CCCARE's Clinical Exercise Physiologists (CEPs) and Registered Kinesiologists (R.KINs) use a simple staff app. Plain, unstyled pages are fine. In the app they can:
- See a list of participants and open one
- View and **edit** that participant's dosage: exercises, sets, reps, and load (for example, change goblet squats from 15 to 18 reps)
- View the participant's log history (what they actually did each session)
- See flags:
  - **Increase load:** the participant completed 20 reps two sessions in a row
  - **New exercises due:** every 12 sessions

Edits are saved to the database right away, so the station shows the new values the next time that participant taps in.

The prototype has no login. A real version would need staff logins, since this is health data.

## Modes

| | Clinic mode (what we build) | Home mode (future idea, for the presentation) |
|---|---|---|
| Where | Shared station in CCCARE's exercise space | Participant's home |
| Users | Many (10–30 per session) | One participant |
| How it knows who's logging | Tap an RFID card | No card needed: the device's ID is linked to the participant |
| Extras | Privacy rules below | Daily reminder buzzer; Wi-Fi set up by staff (or with WiFiManager) |

Both modes use the same hardware and code, with a mode setting, and both log to the same participant record. A graduate could use the clinic station on some days and a home device on others.

**Clinic mode rules (the screen is shared):**
- Show only the participant's first name and the exercise. No medical info.
- Go back to "Tap your card" after ✓, or after 30–60 s with no button presses.
- One tap per exercise, so it's always clear who is logging.

## System

```
RFID card → RC522 ─┐
OLED ──────────────┼─ ESP32 ── Wi-Fi (HTTP) ──► Laptop: Flask server + SQLite database
Buttons ───────────┘                                     ▲
                                            Staff app (web pages from the same server)
```

The ESP32 sends two kinds of requests to the laptop:
1. **On tap:** "What's the next exercise for card `A1B2C3D4`?" The server replies with the exercise, set, reps, and load.
2. **On ✓:** "Log card `A1B2C3D4`: goblet squat, set 1, 15 reps, 10 lb." The server saves it.

The staff app is just a few more pages in the same Flask server. Staff open it in a browser on the laptop, or on a tablet or phone on the same network. There's nothing to install or deploy, and only one program touches the database.

**Why a laptop server instead of a cloud database (for the prototype):**
- It's simpler: no cloud accounts or security certificates to set up on the ESP32.
- It's easier to debug, and it works without internet.
- It keeps health data (protected under Ontario's PHIPA privacy law) on a machine the clinic controls.

Moving to the cloud later only means changing the server address in the ESP32 code.

## Database (3 tables)

| Table | What it stores |
|---|---|
| `participants` | ID, first name, card ID |
| `dosages` | What each participant should do: exercise, type (strength / cardio / mobility), order, sets, reps, load. **Staff edit this one.** |
| `logs` | What each participant actually did: exercise, set, reps, load (or minutes), time |

The station reads from `dosages` and writes to `logs`. The flags are worked out from `logs`.

## Parts

| Part | Notes | ~Cost |
|---|---|---|
| ESP32 dev board | Already have | — |
| RC522 RFID reader | Usually comes with 1 card and 1 fob. The pin headers may need soldering. | $5–10 |
| 13.56 MHz MIFARE cards/fobs | A pack, so we can demo several participants | $5–10 |
| OLED display (I2C) | 1.3" is easier to read than 0.96" | $5–15 |
| 3 push buttons | −, +, ✓ | $1–5 |
| Breadboard + jumper wires | Male-to-female wires for the RC522 | ~$5 |
| Buzzer (optional) | Beeps when a tap is read | ~$1 |

## Wiring (classic ESP32 DevKit / WROOM-32)

| Component | Pin | ESP32 |
|---|---|---|
| RC522 | SDA (SS) / SCK / MOSI / MISO | GPIO 5 / 18 / 23 / 19 |
| RC522 | RST | GPIO 27 |
| RC522 | 3.3V / GND | **3V3 (not 5V)** / GND |
| RC522 | IRQ | not connected |
| OLED | SDA / SCL | GPIO 21 / 22 |
| OLED | VCC / GND | 3V3 / GND |
| Buttons | − / + / ✓ | GPIO 25 / 26 / 32 (other leg of each to GND, use `INPUT_PULLUP`) |
| Buzzer (optional) | + | GPIO 33 |

## Software
- **ESP32:** `MFRC522`, `U8g2`, `ArduinoJson`, `WiFi` + `HTTPClient` (built in)
- **Server:** Python Flask + SQLite
- **Staff app:** a few plain HTML pages in the same Flask server (no styling needed)

## Milestones
- **Progress update (week of Nov 9):** tap a card → the OLED shows the participant's name sent from the laptop → the tap is saved in the database.
- **Symposium (Nov 23 / Nov 25):** the full logging flow, plus the staff app for viewing history and flags and editing dosages.

## Demo tips
- Use a phone hotspot. ESP32s have trouble connecting to eduroam.
- Allow Python through Windows Firewall so the ESP32 can reach the server.

## Questions for CCCARE / teaching team
1. Do participants already have membership cards? If so, what kind (13.56 MHz, 125 kHz, or barcode)?
2. What does the exercise space look like? One room or several?
3. Is there Wi-Fi and power where a station could go?
