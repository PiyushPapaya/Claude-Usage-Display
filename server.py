"""
Claude Usage Display - Server (V7.1)

Data source: api.anthropic.com/api/oauth/usage (undocumented OAuth endpoint,
the exact one Claude Desktop / Claude Code uses for /usage).

V7.1 fixes / hardening over V7:
  - Safe logging under pythonw (no console): only attach a stderr handler when
    a real stderr stream exists, otherwise every log() raised+swallowed an
    AttributeError on Windows background launches.
  - Atomic history writes (temp file + os.replace) so a kill mid-write can no
    longer corrupt usage_history.json.
  - Trend history is recorded only on scheduled fetches, not on manual
    /refresh, keeping sample spacing even.
  - Flask dev server runs threaded so /health and /usage don't block each other
    while a fetch / token refresh is in flight.
  - Cleaner shutdown and clearer error surfaces; behaviour otherwise unchanged.

Earlier V7 features retained:
  - Per-model breakdown (Opus / Sonnet / future codenames) sent to ESP
  - Persistent trend history (survives restarts)
  - /refresh and /health endpoints
  - Auto token refresh via short-lived `claude` subprocess (closes itself)
  - File logging with rotation
  - Weekly burn-rate projection
  - Correct sesActive (keyed on resets_at, not utilization)
  - PID file so stop.bat can kill cleanly
"""

import json
import sys
import os
import time
import threading
import subprocess
import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path
from datetime import datetime, timezone
from collections import deque
import urllib.request
import urllib.error

from flask import Flask, jsonify

# ===================== Config =====================
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
USAGE_URL        = "https://api.anthropic.com/api/oauth/usage"
USER_AGENT       = "claude-code/1.0.0"

CACHE_TTL_OK     = 180     # 3 min between calls on success
CACHE_TTL_ERR    = 30      # faster retry after error
TREND_MAX        = 64      # ~3.2h of 5h-window samples at 3min cache

HERE         = Path(__file__).parent.resolve()
HISTORY_FILE = HERE / "usage_history.json"
LOG_FILE     = HERE / "server.log"
PID_FILE     = HERE / "server.pid"

TOKEN_REFRESH_THRESHOLD_S = 30 * 60   # trigger refresh when <30 min to expiry
TOKEN_REFRESH_COOLDOWN_S  = 30 * 60   # don't retry within 30 min

HOST = "0.0.0.0"   # the ESP needs LAN access; keep behind your router/firewall
PORT = 8080
# ==================================================

WEEKDAYS_DE = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]

# ----- logging -----
logger = logging.getLogger("claude-usage")
logger.setLevel(logging.INFO)

_fh = RotatingFileHandler(LOG_FILE, maxBytes=1_000_000, backupCount=3, encoding="utf-8")
_fh.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
logger.addHandler(_fh)

# pythonw.exe has no console, so sys.stderr is None. Attaching a StreamHandler
# to a None stream makes every emit raise (and silently swallow) an error.
if sys.stderr is not None:
    _sh = logging.StreamHandler(sys.stderr)
    _sh.setFormatter(logging.Formatter("[%(asctime)s] %(message)s", datefmt="%H:%M:%S"))
    logger.addHandler(_sh)


def log(msg: str) -> None:
    logger.info(msg)


# ===================== Persistent history =====================
_history_5h: "deque[int]" = deque(maxlen=TREND_MAX)
_history_7d: "deque[int]" = deque(maxlen=TREND_MAX)
_history_lock = threading.Lock()


def _load_history() -> None:
    if not HISTORY_FILE.exists():
        return
    try:
        data = json.loads(HISTORY_FILE.read_text(encoding="utf-8"))
        for v in data.get("h5", []):
            _history_5h.append(int(v))
        for v in data.get("h7", []):
            _history_7d.append(int(v))
        log(f"history loaded: 5h={len(_history_5h)} 7d={len(_history_7d)}")
    except Exception as e:
        log(f"history load failed: {e}")


