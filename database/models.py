from datetime import date, datetime

from .extensions import db


class Patient(db.Model):
    __tablename__ = "patient_list"

    patient_id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(100))
    card_uid = db.Column(db.String(32), unique=True)  # RFID card ID; empty until a card is assigned

    sessions = db.relationship(
        "Session",
        back_populates="patient"
    )
    dosages = db.relationship("Dosage", back_populates="patient", order_by="Dosage.position")
    logs = db.relationship("ExerciseLog", back_populates="patient")


class Session(db.Model):
    __tablename__ = "sessions"

    session_id = db.Column(db.Integer, primary_key=True)
    date = db.Column(db.Date)
    is_active = db.Column(db.Boolean, default=False)
    is_scheduled = db.Column(db.Boolean, default=True)

    patient_id = db.Column(
        db.Integer,
        db.ForeignKey("patient_list.patient_id")
    )

    patient = db.relationship(
        "Patient",
        back_populates="sessions"
    )


# One row per exercise in a patient's plan. Staff edit these.
class Dosage(db.Model):
    __tablename__ = "dosages"

    dosage_id = db.Column(db.Integer, primary_key=True)
    patient_id = db.Column(db.Integer, db.ForeignKey("patient_list.patient_id"), nullable=False)
    position = db.Column(db.Integer, nullable=False, default=0)  # order in the plan
    exercise = db.Column(db.String(100), nullable=False)         # e.g. "Goblet squat"
    exercise_type = db.Column(db.String(20), nullable=False)     # "strength", "cardio" or "mobility"
    sets = db.Column(db.Integer)        # strength
    reps = db.Column(db.Integer)        # strength
    load_lb = db.Column(db.Float)       # strength
    minutes = db.Column(db.Integer)     # cardio
    is_active = db.Column(db.Boolean, default=True)      # False = swapped out (kept for history)
    created_on = db.Column(db.Date, default=date.today)  # for the "new exercises every 12 sessions" flag

    patient = db.relationship("Patient", back_populates="dosages")
    logs = db.relationship("ExerciseLog", back_populates="dosage")


# One row per set a patient logs at the station.
class ExerciseLog(db.Model):
    __tablename__ = "exercise_logs"

    log_id = db.Column(db.Integer, primary_key=True)
    patient_id = db.Column(db.Integer, db.ForeignKey("patient_list.patient_id"), nullable=False)
    dosage_id = db.Column(db.Integer, db.ForeignKey("dosages.dosage_id"), nullable=False)
    set_number = db.Column(db.Integer)
    reps = db.Column(db.Integer)
    load_lb = db.Column(db.Float)
    minutes = db.Column(db.Integer)
    logged_at = db.Column(db.DateTime, default=datetime.now)

    patient = db.relationship("Patient", back_populates="logs")
    dosage = db.relationship("Dosage", back_populates="logs")