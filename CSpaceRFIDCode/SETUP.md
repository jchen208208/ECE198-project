# CCCARE Tap-In Station: hardware setup guide

How to get the Arduino UNO Q station talking to the Flask server. It covers the wiring, the server, the board
software, and every problem we hit on the way.

Last verified: 2026-11-24 with Arduino App Lab 0.12.0 and the `arduino:zephyr` board package 1.1.0.

## 1. How it fits together

The UNO Q is two computers on one board:

| Side | Runs | Does |
|---|---|---|
| **MCU** (STM32U585) | `CSpaceRFIDCode.ino` | LCD, 2 OLEDs, RFID reader, knob, buttons |
| **Linux** (on the same board) | `python/main.py` (App Lab app) | Wi-Fi, HTTP to the Flask server |

The two sides talk over the **Bridge** (`Arduino_RouterBridge`). The MCU cannot make HTTP calls itself.

```
tap card ─► MCU ──card_tap(uid)──► Python ──GET /api/plan?card=──► Flask server
                                                                       │
 LCD / OLEDs ◄── MCU ◄──load_plan("OK;Name;Ex|reps;...")── Python ◄────┘

confirm  ─► MCU ──save_workout("uid;name;reps;targets")──► Python ──POST /api/workout──► Flask server
```

- A card tap fetches that patient's plan from the website, so patients, cards and plans are edited **only on the
  website**. No re-flashing is needed.
- If the server does not answer within 4 seconds, the sketch falls back to its built-in `USERS[]` list and the 4
  default exercises.
- Guests (button B1 on the scan screen) use the default exercises and are **not saved**.

## 2. Hardware

Pin map (also at the top of `CSpaceRFIDCode.ino`):

| Part | Pins |
|---|---|
| LCD 16x2 (RS, E, D4 to D7) | D2, D3, D4, D5, D6, D7. RW to GND. VDD and backlight on 5 V. |
| RC522 RFID reader | RST D9, SDA/SS D10, MOSI D11, MISO D12, SCK D13. **VCC on 3.3 V only.** |
| Rep knob (potentiometer wiper) | A0 (ends on 3.3 V and GND) |
| Button 1 (OK / next) | A2 to GND |
| Button 2 (back) | A1 to GND |
| OLED 1 (target reps) | the SDA/SCL header next to AREF (hardware I2C) |
| OLED 2 (progress) | A4 = SDA, A5 = SCL (software I2C) |

## 3. Server (your computer)

```powershell
cd <repo>
pip install -r requirements.txt
$env:API_KEY = "pick-a-random-string"        # required header X-API-Key on /api/*; unset = no check
$env:SECRET_KEY = "another-random-string"
python flaskapp.py                            # listens on 0.0.0.0:5000
```

Environment variables only last for that terminal window, so set them every time.

In the website (`http://127.0.0.1:5000`):
1. Add a patient.
2. Enter their **Card ID**. Use the UID the station shows, for example `A1 B2 C3 D4`. The website strips
   spaces and uppercases it (`A1B2C3D4`).
3. Give the patient **at least one exercise**. A card with no plan shows "No plan yet for <name>" on the LCD.

Plan order matters: the n-th number the station saves goes to the n-th exercise of the patient's active plan.

Allow Python through **Windows Defender Firewall** (private networks), or the board cannot reach the server.

### Finding a card's UID

Flash `tools/uid_reader/uid_reader.ino` with the Arduino IDE and open the Serial Monitor. It prints
`UID: A1 B2 C3 D4` for each tap. The main station sketch also shows the UID of an unknown card on its LCD.

## 4. Network

The board needs Wi-Fi: for the server link **and** to download libraries.

- **Use a Windows Mobile Hotspot (or a phone hotspot).** A school network with a username/password
  (WPA2-Enterprise) cannot be joined from App Lab, and campus networks often block device-to-device traffic.
- With the Windows hotspot: your computer is `192.168.137.1`, and the board gets `192.168.137.x`. Put
  `http://192.168.137.1:5000` in `config.json`.
- With another network: run `ipconfig` and use your computer's IPv4 address.
- Check what the board can see (from a shell on the board):
  `curl -s -H "X-API-Key: <key>" "http://192.168.137.1:5000/api/plan?card=CARD001"`

## 5. Board setup

### 5.1 Install App Lab and connect the board
1. Install **Arduino App Lab** (Windows NSIS installer from arduino.cc/en/software). Allow `mdns-discovery` through the
   firewall when asked.
2. Connect the board to your hotspot in App Lab's board settings.
3. Create an app named `ECE 198 CSpace` (its folder on the board is `~/ArduinoApps/ece-198-cspace`).

### 5.2 Update the board's package to 1.1.0 (important)

New boards ship with `arduino:zephyr` **0.51.0**, but the Arduino IDE uses **1.1.0**. They flash different memory
layouts, so a sketch uploaded by App Lab with 0.51.0 does not start after the IDE has used 1.1.0 (blank LCD,
OLED boxes, nothing responds). 0.51.0 also links all of U8g2's fonts, making the program about 13 MB, which does
not fit. Match them by upgrading the board.

