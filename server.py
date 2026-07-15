"""
Claude usage server.

Reads the local Claude OAuth token and queries
api.anthropic.com/api/oauth/usage - the same endpoint Claude Desktop and
Claude Code use for /usage. Reshapes the response into the compact JSON the
ESP8266 display expects and serves it over the LAN.
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

from flask import Flask, jsonify, request, Response

# ===================== Config =====================
# Most settings can be overridden with environment variables so you don't have
# to edit this file. See the README "Configuration" table for the full list.
def _env(name: str, default: str) -> str:
    return os.environ.get(name, default)


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


_default_creds = Path.home() / ".claude" / ".credentials.json"
CREDENTIALS_PATH = Path(_env("CLAUDE_CREDENTIALS_PATH", str(_default_creds)))
USAGE_URL        = "https://api.anthropic.com/api/oauth/usage"
USER_AGENT       = "claude-code/1.0.0"

CACHE_TTL_OK     = _env_int("USAGE_CACHE_TTL", 180)   # min seconds between upstream calls on success
CACHE_TTL_ERR    = _env_int("USAGE_CACHE_TTL_ERR", 30)  # retry sooner after an error
TREND_MAX        = _env_int("USAGE_TREND_MAX", 64)     # samples kept per trend series

HERE         = Path(__file__).parent.resolve()
HISTORY_FILE = HERE / "usage_history.json"
LOG_FILE     = HERE / "server.log"
PID_FILE     = HERE / "server.pid"

TOKEN_REFRESH_THRESHOLD_S = 30 * 60   # refresh when this little time is left
TOKEN_REFRESH_COOLDOWN_S  = 30 * 60   # don't try refreshing again within this

HOST = _env("USAGE_HOST", "0.0.0.0")   # ESP needs LAN access; keep this behind your firewall
PORT = _env_int("USAGE_PORT", 8080)

# The /raw route leaks the full, unfiltered upstream payload (account info and
# all), so it's off unless you explicitly opt in.
EXPOSE_RAW = _env("USAGE_EXPOSE_RAW", "0").lower() in ("1", "true", "yes", "on")

# Optional shared secret. When set, /usage and /refresh require it (via the
# X-Usage-Token header or a ?token= query param) so only your ESP — and anyone
# you hand the token to — can read usage off the LAN. Loopback is always exempt
# so the local dashboard and curl keep working. Empty = no auth (default).
USAGE_TOKEN = _env("USAGE_TOKEN", "").strip()
# ==================================================

# Weekday abbreviations used for reset labels more than a day out. English by
# default; set DISPLAY_LOCALE=de for German (Mo/Di/Mi...), or supply a custom
# comma-separated list of 7 labels (Mon-first) via WEEKDAY_LABELS.
_WEEKDAYS = {
    "en": ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"],
    "de": ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"],
}


def _resolve_weekdays() -> list:
    custom = os.environ.get("WEEKDAY_LABELS")
    if custom:
        parts = [p.strip() for p in custom.split(",")]
        if len(parts) == 7:
            return parts
    locale = _env("DISPLAY_LOCALE", "en").lower()
    return _WEEKDAYS.get(locale, _WEEKDAYS["en"])


WEEKDAYS = _resolve_weekdays()

logger = logging.getLogger("claude-usage")
logger.setLevel(logging.INFO)

_fh = RotatingFileHandler(LOG_FILE, maxBytes=1_000_000, backupCount=3, encoding="utf-8")
_fh.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
logger.addHandler(_fh)

# pythonw.exe has no console, so sys.stderr is None. Logging to a None stream
# raises on every emit, so only attach the console handler when there's a real
# stream to write to.
if sys.stderr is not None:
    _sh = logging.StreamHandler(sys.stderr)
    _sh.setFormatter(logging.Formatter("[%(asctime)s] %(message)s", datefmt="%H:%M:%S"))
    logger.addHandler(_sh)


def log(msg: str) -> None:
    logger.info(msg)


# ===================== Trend history =====================
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
    # Write to a temp file then os.replace() so a kill mid-write can't leave a
    # half-written, unparseable history file behind.
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
    """Spawn `claude` to force a token refresh when expiry is near."""
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
    # `claude -p hi` runs in print mode, refreshes the token as a side effect,
    # and exits on its own. We just wait for it (or kill it if it hangs).
    log("token refresh: launching claude -p hi ...")
    try:
        kwargs = dict(
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if sys.platform == "win32":
            # No window, and detached so we can exit even if claude hangs.
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
            abs_lbl = f"{WEEKDAYS[local.weekday()]} {local.strftime('%H:%M')}"
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

    # Only sample on the scheduled fetches, not manual /refresh, so the
    # sparkline keeps even time spacing.
    if record_history:
        with _history_lock:
            if fh_pct >= 0: _history_5h.append(fh_pct)
            if sd_pct >= 0: _history_7d.append(sd_pct)
            _save_history()

    # Every remaining dict with a utilization is a per-model entry.
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

    # A session is active when the 5h window has a reset time still ahead.
    ses_active = bool(fh.get("resets_at")) and fh_sec > 0

    extra_pct      = pct_from(extra)
    extra_used     = float(extra.get("used_credits") or 0)
    extra_limit    = float(extra.get("monthly_limit") or 0)
    extra_enabled  = bool(extra.get("is_enabled"))
    extra_currency = extra.get("currency") or ""
    extra_rel, extra_abs, extra_sec = reset_labels(extra.get("resets_at"))

    # Extrapolate the 7-day percentage to the end of the window, assuming the
    # current burn rate holds.
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


@app.after_request
def _cors(resp):
    # The dashboard may be opened as a local file (file://) or hosted on a
    # different origin than the server, in which case the browser treats
    # /usage as a cross-origin request. Allow it so the web page can read the
    # same data the OLED does. Access is still gated by USAGE_TOKEN when set.
    resp.headers["Access-Control-Allow-Origin"] = "*"
    resp.headers["Access-Control-Allow-Headers"] = "X-Usage-Token, Content-Type"
    resp.headers["Access-Control-Allow-Methods"] = "GET, OPTIONS"
    return resp


# Flask auto-answers the browser's CORS preflight (OPTIONS) for these GET
# routes without invoking the view, so the token gate is skipped for preflight
# and the after_request hook above attaches the CORS headers.

_cache = {"d": None, "t": 0.0, "err": False}
_cache_lock = threading.Lock()   # guards the cache dict only, never held across I/O
_fetch_lock = threading.Lock()   # serializes upstream fetches (no thundering herd)


def _cache_is_fresh(force):
    now = time.time()
    ttl = CACHE_TTL_ERR if _cache["err"] else CACHE_TTL_OK
    return _cache["d"] is not None and not force and now - _cache["t"] <= ttl


def _compute_cached(force=False, record_history=True):
    # Fast path: return cached data without touching the network or blocking.
    with _cache_lock:
        if _cache_is_fresh(force):
            return _cache["d"]

    # Slow path: one fetch at a time. Crucially we do NOT hold _cache_lock during
    # compute()'s network call, so /health and concurrent /usage stay responsive.
    with _fetch_lock:
        with _cache_lock:
            if _cache_is_fresh(force):
                return _cache["d"]   # another thread refreshed while we waited
        d = compute(record_history=record_history)
        with _cache_lock:
            _cache["d"]   = d
            _cache["t"]   = time.time()
            _cache["err"] = not d.get("ok", False)
            return _cache["d"]


def _token_ok():
    """True when the request may read usage data."""
    if not USAGE_TOKEN:
        return True
    if request.remote_addr in ("127.0.0.1", "::1", "localhost"):
        return True
    supplied = request.headers.get("X-Usage-Token") or request.args.get("token")
    return supplied == USAGE_TOKEN


@app.route("/usage")
def route_usage():
    if not _token_ok():
        return jsonify({"ok": False, "error": "unauthorized"}), 401
    return jsonify(_compute_cached())


@app.route("/refresh")
def route_refresh():
    if not _token_ok():
        return jsonify({"ok": False, "error": "unauthorized"}), 401
    log("manual /refresh requested")
    # Force a fetch, but don't record a sample - that would skew the spacing.
    return jsonify(_compute_cached(force=True, record_history=False))


@app.route("/health")
def route_health():
    with _cache_lock:
        d   = _cache["d"]
        age = (time.time() - _cache["t"]) if d else None
    _, token_err, sec = load_token()
    refresh_ago = int(time.time() - _last_token_refresh) if _last_token_refresh else None
    return jsonify({
        "ok":                  bool(d and d.get("ok")),
        "cache_age_sec":       int(age) if age is not None else None,
        "token_error":         token_err,
        "token_expires_in_sec": sec,
        "last_token_refresh_ago_sec": refresh_ago,
        "history_5h_count":    len(_history_5h),
        "history_7d_count":    len(_history_7d),
        "last_error":          (d or {}).get("error"),
    })


@app.route("/raw")
def route_raw():
    # Debug only - returns the full upstream payload, account info and all.
    # Disabled unless USAGE_EXPOSE_RAW is set, and even then only answered for
    # loopback callers so account data can't be scraped from the LAN.
    if not EXPOSE_RAW:
        return jsonify({"ok": False, "error": "raw disabled (set USAGE_EXPOSE_RAW=1)"}), 404
    if request.remote_addr not in ("127.0.0.1", "::1", "localhost"):
        return jsonify({"ok": False, "error": "raw is localhost-only"}), 403
    data, err, _ = fetch_raw()
    return jsonify({"ok": err is None, "error": err, "data": data})


DASHBOARD_FILE = HERE / "dashboard.html"


@app.route("/")
def route_root():
    # Serve the human-facing dashboard when it's present; fall back to a plain
    # endpoint listing so the server still self-documents without the file.
    try:
        return Response(DASHBOARD_FILE.read_text(encoding="utf-8"), mimetype="text/html")
    except OSError:
        return Response(
            "claude-usage\n"
            "  GET /        - dashboard (dashboard.html missing)\n"
            "  GET /usage   - ESP-shaped JSON\n"
            "  GET /refresh - force a fresh fetch\n"
            "  GET /health  - status snapshot (no API call)\n",
            mimetype="text/plain",
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
    # threaded so a slow upstream fetch can't block /health or another /usage
    # poll; no reloader so we don't end up with a second PID.
    app.run(host=HOST, port=PORT, debug=False, use_reloader=False, threaded=True)


if __name__ == "__main__":
    main()