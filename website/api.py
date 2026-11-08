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
import math
from datetime import date, datetime, time

from flask import Blueprint, jsonify, request

from database.extensions import db
from database.models import Dosage, ExerciseLog, Patient

from .routes import normalize_card

api = Blueprint("api", __name__, url_prefix="/api")


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

    # Same set already logged today (e.g. a retry)? Update it instead of adding a duplicate.
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
    db.session.commit()

    return jsonify(ok=True, log_id=log.log_id, set=set_number, updated=updated)
