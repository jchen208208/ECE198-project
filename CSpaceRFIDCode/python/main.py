# App Lab app (Linux side of the UNO Q):
#   card_tap     -> fetches the patient's plan from the Flask server and sends it to the sketch (load_plan)
#   save_workout -> POSTs the finished workout to the server, then tells the sketch whether it
#                   worked (save_result), so the station only says "Saved" when it really was
#
# Put this in your App Lab app's python/main.py (next to the sketch).
# Set SERVER_URL to your computer's LAN IP (NOT localhost: that is the board itself).
#   Windows:  ipconfig  ->  IPv4 Address, e.g. 192.168.1.23
import json
import threading
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from arduino.app_utils import App, Bridge

# Settings live in config.json (gitignored). Copy config.example.json to config.json and edit it:
#   server_url: your computer's IP (NOT localhost), api_key: same value as the server's API_KEY
_dir = Path(__file__).resolve().parent
_cfg_file = _dir / "config.json"
if not _cfg_file.exists():
    _cfg_file = _dir / "config.example.json"
    print("config.json not found - using config.example.json (probably the wrong IP)")
CONFIG = json.loads(_cfg_file.read_text())
# server_url may be "http://ip:5000" or the old full ".../api/workout" form; keep just the base
BASE_URL = CONFIG["server_url"].split("/api")[0].rstrip("/")
API_KEY = CONFIG.get("api_key", "")

MAX_STEPS = 16  # must match MAX_STEPS in the sketch: one step per set
KINDS = {"strength": "s", "cardio": "c", "mobility": "m"}


def _request(path, body=None, timeout=3):
    headers = {"Content-Type": "application/json"}
    if API_KEY:
        headers["X-API-Key"] = API_KEY
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE_URL + path, data=data, headers=headers,
                                 method="POST" if body is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def _clean(text):
    return str(text).replace(";", " ").replace("|", " ").replace(",", " ").replace(":", " ").strip()[:16]


def _tell_sketch(name, msg):
    try:
        print(f"{name} ->", Bridge.call(name, msg))
    except Exception as e:
        print(f"Could not send {name} to the sketch: {e!r}")


# ---------- card tap: plan ----------

def plan_message(plan):
    """Server plan -> "OK;name;exercise|kind|dosage_id|first_set|sets|target;..." for the sketch.
    kind: s = strength (target = reps), c = cardio (target = minutes), m = mobility (target = 1).
    The sketch shows one step per set, so the plan is cut off (between exercises) at MAX_STEPS sets."""
    name = _clean(plan["first_name"] or "Hi")
    items, steps = [], 0
    for e in plan["exercises"]:
        kind = KINDS.get(e["type"], "m")
        sets = max(1, e.get("sets") or 1) if kind == "s" else 1
        if steps + sets > MAX_STEPS:
            print(f"Plan has more than {MAX_STEPS} sets: the station stops before {e['exercise']}")
            break
        target = e.get("reps") if kind == "s" else e.get("minutes") if kind == "c" else 1
        items.append(f"{_clean(e['exercise'])}|{kind}|{e['dosage_id']}|{e['next_set']}|{sets}|{target or 0}")
        steps += sets
    if not items:
        return "NOPLAN;" + name   # registered, but no exercises yet
    return "OK;" + name + ";" + ";".join(items)


def _send_plan(uid: str):
    """Fetch the patient's plan and hand it to the sketch (load_plan)."""
    try:
        msg = plan_message(_request("/api/plan?card=" + urllib.parse.quote(uid)))
    except urllib.error.HTTPError as e:
        msg = "UNKNOWN" if e.code == 404 else None
        print(f"Plan request failed: HTTP {e.code}")
    except Exception as e:
        msg = None                    # server unreachable: the sketch falls back to its own list
        print(f"Plan request failed: {e!r}")
    if msg:
        _tell_sketch("load_plan", msg)


def card_tap(uid: str):
    # Do the network work in a thread so the bridge handler returns immediately
    threading.Thread(target=_send_plan, args=(uid,), daemon=True).start()


# ---------- confirm: save ----------

def workout_body(msg):
    """Sketch message "UID;name;dosage_id:set:value,..." -> (uid, name, JSON body for /api/workout)."""
    uid, name, entries = msg.split(";", 2)
    sets = []
    for entry in filter(None, entries.split(",")):
        dosage_id, set_number, value = (int(x) for x in entry.split(":"))
        sets.append({"dosage_id": dosage_id, "set": set_number, "value": value})
    return uid, name, {"card": uid, "sets": sets}


def _http_reason(e):
    try:
        code = json.loads(e.read().decode()).get("error", "")
    except Exception:
        code = ""
    return f"HTTP {e.code} {code}".strip()[:21]


def _save(msg: str):
    """POST the workout, then tell the sketch: "OK" or "FAIL;<reason up to 21 chars>"."""
    try:
        uid, name, body = workout_body(msg)
        reply = _request("/api/workout", body, timeout=5)
        print(f"Saved {name} ({uid}): {reply}")
        result = "OK"
    except urllib.error.HTTPError as e:   # the server answered, but said no
        result = "FAIL;" + _http_reason(e)
        print(f"Could not save workout: {result}  msg={msg!r}")
    except (urllib.error.URLError, OSError) as e:   # includes timeouts
        result = "FAIL;No server connection"
        print(f"Could not save workout: {e!r}  msg={msg!r}")
    except Exception as e:  # e.g. a garbled message; never let it crash the app
        result = "FAIL;Station error"
        print(f"Could not save workout: {e!r}  msg={msg!r}")
    _tell_sketch("save_result", result)


def save_workout(msg: str):
    threading.Thread(target=_save, args=(msg,), daemon=True).start()


Bridge.provide("save_workout", save_workout)
Bridge.provide("card_tap", card_tap)
App.run()
