# App Lab app (Linux side of the UNO Q):
#   card_tap     -> fetches the patient's plan from the Flask server and sends it to the sketch
#   save_workout -> POSTs the finished workout to the server
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


def _request(path, body=None):
    headers = {"Content-Type": "application/json"}
    if API_KEY:
        headers["X-API-Key"] = API_KEY
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE_URL + path, data=data, headers=headers,
                                 method="POST" if body is not None else "GET")
    with urllib.request.urlopen(req, timeout=3) as resp:
        return json.loads(resp.read().decode())


def _clean(text):
    return str(text).replace(";", " ").replace("|", " ").strip()[:16]


def _send_plan(uid: str):
    """Fetch the patient's plan and hand it to the sketch (load_plan)."""
    try:
        plan = _request("/api/plan?card=" + urllib.parse.quote(uid))
        items = [f"{_clean(e['exercise'])}|{e.get('reps') or e.get('minutes') or 0}"
                 for e in plan["exercises"][:8]]
        msg = "OK;" + _clean(plan["first_name"] or "Hi") + ";" + ";".join(items)
        if not items:
            msg = "NOPLAN;" + _clean(plan["first_name"] or "Hi")   # registered, but no exercises yet
    except urllib.error.HTTPError as e:
        msg = "UNKNOWN" if e.code == 404 else None
        print(f"Plan request failed: HTTP {e.code}")
    except Exception as e:
        msg = None                    # server unreachable: the sketch falls back to its own list
        print(f"Plan request failed: {e!r}")
    if msg:
        try:
            print("load_plan ->", Bridge.call("load_plan", msg))
        except Exception as e:
            print(f"Could not send plan to the sketch: {e!r}")


def card_tap(uid: str):
    # Do the network work in a thread so the bridge handler returns immediately
    threading.Thread(target=_send_plan, args=(uid,), daemon=True).start()


def save_workout(msg: str):
    # msg = "UID;name;r1,r2,r3,r4;t1,t2,t3,t4"
    try:
        uid, name, reps, _targets = msg.split(";")
        if not uid:
            print(f"{name}: guest session, not saved")
            return
        print(f"Saved {name} ({uid}): {_request('/api/workout', {'card': uid, 'reps': [int(r) for r in reps.split(',')]})}")
    except Exception as e:  # never let a network error crash the app
        print(f"Could not save workout: {e!r}  msg={msg!r}")


Bridge.provide("save_workout", save_workout)
Bridge.provide("card_tap", card_tap)
App.run()
