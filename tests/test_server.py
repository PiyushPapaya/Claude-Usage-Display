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


# ---------------- token auth ----------------
def test_token_ok(monkeypatch):
    # No token configured -> everything allowed.
    monkeypatch.setattr(server, "USAGE_TOKEN", "")
    with server.app.test_request_context("/usage"):
        assert server._token_ok() is True

    monkeypatch.setattr(server, "USAGE_TOKEN", "secret")
    lan = {"REMOTE_ADDR": "192.168.1.9"}

    # LAN caller with no/wrong token -> denied.
    with server.app.test_request_context("/usage", environ_base=lan):
        assert server._token_ok() is False
    with server.app.test_request_context("/usage", headers={"X-Usage-Token": "nope"}, environ_base=lan):
        assert server._token_ok() is False

    # Correct token via header or query param -> allowed.
    with server.app.test_request_context("/usage", headers={"X-Usage-Token": "secret"}, environ_base=lan):
        assert server._token_ok() is True
    with server.app.test_request_context("/usage?token=secret", environ_base=lan):
        assert server._token_ok() is True

    # Loopback is always exempt, even with a token set.
    with server.app.test_request_context("/usage", environ_base={"REMOTE_ADDR": "127.0.0.1"}):
        assert server._token_ok() is True


# ---------------- CORS origin gating ----------------
def test_host_is_private():
    for good in ("localhost", "::1", "nas.local", "127.0.0.1",
                 "10.1.2.3", "192.168.1.50", "172.16.0.1", "172.31.255.9"):
        assert server._host_is_private(good) is True, good
    for bad in ("evil.com", "8.8.8.8", "172.15.0.1", "172.32.0.1", "169.254.1.1", ""):
        assert server._host_is_private(bad) is False, bad


def test_origin_allowed(monkeypatch):
    monkeypatch.setattr(server, "ALLOW_FILE_ORIGIN", True)
    assert server._origin_allowed("http://192.168.1.50:8080") is True
    assert server._origin_allowed("http://localhost:8080") is True
    assert server._origin_allowed("null") is True
    # Public origins and junk are rejected.
    assert server._origin_allowed("https://evil.com") is False
    assert server._origin_allowed("http://8.8.8.8") is False
    assert server._origin_allowed("") is False
    assert server._origin_allowed(None) is False
    # file:// can be turned off.
    monkeypatch.setattr(server, "ALLOW_FILE_ORIGIN", False)
    assert server._origin_allowed("null") is False


def test_cors_header_only_for_trusted_origin():
    c = server.app.test_client()
    # Trusted origin is reflected back exactly (never "*").
    r = c.get("/usage", headers={"Origin": "http://192.168.1.50:8080"})
    assert r.headers.get("Access-Control-Allow-Origin") == "http://192.168.1.50:8080"
    assert "Origin" in r.headers.get("Vary", "")
    # Public origin gets no CORS header, so the browser blocks the read.
    r = c.get("/usage", headers={"Origin": "https://evil.com"})
    assert r.headers.get("Access-Control-Allow-Origin") is None
    # Same-origin (no Origin header) needs no CORS header.
    r = c.get("/usage")
    assert r.headers.get("Access-Control-Allow-Origin") is None


# ---------------- locale resolution ----------------
def test_weekday_resolution_env(monkeypatch):
    monkeypatch.setenv("DISPLAY_LOCALE", "de")
    monkeypatch.delenv("WEEKDAY_LABELS", raising=False)
    assert server._resolve_weekdays()[0] == "Mo"

    monkeypatch.setenv("DISPLAY_LOCALE", "en")
    assert server._resolve_weekdays()[0] == "Mon"

    monkeypatch.setenv("WEEKDAY_LABELS", "L,M,X,J,V,S,D")
    assert server._resolve_weekdays() == ["L", "M", "X", "J", "V", "S", "D"]
