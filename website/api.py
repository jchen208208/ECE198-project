"""Station API: what the ESP32 station (and the simulator page) call.

GET /api/next?card=CARD001
    The next exercise for the patient with this card.
    200 {"done": false, "first_name": "Pat", "dosage_id": 5, "exercise": "Goblet squats",
         "type": "strength", "set": 1, "sets": 2, "reps": 15, "load_lb": 10.0, "minutes": null}
    200 {"done": true, "first_name": "Pat", "no_plan": false}      everything done today
                                                                    (no_plan: true = nothing assigned yet)
    404 {"error": "unknown_card", "message": "..."}

POST /api/log      JSON body, e.g. {"card": "CARD001", "dosage_id": 5, "set": 1, "reps": 15, "load_lb": 10}
    Strength: send reps (and load_lb). Cardio: send minutes. Mobility: send neither.
    Anything left out is filled in from the plan, so a plain "confirm" only needs card + dosage_id.
    "set" is optional too: left out, the server uses the next set number.
    Sending the same set again (e.g. a retry after a dropped connection) updates it instead of
    adding a duplicate.
    200 {"ok": true, "log_id": 12, "set": 1, "updated": false}
    400 {"error": "bad_value" / "bad_dosage" / "bad_request", "message": "..."}
    404 {"error": "unknown_card", "message": "..."}

Replies are small, flat JSON so the ESP32 can read them with ArduinoJson.
"""
import hmac
import math
import os
from datetime import date, datetime, time

from flask import Blueprint, jsonify, request

from database.extensions import db
from database.models import Dosage, ExerciseLog, Patient

from .routes import normalize_card

api = Blueprint("api", __name__, url_prefix="/api")


@api.before_request
def _check_api_key():
    """If the API_KEY environment variable is set, stations must send it as an X-API-Key header.
    Not set = no check (handy for the simulator page and local testing)."""
    expected = os.environ.get("API_KEY")
    if expected and not hmac.compare_digest(request.headers.get("X-API-Key", ""), expected):
        return _error(401, "unauthorized", "Missing or wrong X-API-Key.")


# ---------- helpers ----------

def _error(status, code, message):
    return jsonify(error=code, message=message), status


def _patient_for_card(value):
    card = normalize_card(value) if isinstance(value, str) else None
    if card is None:
        return None
    return db.session.execute(db.select(Patient).where(Patient.card_uid == card)).scalar_one_or_none()


def _first_name(patient):
    parts = (patient.name or "").split()
    return parts[0] if parts else ""


def _start_of_today():
    return datetime.combine(date.today(), time.min)


def _sets_required(dosage):
    """Strength exercises have several sets; cardio and mobility count as a single set."""
    return dosage.sets if dosage.exercise_type == "strength" and dosage.sets else 1


def _sets_done_today(patient_id):
    """{dosage_id: number of different sets logged today} for one patient."""
    query = (
        db.select(ExerciseLog.dosage_id, db.func.count(db.distinct(ExerciseLog.set_number)))
        .where(ExerciseLog.patient_id == patient_id, ExerciseLog.logged_at >= _start_of_today())
        .group_by(ExerciseLog.dosage_id)
    )
    return dict(db.session.execute(query).all())


def _last_set_today(patient_id):
    """{dosage_id: highest set number logged today} for one patient."""
    query = (
        db.select(ExerciseLog.dosage_id, db.func.max(ExerciseLog.set_number))
        .where(ExerciseLog.patient_id == patient_id, ExerciseLog.logged_at >= _start_of_today())
        .group_by(ExerciseLog.dosage_id)
    )
    return {dosage_id: last or 0 for dosage_id, last in db.session.execute(query).all()}