def _save_history() -> None:
    """Atomic write: serialize to a temp file then os.replace() over the real
    one. A crash/kill can never leave a half-written (unparseable) JSON file."""
    try:
        payload = json.dumps({
            "h5": list(_history_5h),
            "h7": list(_history_7d),
            "saved_at": datetime.now(timezone.utc).isoformat(),
        })
        tmp = HISTORY_FILE.with_name(HISTORY_FILE.name + ".tmp")
        tmp.write_text(payload, encoding="utf-8")
        os.replace(tmp, HISTORY_FILE)
    except Exception as e:
        log(f"history save failed: {e}")


_load_history()


# ===================== Token handling =====================
_last_token_refresh = 0.0
_refresh_lock = threading.Lock()


def _read_creds():
    if not CREDENTIALS_PATH.exists():
        return None, "creds missing"
    try:
        data = json.loads(CREDENTIALS_PATH.read_text(encoding="utf-8"))
    except Exception:
        return None, "creds parse"
    return (data.get("claudeAiOauth") or data), None


def load_token():
    """Returns (token, error, seconds_until_expiry_or_None)."""
    oauth, err = _read_creds()
    if err:
        return None, err, None
    token = oauth.get("accessToken")
    if not token:
        return None, "no token", None
    exp = oauth.get("expiresAt")
    if exp and isinstance(exp, (int, float)) and exp > 0:
        if time.time() * 1000 > exp:
            return None, "token expired - run claude", 0
        return token, None, int(exp / 1000 - time.time())
    return token, None, None


def _maybe_refresh_token_async(seconds_left):
    """If token expires soon, spawn `claude` briefly to force a refresh."""
    global _last_token_refresh
    if seconds_left is None or seconds_left > TOKEN_REFRESH_THRESHOLD_S:
        return
    with _refresh_lock:
        now = time.time()
        if now - _last_token_refresh < TOKEN_REFRESH_COOLDOWN_S:
            return
        _last_token_refresh = now
    threading.Thread(target=_run_claude_refresh, daemon=True).start()


