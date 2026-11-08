from rehab_app.extensions import db


class Patient(db.Model):
    __tablename__ = "patient_list"

    patient_id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(100))

    sessions = db.relationship(
        "Session",
        back_populates="patient"
    )


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