def _save_set(patient, dosage, set_number, reps, load_lb, minutes):
    """Log one set. If that set was already logged today (e.g. a retry), update it instead of adding
    a duplicate. Returns True if an existing log was updated. The caller commits."""
    log = db.session.execute(
        db.select(ExerciseLog).where(
            ExerciseLog.patient_id == patient.patient_id,
            ExerciseLog.dosage_id == dosage.dosage_id,
            ExerciseLog.set_number == set_number,
            ExerciseLog.logged_at >= _start_of_today(),
        )
    ).scalars().first()
    updated = log is not None
    if log is None:
        log = ExerciseLog(patient=patient, dosage=dosage, set_number=set_number)
        db.session.add(log)
    log.reps, log.load_lb, log.minutes = reps, load_lb, minutes
    log.logged_at = datetime.now()
    return log, updated


def _read_number(data, key, whole):
    """Optional JSON number, 0 or more. Missing or null -> None. Raises ValueError otherwise."""
    value = data.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{key} must be a number.")
    if value < 0:
        raise ValueError(f"{key} must be 0 or more.")
    if whole:
        if value != int(value):
            raise ValueError(f"{key} must be a whole number.")
        return int(value)
    return float(value)


# ---------- endpoints ----------

@api.get("/next")
def next_exercise():
    if not request.args.get("card"):
        return _error(400, "missing_card", "Add ?card=<card ID> to the URL.")
    patient = _patient_for_card(request.args["card"])
    if patient is None:
        return _error(404, "unknown_card", "This card isn't assigned to a patient.")

    plan = sorted((d for d in patient.dosages if d.is_active), key=lambda d: (d.position, d.dosage_id))
    done_today = _sets_done_today(patient.patient_id)
    for dosage in plan:
        sets_done = done_today.get(dosage.dosage_id, 0)
        if sets_done < _sets_required(dosage):
            return jsonify(
                done=False,
                first_name=_first_name(patient),
                dosage_id=dosage.dosage_id,
                exercise=dosage.exercise,
                type=dosage.exercise_type,
                set=sets_done + 1,
                sets=_sets_required(dosage),
                reps=dosage.reps,
                load_lb=dosage.load_lb,
                minutes=dosage.minutes,
            )
    return jsonify(done=True, first_name=_first_name(patient), no_plan=not plan)


@api.get("/plan")
def plan():
    """The patient's whole active plan in order (used by the UNO Q station on a card tap).

    GET /api/plan?card=CARD001
    200 {"first_name": "Pat", "exercises": [{"dosage_id": 5, "exercise": "Goblet squats",
         "type": "strength", "sets": 2, "reps": 15, "load_lb": 10.0, "minutes": null,
         "next_set": 1}, ...]}
    404 {"error": "unknown_card", ...}

    next_set is the set number to log first: 1, or higher if the patient already logged sets of
    that exercise today. The station sends dosage_id and set numbers back to POST /api/workout.
    """
    if not request.args.get("card"):
        return _error(400, "missing_card", "Add ?card=<card ID> to the URL.")
    patient = _patient_for_card(request.args["card"])
    if patient is None:
        return _error(404, "unknown_card", "This card isn't assigned to a patient.")
    dosages = sorted((d for d in patient.dosages if d.is_active), key=lambda d: (d.position, d.dosage_id))
    last_set = _last_set_today(patient.patient_id)
    return jsonify(
        first_name=_first_name(patient),
        exercises=[
            dict(dosage_id=d.dosage_id, exercise=d.exercise, type=d.exercise_type, sets=d.sets,
                 reps=d.reps, load_lb=d.load_lb, minutes=d.minutes,
                 next_set=last_set.get(d.dosage_id, 0) + 1)
            for d in dosages
        ],
    )