def _run_claude_refresh():
    """Run `claude -p hi` in the background to trigger a token refresh,
    then make sure it's gone. claude in print mode exits on its own."""
    log("token refresh: launching claude -p hi ...")
    try:
        kwargs = dict(
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if sys.platform == "win32":
            # No console window, detached so server can exit cleanly even if hung
            kwargs["creationflags"] = (
                subprocess.CREATE_NO_WINDOW | subprocess.DETACHED_PROCESS
            )
        proc = subprocess.Popen(["claude", "-p", "hi"], **kwargs)
        try:
            proc.wait(timeout=25)
            log("token refresh: claude exited cleanly")
        except subprocess.TimeoutExpired:
            proc.kill()
            log("token refresh: timed out, killed")
    except FileNotFoundError:
        log("token refresh: `claude` not found in PATH - skipping")
    except Exception as e:
        log(f"token refresh: failed: {e}")


# ===================== API call =====================
def fetch_raw():
    """Returns (parsed_dict, error_str_or_None, seconds_left_or_None)."""
    token, err, seconds_left = load_token()
    if err:
        return None, err, seconds_left
    _maybe_refresh_token_async(seconds_left)

    req = urllib.request.Request(
        USAGE_URL,
        method="GET",
        headers={
            "Authorization":  f"Bearer {token}",
            "anthropic-beta": "oauth-2025-04-20",
            "User-Agent":     USER_AGENT,
            "Content-Type":   "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body), None, seconds_left
    except urllib.error.HTTPError as e:
        if e.code == 401:
            return None, "401 - run claude", seconds_left
        if e.code == 429:
            return None, "429 rate limited", seconds_left
        return None, f"http {e.code}", seconds_left
    except Exception as e:
        return None, f"net: {str(e)[:50]}", seconds_left


# ===================== Helpers =====================
def pct_from(obj):
    if not isinstance(obj, dict):
        return -1
    u = obj.get("utilization")
    if u is None:
        return -1
    try:
        return int(round(float(u)))
    except Exception:
        return -1


def status_word(pct):
    if pct < 0:    return "—"
    if pct >= 100: return "limit!"
    if pct >= 90:  return "careful"
    if pct >= 75:  return "watch"
    if pct >= 50:  return "steady"
    if pct >= 25:  return "easy"
    return "chill"


def reset_labels(iso_str):
    """ISO timestamp -> (relative_str, absolute_local_str, total_seconds)."""
    if not iso_str:
        return ("", "", 0)
    try:
        dt = datetime.fromisoformat(iso_str.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        now = datetime.now(timezone.utc)
        sec = max(0, int((dt - now).total_seconds()))
        d = sec // 86400
        h = (sec % 86400) // 3600
        m = (sec % 3600) // 60
        if   d > 0: rel = f"{d}d{h}h"
        elif h > 0: rel = f"{h}h{m:02d}m"
        else:       rel = f"{m}m"
        local = dt.astimezone()
        if sec > 86400:
            abs_lbl = f"{WEEKDAYS_DE[local.weekday()]} {local.strftime('%H:%M')}"
        else:
            abs_lbl = local.strftime("%H:%M")
        return (rel, abs_lbl, sec)
    except Exception:
        return ("", "", 0)


def _model_label(key):
    """seven_day_opus -> 'Opus', seven_day_tangelo -> 'Tangelo', etc."""
    if key.startswith("seven_day_"):
        return key[len("seven_day_"):].replace("_", " ").title()
    return key.replace("_", " ").title()


# ===================== Main compute =====================
def compute(record_history=True):
    raw, err, seconds_left = fetch_raw()
    if err:
        return {"ok": False, "error": err, "token_expires_in_sec": seconds_left}
    if not isinstance(raw, dict):
        return {"ok": False, "error": "bad response shape"}

    fh    = raw.get("five_hour")   or {}
    sd    = raw.get("seven_day")   or {}
    extra = raw.get("extra_usage") or {}

    fh_pct = pct_from(fh)
    sd_pct = pct_from(sd)
    fh_rel, fh_abs, fh_sec = reset_labels(fh.get("resets_at"))
    sd_rel, sd_abs, sd_sec = reset_labels(sd.get("resets_at"))

    # Record a trend sample only on scheduled fetches (not manual /refresh),
    # so the sparkline keeps a consistent time spacing.
    if record_history:
        with _history_lock:
            if fh_pct >= 0: _history_5h.append(fh_pct)
            if sd_pct >= 0: _history_7d.append(sd_pct)
            _save_history()

    # ------ per-model breakdown ------
    SKIP = {"five_hour", "seven_day", "extra_usage"}
    models = []
    for k, v in raw.items():
        if k in SKIP:                continue
        if not isinstance(v, dict):  continue
        if v.get("utilization") is None: continue
        p = pct_from(v)
        if p < 0: continue
        rel, abs_lbl, sec = reset_labels(v.get("resets_at"))
        models.append({
            "key":       k,
            "label":     _model_label(k),
            "pct":       p,
            "status":    status_word(p),
            "reset_rel": rel,
            "reset_abs": abs_lbl,
            "reset_sec": sec,
        })
    models.sort(key=lambda m: m["label"])

    # ------ session active flag (proper) ------
    ses_active = bool(fh.get("resets_at")) and fh_sec > 0

    # ------ extra usage ------
    extra_pct      = pct_from(extra)
    extra_used     = float(extra.get("used_credits") or 0)
    extra_limit    = float(extra.get("monthly_limit") or 0)
    extra_enabled  = bool(extra.get("is_enabled"))
    extra_currency = extra.get("currency") or ""
    extra_rel, extra_abs, extra_sec = reset_labels(extra.get("resets_at"))

    # ------ weekly burn-rate projection ------
    # Project end-of-window pct assuming current rate continues.
    weekly_projected = -1
    WEEKLY_WINDOW_S = 7 * 86400
    if sd_pct >= 0 and 0 < sd_sec < WEEKLY_WINDOW_S:
        elapsed = max(1, WEEKLY_WINDOW_S - sd_sec)
        weekly_projected = int(round(sd_pct * WEEKLY_WINDOW_S / elapsed))
        if weekly_projected > 999: weekly_projected = 999

    return {
        "ok": True,
        "session": {
            "pct":       fh_pct,
            "reset_rel": fh_rel,
            "reset_abs": fh_abs,
            "reset_sec": fh_sec,
            "status":    status_word(fh_pct),
            "active":    ses_active,
        },
        "weekly": {
            "pct":           sd_pct,
            "reset_rel":     sd_rel,
            "reset_abs":     sd_abs,
            "reset_sec":     sd_sec,
            "status":        status_word(sd_pct),
            "projected_pct": weekly_projected,
        },
        "models": models,
        "extra": {
            "enabled":   extra_enabled,
            "pct":       extra_pct,
            "used":      extra_used,
            "limit":     extra_limit,
            "currency":  extra_currency,
            "reset_rel": extra_rel,
            "reset_abs": extra_abs,
            "reset_sec": extra_sec,
        },
        "trend_5h": list(_history_5h),
        "trend_7d": list(_history_7d),
        "token_expires_in_sec": seconds_left,
    }


# ===================== Flask =====================
app = Flask(__name__)
_cache = {"d": None, "t": 0.0, "err": False}
_cache_lock = threading.Lock()


def _compute_cached(force=False, record_history=True):
    with _cache_lock:
        now = time.time()
        ttl = CACHE_TTL_ERR if _cache["err"] else CACHE_TTL_OK
        if force or _cache["d"] is None or now - _cache["t"] > ttl:
            d = compute(record_history=record_history)
            _cache["d"]   = d
            _cache["t"]   = now
            _cache["err"] = not d.get("ok", False)
        return _cache["d"]


@app.route("/usage")
def route_usage():
    return jsonify(_compute_cached())


@app.route("/refresh")
def route_refresh():
    log("manual /refresh requested")
    # Force a fresh fetch but don't distort the trend's even spacing.
    return jsonify(_compute_cached(force=True, record_history=False))


@app.route("/health")
def route_health():
    with _cache_lock:
        d   = _cache["d"]
        age = (time.time() - _cache["t"]) if d else None
    _, token_err, sec = load_token()
    return jsonify({
        "ok":                  bool(d and d.get("ok")),
        "cache_age_sec":       int(age) if age is not None else None,
        "token_error":         token_err,
        "token_expires_in_sec": sec,
        "history_5h_count":    len(_history_5h),
        "history_7d_count":    len(_history_7d),
        "last_error":          (d or {}).get("error"),
    })


@app.route("/raw")
def route_raw():
    # Debug only: returns the full upstream payload (may include account info).
    data, err, _ = fetch_raw()
    return jsonify({"ok": err is None, "error": err, "data": data})


@app.route("/")
def route_root():
    return (
        "claude-usage V7.1\n"
        "  GET /usage   - ESP-shaped JSON\n"
        "  GET /refresh - force a fresh fetch\n"
        "  GET /health  - status snapshot (no API call)\n"
        "  GET /raw     - raw upstream response (debug)\n"
    )


# ===================== Entrypoint =====================
def main():
    PID_FILE.write_text(str(os.getpid()))
    import atexit
    atexit.register(lambda: PID_FILE.unlink(missing_ok=True))

    log(f"server starting on :{PORT}")
    log(f"  log:     {LOG_FILE}")
    log(f"  pid:     {PID_FILE}")
    log(f"  history: {HISTORY_FILE}")
    # threaded=True so a slow upstream fetch can't block /health or a second
    # /usage poll. use_reloader=False so we don't fork a second PID.
    app.run(host=HOST, port=PORT, debug=False, use_reloader=False, threaded=True)


if __name__ == "__main__":
    main()