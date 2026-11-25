"""Patient charts, drawn with matplotlib and put into the page as inline SVG.

Inline (not <img>) so the chart text picks up the page font from style.css.
Uses matplotlib.figure.Figure directly instead of pyplot, so it's safe inside Flask.
"""
import io
import re
from html import escape

import matplotlib
from markupsafe import Markup
from matplotlib.figure import Figure
from matplotlib.patches import Rectangle

# Same values as the tokens at the top of static/style.css.
ACCENT = "#1F6B5A"
WARN = "#A86A12"
INK = "#1C2B2D"
MUTED = "#5E6B6D"
LINE = "#DCE2E0"


def _day_label(day):
    return f"{day:%b} {day.day}"


def _tooltip(row):
    label = f"{row.day:%a %b} {row.day.day}"
    if not row.attended:
        return f"{label}: missed (scheduled, nothing logged)"
    return f"{label}: {row.done} of {row.prescribed} planned sets logged ({row.percent}%)"


def completion_chart_svg(rows):
    """Bar per session: % of the plan logged. Missed scheduled sessions get a short ochre stub."""
    if not rows:
        return None

    with matplotlib.rc_context({"svg.fonttype": "none", "font.size": 9}):
        fig = Figure(figsize=(7.5, 2.3))
        fig.patch.set_alpha(0)
        ax = fig.add_axes((0.06, 0.14, 0.93, 0.8))
        ax.patch.set_alpha(0)

        positions = range(len(rows))
        latest = max((i for i, r in enumerate(rows) if r.attended), default=None)

        for i, row in enumerate(rows):
            if row.attended:
                ax.bar(i, row.percent, width=0.56, color=ACCENT, zorder=2)
                if i == latest:
                    ax.text(i, row.percent + 4, f"{row.percent}%", ha="center", va="bottom",
                            color=INK, fontsize=9, fontweight="bold")
            else:
                ax.bar(i, 3, width=0.56, color=WARN, zorder=2)
                ax.text(i, 7, "missed", ha="center", va="bottom", color=MUTED, fontsize=8)

            # Invisible full-height column: a bigger hover target that carries the tooltip.
            hit = Rectangle((i - 0.5, 0), 1, 112, facecolor=INK, alpha=0, linewidth=0, zorder=3)
            hit.set_gid(f"session-{i}")
            ax.add_patch(hit)

        ax.set_xlim(-0.6, len(rows) - 0.4)
        ax.set_ylim(0, 112)
        ax.set_yticks([0, 50, 100], ["0%", "50%", "100%"])
        ax.set_xticks(list(positions), [_day_label(r.day) for r in rows])
        ax.tick_params(length=0, colors=MUTED, labelsize=9, pad=6)
        ax.grid(axis="y", color=LINE, linewidth=0.8)
        ax.set_axisbelow(True)
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(LINE)

        buffer = io.StringIO()
        fig.savefig(buffer, format="svg", metadata={"Date": None})

    svg = buffer.getvalue()
    svg = svg[svg.index("<svg"):]  # drop the XML prolog and doctype
    # Let CSS size it: remove the fixed width/height on the root element only.
    svg = re.sub(r'<svg([^>]*?) width="[^"]*" height="[^"]*"', r"<svg\1", svg, count=1)
    attended = sum(r.attended for r in rows)
    svg = svg.replace(
        "<svg", f'<svg role="img" aria-label="Plan completion for the last {len(rows)} sessions, '
                f'{attended} attended"', 1)
    for i, row in enumerate(rows):
        svg = svg.replace(f'<g id="session-{i}">', f'<g id="session-{i}"><title>{escape(_tooltip(row))}</title>', 1)
    return Markup(svg)


def completion_summary(rows):
    """One plain sentence for above the chart, or None."""
    if not rows:
        return None
    scheduled = [r for r in rows if r.scheduled]
    attended = [r for r in rows if r.attended]
    parts = []
    if scheduled:
        came = sum(r.attended for r in scheduled)
        parts.append(f"Came to {came} of {len(scheduled)} scheduled sessions.")
    if attended:
        average = round(sum(r.percent for r in attended) / len(attended))
        parts.append(f"Logged {average}% of the plan on average.")
    return " ".join(parts)
