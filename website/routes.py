"""Staff site: pages for viewing and editing patient data.

Registered in flaskapp.py with `app.register_blueprint(site)`.
"""
import math
from datetime import date

from flask import Blueprint, flash, redirect, render_template, request, url_for

from database.extensions import db
from database.models import Dosage, Patient, Session

from .progress import log_history, patient_flags

# This folder holds its own templates/ and static/ (CSS).
site = Blueprint("site", __name__, template_folder="templates", static_folder="static", static_url_path="/static")

EXERCISE_TYPES = ("strength", "cardio", "mobility")

# Suggestions for the "add exercise" box, from CCCARE's sample exercise list.
SAMPLE_EXERCISES = [
    # Mobility
    "Arm rainbows", "Standing hip opener/closer", "Airplane", "Thoracic spine rotations",
    "Wall angels", "Scapular squeezes",
    # Squat / lunge
    "Ball squats", "Goblet squats", "Step-ups", "Lunges", "Wall sits", "Box squats",
    # Push
    "Wall push-ups", "Push-ups", "Chest press", "Shoulder press",
    # Hinge
    "Glute bridges", "Romanian deadlifts", "Kettlebell swings",
    # Pull
    "Seated row", "Lat pulldown", "Resistance band rows", "Bicep curls",
    # Carry
    "Farmer's carry", "Suitcase carry",
    # Core
    "Plank", "Side plank", "Bird dog", "Dead bug",
    # Stretches
    "Hamstring stretch", "Calf stretch", "Chest doorway stretch", "Hip flexor stretch", "Cat-cow stretch",
    # Cardio
    "Treadmill", "Recumbent bike", "Elliptical",
]


# ---------- helpers ----------

def normalize_card(value):
    """'a1 b2:c3d4' -> 'A1B2C3D4'; blank -> None. The station API should use this too."""
    card = "".join(ch for ch in (value or "") if ch.isalnum()).upper()
    return card or None


def _number(value, cast, label):
    """Form text -> number (0 or more), or None if blank. Raises ValueError with a readable message."""
    text = (value or "").strip()
    if not text:
        return None
    try:
        number = cast(text)
    except ValueError:
        raise ValueError(f"{label} must be a number.") from None
    if not math.isfinite(number) or number < 0:
        raise ValueError(f"{label} must be 0 or more.")
    return number


def _dosage_numbers(form, exercise_type, suffix=""):
    """Read the numbers that apply to this exercise type. Columns that don't apply are cleared."""
    def read(field, cast, label):
        return _number(form.get(field + suffix), cast, label)

    numbers = {"sets": None, "reps": None, "load_lb": None, "minutes": None}
    if exercise_type == "strength":
        numbers["sets"] = read("sets", int, "Sets")
        numbers["reps"] = read("reps", int, "Reps")
        numbers["load_lb"] = read("load_lb", float, "Load")  # blank = bodyweight
        if not numbers["sets"] or not numbers["reps"]:
            raise ValueError("Strength exercises need sets and reps.")
    elif exercise_type == "cardio":
        numbers["minutes"] = read("minutes", int, "Minutes")
        if not numbers["minutes"]:
            raise ValueError("Cardio exercises need minutes.")
    # mobility: nothing to record, the patient just marks it done
    return numbers


def _back_to(patient_id, anchor=None):
    return redirect(url_for("site.patient_detail", patient_id=patient_id, _anchor=anchor))


# ---------- patients ----------

@site.get("/")
def patient_list():
    patients = db.session.execute(db.select(Patient).order_by(Patient.name)).scalars().all()
    flags_by_patient = {p.patient_id: patient_flags(p) for p in patients}
    return render_template("patients.html", patients=patients, flags_by_patient=flags_by_patient)


@site.post("/patients")
def add_patient():
    name = request.form.get("name", "").strip()
    if not name:
        flash("Name is required.")
        return redirect(url_for("site.patient_list"))

    patient = Patient(name=name)
    db.session.add(patient)
    db.session.commit()
    flash(f"Added {name}.")
    return _back_to(patient.patient_id)


@site.get("/patients/<int:patient_id>")
def patient_detail(patient_id):
    patient = db.get_or_404(Patient, patient_id)
    dosages = sorted((d for d in patient.dosages if d.is_active), key=lambda d: (d.position, d.dosage_id))
    sessions = sorted(patient.sessions, key=lambda s: s.date or date.min, reverse=True)
    history, session_count = log_history(patient)
    return render_template(
        "patient.html",
        patient=patient,
        dosages=dosages,
        flags=patient_flags(patient),
        history=history,
        session_count=session_count,
        sessions=sessions,
        sample_exercises=SAMPLE_EXERCISES,
        today=date.today().isoformat(),
    )


