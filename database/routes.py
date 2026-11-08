"""Staff site: pages for viewing and editing patient data.

Registered in flaskapp.py with `app.register_blueprint(site)`.
"""
from datetime import date

from flask import Blueprint, flash, redirect, render_template, request, url_for

from extensions import db
from models import Patient, Session

site = Blueprint("site", __name__)


@site.get("/")
def patient_list():
    patients = db.session.execute(db.select(Patient).order_by(Patient.name)).scalars().all()
    return render_template("patients.html", patients=patients)


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
    return redirect(url_for("site.patient_detail", patient_id=patient.patient_id))


@site.get("/patients/<int:patient_id>")
def patient_detail(patient_id):
    patient = db.get_or_404(Patient, patient_id)
    sessions = sorted(patient.sessions, key=lambda s: s.date or date.min, reverse=True)
    return render_template(
        "patient.html",
        patient=patient,
        sessions=sessions,
        today=date.today().isoformat(),
    )


@site.post("/patients/<int:patient_id>/edit")
def edit_patient(patient_id):
    patient = db.get_or_404(Patient, patient_id)
    name = request.form.get("name", "").strip()
    if not name:
        flash("Name is required.")
    else:
        patient.name = name
        db.session.commit()
        flash("Saved.")
    return redirect(url_for("site.patient_detail", patient_id=patient_id))


@site.post("/patients/<int:patient_id>/sessions")
def add_session(patient_id):
    patient = db.get_or_404(Patient, patient_id)
    try:
        session_date = date.fromisoformat(request.form.get("date", ""))
    except ValueError:
        flash("Pick a valid date.")
        return redirect(url_for("site.patient_detail", patient_id=patient_id))

    db.session.add(Session(patient=patient, date=session_date, is_scheduled=True))
    db.session.commit()
    flash(f"Added session on {session_date}.")
    return redirect(url_for("site.patient_detail", patient_id=patient_id))
