"""
Unit tests for the pure helpers in server.py.

These cover the data-shaping logic the ESP and dashboard depend on, without
touching the network or the Claude credentials. Run with:

    pip install -r requirements-dev.txt
    pytest
"""

import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

# Import server.py from the repo root regardless of where pytest is invoked.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import server  # noqa: E402


# ---------------- pct_from ----------------
def test_pct_from_rounds_and_ints():
    assert server.pct_from({"utilization": 42.6}) == 43
    assert server.pct_from({"utilization": 0}) == 0
    assert server.pct_from({"utilization": 99.4}) == 99


def test_pct_from_missing_or_bad():
    assert server.pct_from({}) == -1
    assert server.pct_from(None) == -1
    assert server.pct_from({"utilization": None}) == -1
    assert server.pct_from({"utilization": "nope"}) == -1


# ---------------- status_word ----------------
def test_status_word_bands():
    assert server.status_word(-1) == "—"
    assert server.status_word(0) == "chill"
    assert server.status_word(24) == "chill"
    assert server.status_word(25) == "easy"
    assert server.status_word(50) == "steady"
    assert server.status_word(75) == "watch"
    assert server.status_word(90) == "careful"
    assert server.status_word(100) == "limit!"
    assert server.status_word(150) == "limit!"


# ---------------- _model_label ----------------
def test_model_label_strips_prefix_and_titlecases():
    assert server._model_label("seven_day_opus") == "Opus"
    assert server._model_label("seven_day_claude_sonnet") == "Claude Sonnet"
    assert server._model_label("some_other_key") == "Some Other Key"


# ---------------- reset_labels ----------------
def test_reset_labels_empty():
    assert server.reset_labels("") == ("", "", 0)
    assert server.reset_labels(None) == ("", "", 0)


def test_reset_labels_hours_and_minutes():
    future = datetime.now(timezone.utc) + timedelta(hours=1, minutes=46, seconds=30)
    rel, abs_lbl, sec = server.reset_labels(future.isoformat())
    # Allow a one-minute slack for the sub-second floor between now() calls.
    assert rel in ("1h46m", "1h45m")
    assert sec > 6000
    assert abs_lbl  # local HH:MM present


def test_reset_labels_days_uses_weekday_label():
    future = datetime.now(timezone.utc) + timedelta(days=2, hours=3, seconds=30)
    rel, abs_lbl, sec = server.reset_labels(future.isoformat())
    assert rel in ("2d3h", "2d2h")
    # Beyond a day, the absolute label is prefixed with a weekday token.
    assert any(abs_lbl.startswith(w) for w in server.WEEKDAYS)


def test_reset_labels_past_clamps_to_zero():
    past = datetime.now(timezone.utc) - timedelta(hours=5)
    rel, abs_lbl, sec = server.reset_labels(past.isoformat())
    assert sec == 0
    assert rel == "0m"


def test_reset_labels_handles_z_suffix():
    future = (datetime.now(timezone.utc) + timedelta(minutes=30))
    iso_z = future.strftime("%Y-%m-%dT%H:%M:%SZ")
    rel, _, sec = server.reset_labels(iso_z)
    assert sec > 0
    assert rel.endswith("m")


# ---------------- weekly projection (via compute math) ----------------
def test_weekly_projection_extrapolates():
    # Re-derive the projection formula the way compute() does: at 50% of the
    # window elapsed with 10% used, the projected end is ~20%.
    WEEKLY_WINDOW_S = 7 * 86400
    sd_pct = 10
    sd_sec = WEEKLY_WINDOW_S // 2  # half the window still remaining
    elapsed = max(1, WEEKLY_WINDOW_S - sd_sec)
    projected = int(round(sd_pct * WEEKLY_WINDOW_S / elapsed))
    assert projected == 20


# ---------------- cache freshness ----------------
def test_cache_freshness(monkeypatch):
    import time as _t
    server._cache.update({"d": {"ok": True}, "t": _t.time(), "err": False})
    assert server._cache_is_fresh(force=False) is True
    # force always misses.
    assert server._cache_is_fresh(force=True) is False
    # An empty cache is never fresh.
    server._cache.update({"d": None, "t": 0.0, "err": False})
    assert server._cache_is_fresh(force=False) is False
    # Aged past the TTL -> stale.
    server._cache.update({"d": {"ok": True}, "t": _t.time() - server.CACHE_TTL_OK - 5, "err": False})
    assert server._cache_is_fresh(force=False) is False


# ---------------- locale resolution ----------------
def test_weekday_resolution_env(monkeypatch):
    monkeypatch.setenv("DISPLAY_LOCALE", "de")
    monkeypatch.delenv("WEEKDAY_LABELS", raising=False)
    assert server._resolve_weekdays()[0] == "Mo"

    monkeypatch.setenv("DISPLAY_LOCALE", "en")
    assert server._resolve_weekdays()[0] == "Mon"

    monkeypatch.setenv("WEEKDAY_LABELS", "L,M,X,J,V,S,D")
    assert server._resolve_weekdays() == ["L", "M", "X", "J", "V", "S", "D"]