@site.post("/patients/<int:patient_id>/edit")
def edit_patient(patient_id):
    patient = db.get_or_404(Patient, patient_id)
    name = request.form.get("name", "").strip()
    card_uid = normalize_card(request.form.get("card_uid"))

    if not name:
        flash("Name is required.")
        return _back_to(patient_id)
    if card_uid:
        owner = db.session.execute(
            db.select(Patient).where(Patient.card_uid == card_uid, Patient.patient_id != patient_id)
        ).scalar_one_or_none()
        if owner:
            flash(f"Card {card_uid} already belongs to {owner.name}.")
            return _back_to(patient_id)

    patient.name = name
    patient.card_uid = card_uid
    db.session.commit()
    flash("Saved.")
    return _back_to(patient_id)


# ---------- exercise plan (dosages) ----------

@site.post("/patients/<int:patient_id>/dosages")
def add_dosage(patient_id):
    patient = db.get_or_404(Patient, patient_id)
    exercise = request.form.get("exercise", "").strip()
    exercise_type = request.form.get("exercise_type", "")

    if not exercise or exercise_type not in EXERCISE_TYPES:
        flash("Couldn't add exercise. Enter a name and pick a type.")
        return _back_to(patient_id, "plan")
    try:
        numbers = _dosage_numbers(request.form, exercise_type)
    except ValueError as error:
        flash(f"Couldn't add exercise. {error}")
        return _back_to(patient_id, "plan")

    last_position = max((d.position for d in patient.dosages), default=0)
    db.session.add(Dosage(
        patient=patient,
        exercise=exercise,
        exercise_type=exercise_type,
        position=last_position + 1,
        **numbers,
    ))
    db.session.commit()
    flash(f"Added {exercise}.")
    return _back_to(patient_id, "plan")


@site.post("/patients/<int:patient_id>/dosages/save")
def save_dosages(patient_id):
    """Save every row of the plan table at once. Nothing is saved if any row is invalid."""
    patient = db.get_or_404(Patient, patient_id)
    updates = []
    for dosage in patient.dosages:
        suffix = f"-{dosage.dosage_id}"
        if not dosage.is_active or f"position{suffix}" not in request.form:
            continue  # not shown in the table (e.g. added in another tab after this page loaded)
        try:
            position = _number(request.form.get(f"position{suffix}"), int, "Order")
            numbers = _dosage_numbers(request.form, dosage.exercise_type, suffix)
        except ValueError as error:
            flash(f"Not saved. {dosage.exercise}: {error}")
            return _back_to(patient_id, "plan")
        updates.append((dosage, position, numbers))

    for dosage, position, numbers in updates:
        if position is not None:
            dosage.position = position
        for column, value in numbers.items():
            setattr(dosage, column, value)
    db.session.commit()
    flash("Exercise plan saved.")
    return _back_to(patient_id, "plan")


@site.post("/dosages/<int:dosage_id>/remove")
def remove_dosage(dosage_id):
    """Take an exercise out of the plan. The row is kept (inactive) so old logs still make sense."""
    dosage = db.get_or_404(Dosage, dosage_id)
    dosage.is_active = False
    db.session.commit()
    flash(f"Removed {dosage.exercise} from the plan.")
    return _back_to(dosage.patient_id, "plan")


# ---------- sessions ----------

@site.post("/patients/<int:patient_id>/sessions")
def add_session(patient_id):
    patient = db.get_or_404(Patient, patient_id)
    try:
        session_date = date.fromisoformat(request.form.get("date", ""))
    except ValueError:
        flash("Pick a valid date.")
        return _back_to(patient_id, "sessions")

    db.session.add(Session(patient=patient, date=session_date, is_scheduled=True))
    db.session.commit()
    flash(f"Added session on {session_date}.")
    return _back_to(patient_id, "sessions")


# ---------- station simulator ----------

@site.get("/simulator")
def simulator():
    """A browser version of the ESP32 station. It calls the same API (website/api.py)."""
    patients_with_cards = db.session.execute(
        db.select(Patient).where(Patient.card_uid.is_not(None)).order_by(Patient.name)
    ).scalars().all()
    return render_template("simulator.html", patients=patients_with_cards)
