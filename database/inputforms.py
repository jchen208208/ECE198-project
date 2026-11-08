from flask_wtf import FlaskForm

from wtforms import (
    StringField,
    IntegerField,
    FloatField
)

from wtforms.validators import Optional


class PushForm(FlaskForm):

    exercise_name = StringField(
        "Push Exercise Name"
    )

    target_reps = IntegerField(
        "Target Reps",
        validators=[Optional()]
    )

    weight_lbs = FloatField(
        "Weight (lbs)",
        validators=[Optional()]
    )