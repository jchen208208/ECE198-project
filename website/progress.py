"""Patient progress for the staff site, worked out from what patients logged at the station.

A "session" is a day on which the patient logged anything at the station.

Flags follow CCCARE's rules from the "Dosages & Exercise Sessions" doc:
- Increase load: "Once they can complete 20 reps with proper form for two sessions in a row,
  their loads are increased." Shown for a strength exercise when, in each of the last 2 sessions
  it was done, every prescribed set reached 20+ reps at the current load. It clears by itself
  once staff raise the load, because the older logs at the lower load no longer count.
- New exercises due: "a set of new exercises is introduced after every 12 sessions."
  Shown for a strength exercise that has been in the plan for 12+ sessions.
"""
from collections import defaultdict
from dataclasses import dataclass

REPS_TO_PROGRESS = 20
SESSIONS_IN_A_ROW = 2
SESSIONS_PER_ROTATION = 12


@dataclass
class Flag:
    kind: str      # "increase_load" or "new_exercises"
    exercise: str
    message: str


def _logs_by_day(logs):
    days = defaultdict(list)
    for log in logs:
        days[log.logged_at.date()].append(log)
    return days


def _at_current_load(logged_load, current_load):
    if current_load is None:  # bodyweight exercise: any load counts
        return True
    return logged_load is not None and logged_load >= current_load


def _hit_target(day_logs, dosage):
    """Every prescribed set done with 20+ reps at the current load."""
    good_sets = {
        log.set_number for log in day_logs
        if (log.reps or 0) >= REPS_TO_PROGRESS and _at_current_load(log.load_lb, dosage.load_lb)
    }
    return len(good_sets) >= (dosage.sets or 1)


def patient_flags(patient):
    """All flags for one patient, in plan order."""
    session_days = set(_logs_by_day(patient.logs))
    plan = sorted(
        (d for d in patient.dosages if d.is_active and d.exercise_type == "strength"),
        key=lambda d: (d.position, d.dosage_id),
    )

    flags = []
    for dosage in plan:
        days = _logs_by_day(dosage.logs)
        recent = sorted(days)[-SESSIONS_IN_A_ROW:]
        if len(recent) == SESSIONS_IN_A_ROW and all(_hit_target(days[day], dosage) for day in recent):
            if dosage.load_lb is None:
                advice = "Consider adding load or a harder variation."
            else:
                advice = f"Consider raising the load from {dosage.load_lb:g} lb."
            flags.append(Flag(
                "increase_load", dosage.exercise,
                f"{REPS_TO_PROGRESS}+ reps on every set for {SESSIONS_IN_A_ROW} sessions in a row. {advice}",
            ))

        sessions_in_plan = sum(1 for day in session_days if dosage.created_on is None or day >= dosage.created_on)
        if sessions_in_plan >= SESSIONS_PER_ROTATION:
            flags.append(Flag(
                "new_exercises", dosage.exercise,
                f"In the plan for {sessions_in_plan} sessions. Time to swap in a new exercise.",
            ))
    return flags


def log_history(patient, max_sessions=10):
    """([(day, logs), ...] newest session first, total number of sessions)."""
    days = _logs_by_day(patient.logs)
    newest = sorted(days, reverse=True)[:max_sessions]
    history = [(day, sorted(days[day], key=lambda log: (log.logged_at, log.log_id))) for day in newest]
    return history, len(days)