Open a shell on the board: App Lab's `>_` terminal button, or from PowerShell on your computer:
```powershell
ssh arduino@<board-ip>        # password = the one you set in App Lab
```
Then run:
```
arduino-cli core list                       # shows 0.51.0
arduino-cli lib update-index                # the board's library index is old otherwise
arduino-cli core upgrade arduino:zephyr     # a few minutes
arduino-cli core list                       # should show 1.1.0
```
Rollback if needed: `arduino-cli core install arduino:zephyr@0.51.0`.

### 5.3 Files in the app

| Repo file | Goes to (in App Lab) |
|---|---|
| `CSpaceRFIDCode/CSpaceRFIDCode.ino` | `sketch/sketch.ino` |
| `CSpaceRFIDCode/MFRC522.h`, `MFRC522.cpp`, `deprecated.h`, `require_cpp11.h` | `sketch/` (same folder) |
| `CSpaceRFIDCode/python/main.py` | `python/main.py` |
| `config.json` (see below) | `python/config.json` |

The MFRC522 library is bundled because App Lab's library picker does not offer it. The sketch includes it with
quotes (`#include "MFRC522.h"`). **Do not add `MFRC522Extended.*`**; it does not compile and is not needed.

`python/config.json` (not in git; copy `config.example.json` and edit):
```json
{
  "server_url": "http://192.168.137.1:5000",
  "api_key": "<same value as the server's API_KEY>"
}
```

### 5.4 Libraries: `sketch/sketch.yaml`

The library panel can add most of these. The versions that worked:
```yaml
profiles:
  default:
    fqbn: arduino:zephyr:unoq
    platforms:
      - platform: arduino:zephyr
    libraries:
      - MsgPack (0.4.2)
      - DebugLog (0.8.4)
      - ArxContainer (0.7.0)
      - ArxTypeTraits (0.3.2)
      - Adafruit GFX Library (1.12.3)
      - Adafruit BusIO (1.17.4)
      - U8g2 (2.35.30)
      - LiquidCrystal (1.0.7)
      - Arduino_RouterBridge (0.4.4)
      - Arduino_RPClite (0.3.1)
default_profile: default
```
`Arduino_RouterBridge` must be **0.4.4 or newer** with package 1.1.0. Older versions fail with
`Please update the Arduino_RouterBridge library`. If App Lab cannot find those versions, run
`arduino-cli lib update-index` on the board. If you cannot edit the yaml in App Lab, edit it over SSH.

### 5.5 Run
Press **Run** in App Lab. The first build takes several minutes, because the board compiles U8g2 from source.
Later builds are cached. A good start is the LCD saying `Scan your card`.

## 6. Using the station
1. Tap a card. The LCD shows "Loading plan...", then the patient's first exercise.
2. Turn the **knob** to set the reps done (0 to 12). **B1** = next, **B2** = back.
3. Review screens, then Confirm (**B1** save, **B2** back).
4. The workout is saved to the website and appears in the patient's log history.

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Compile: `U8g2lib.h` / `MFRC522.h` not found | Add the library (see 5.3 and 5.4). The MFRC522 files must be in the sketch folder, included with quotes. |
| `Sketch too big`, about 13 MB (690%) | Board package is 0.51.0. Upgrade to 1.1.0 (5.2). |
| `Please update the Arduino_RouterBridge library` | Library too old. Set 0.4.4 in `sketch.yaml`. Run `arduino-cli lib update-index` on the board. |
| `Library 'Arduino_RouterBridge' not found` | The board's library index is old. Run `arduino-cli lib update-index`. |
| Library downloads fail: `lookup downloads.arduino.cc` | The board has no internet. Connect it to a hotspot. |
| LCD shows only boxes / nothing, OLEDs dark after an App Lab upload, but the IDE works | Package mismatch (5.2). |
| Upload: `timeout waiting for algorithm` / `block write failed ... 0x000f0000` | Seen with package 0.51.0's flash script (halts the chip mid-run). Fixed by upgrading to 1.1.0. |
| App Lab terminal: `invalid board address` | App Lab lost the address. SSH to the board instead. |
| "Loading plan..." forever, then the offline list | The board cannot reach the server: wrong IP, firewall, or the server is not running. |
| `Plan request failed: HTTP 401` (App Lab console) | `api_key` in `config.json` does not match the server's `API_KEY`. |
| LCD: "Unknown card" | The card is not assigned to a patient on the website. |
| LCD: "No plan yet for <name>" | The patient has no exercises. Add some on the website. |

## 8. Limitations
- One set per exercise is saved, even if the plan has several. Every exercise uses the knob as "reps"; a cardio exercise
  shows its minutes as the target.
- At most 8 exercises per plan, 16 characters per exercise name, and 12 reps on the knob.
- The offline fallback (`USERS[]` in the sketch) is only a safety net; cards normally live on the website.
- Sent over plain HTTP, so the API key is visible to anyone sniffing the network. Fine for a demo, not for real patient data.

## 9. Files
- `CSpaceRFIDCode/`: the station. `CSpaceRFIDCode.ino` is the MCU sketch, `python/main.py` is the Linux-side bridge.
- `website/api.py`: `GET /api/plan`, `POST /api/workout`, plus the older `/api/next` and `/api/log`.
- `tools/uid_reader/`: a small sketch that prints card UIDs to the Serial Monitor.