@api.post("/log")
def log_set():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return _error(400, "bad_request", "Send a JSON object with Content-Type: application/json.")

    patient = _patient_for_card(data.get("card"))
    if patient is None:
        return _error(404, "unknown_card", "This card isn't assigned to a patient.")

    try:
        dosage_id = _read_number(data, "dosage_id", whole=True)
        set_number = _read_number(data, "set", whole=True)
        reps = _read_number(data, "reps", whole=True)
        load_lb = _read_number(data, "load_lb", whole=False)
        minutes = _read_number(data, "minutes", whole=True)
    except ValueError as error:
        return _error(400, "bad_value", str(error))

    dosage = db.session.get(Dosage, dosage_id) if dosage_id is not None else None
    if dosage is None or dosage.patient_id != patient.patient_id or not dosage.is_active:
        return _error(400, "bad_dosage", "That exercise isn't in this patient's current plan.")

    # Keep only the values that fit this exercise type; anything left out comes from the plan.
    if dosage.exercise_type == "strength":
        reps = dosage.reps if reps is None else reps
        load_lb = dosage.load_lb if load_lb is None else load_lb
        minutes = None
    elif dosage.exercise_type == "cardio":
        minutes = dosage.minutes if minutes is None else minutes
        reps = load_lb = None
    else:  # mobility: just "done"
        reps = load_lb = minutes = None

    if set_number is None:
        set_number = _sets_done_today(patient.patient_id).get(dosage.dosage_id, 0) + 1
    elif set_number < 1:
        return _error(400, "bad_value", "set must be 1 or more.")

    log, updated = _save_set(patient, dosage, set_number, reps, load_lb, minutes)
    db.session.commit()

    return jsonify(ok=True, log_id=log.log_id, set=set_number, updated=updated)


@api.post("/workout")
def log_workout():
    """Whole workout in one call (used by the UNO Q station).

    POST {"card": "DEADBEEF", "sets": [{"dosage_id": 5, "set": 1, "value": 15}, ...]}
    One entry per set. dosage_id and set come from GET /api/plan (dosage_id, next_set onwards).
    value: reps (strength; load comes from the plan), minutes (cardio), 1 = done (mobility).
    value 0 = skipped: nothing is logged for that set.
    Sending the same workout again (a retry after a dropped connection) updates those sets
    instead of adding duplicates. An exercise removed from the plan after the card tap is still
    logged, since the patient did it.
    200 {"ok": true, "logged": 3, "updated": 0, "skipped": 1}     logged = new sets saved
    400 {"error": "bad_request" / "bad_value" / "bad_dosage", ...}  nothing is saved
    404 {"error": "unknown_card", ...}
    """
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return _error(400, "bad_request", "Send a JSON object with Content-Type: application/json.")
    patient = _patient_for_card(data.get("card"))
    if patient is None:
        return _error(404, "unknown_card", "This card isn't assigned to a patient.")

    entries = data.get("sets")
    if not isinstance(entries, list) or not all(isinstance(entry, dict) for entry in entries):
        return _error(400, "bad_value", "sets must be a list of {dosage_id, set, value} objects.")
    rows = []
    try:
        for entry in entries:
            dosage_id = _read_number(entry, "dosage_id", whole=True)
            set_number = _read_number(entry, "set", whole=True)
            value = _read_number(entry, "value", whole=True)
            if dosage_id is None or set_number is None or value is None:
                raise ValueError("Each set needs dosage_id, set and value.")
            if set_number < 1:
                raise ValueError("set must be 1 or more.")
            rows.append((dosage_id, set_number, value))
    except ValueError as error:
        return _error(400, "bad_value", str(error))

    dosages = {d.dosage_id: d for d in patient.dosages}
    if any(dosage_id not in dosages for dosage_id, _, _ in rows):
        return _error(400, "bad_dosage", "That exercise isn't in this patient's plan.")

    logged = updated = skipped = 0
    for dosage_id, set_number, value in rows:
        if value == 0:
            skipped += 1
            continue
        dosage = dosages[dosage_id]
        if dosage.exercise_type == "strength":
            reps, load_lb, minutes = value, dosage.load_lb, None
        elif dosage.exercise_type == "cardio":
            reps, load_lb, minutes = None, None, value
        else:  # mobility: just "done"
            reps = load_lb = minutes = None
        _, was_update = _save_set(patient, dosage, set_number, reps, load_lb, minutes)
        if was_update:
            updated += 1
        else:
            logged += 1
    db.session.commit()
    return jsonify(ok=True, logged=logged, updated=updated, skipped=skipped)
