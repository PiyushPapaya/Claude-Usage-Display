# Claude Token OLED

Shows your Claude usage (session and weekly limits) on a small OLED on your desk, so you don't have to type `/usage` in the terminal. A Python server pulls the numbers from the same OAuth endpoint Claude Code uses internally, and an ESP8266 with a 128×64 display polls that server and renders it.

Two parts that work together:

- `server.py` — Flask server on your machine. Reads your Claude OAuth token, queries `api.anthropic.com/api/oauth/usage`, and returns it as cleaned-up JSON.
- `claude_usage_display/` — the Arduino sketch for the ESP8266. Fetches that JSON every 30 seconds and draws it.

You need both. The ESP never talks to Anthropic directly, only to your server on the LAN.

## Running the server

Needs Python on your PATH and a logged-in `claude` (i.e. an existing `~/.claude/.credentials.json`).

```
pip install -r requirements.txt
python server.py
```

Then it's on `http://localhost:8080`. Quick check:

```
curl http://localhost:8080/health
```

On Windows you can use `start.bat` instead — it launches the server headless via `pythonw` (no console window), writes a `server.pid`, and then opens a `claude` window too. `stop.bat` cleans both up. If you don't want the `claude` window, just run `python server.py` directly.

### Endpoints

| Route | What |
|-------|------|
| `/usage` | The JSON the ESP consumes. Cached (3 min); this is what the display polls. |
| `/refresh` | Forces a fresh fetch without skewing the trend history. |
| `/health` | Status snapshot with no API call — cache age, token expiry, last error. |
| `/raw` | The unfiltered response from Anthropic. Debug only, can include account info. |

### Token refresh

The OAuth token expires regularly. When less than 30 minutes are left, the server briefly runs `claude -p hi` in the background — that triggers an internal token refresh and exits on its own. For this to work, `claude` has to be on the PATH. If it isn't, the server keeps running but will eventually show `401 - run claude` until you log in again manually.

## Flashing the display

Hardware: an ESP8266 (NodeMCU or similar) and an SH1106 OLED, 128×64, over I2C. Wiring as set in the sketch: `SDA = D2`, `SCL = D1`.

Arduino libraries:

- ESP8266 core (via Boards Manager)
- ArduinoJson
- U8g2

Before compiling, create a `secrets.h` next to the `.ino` — copy the example and fill it in:

```
cp claude_usage_display/secrets.h.example claude_usage_display/secrets.h
```

```cpp
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define SERVER_URL "http://192.168.1.42:8080/usage"
```

`SERVER_URL` is the LAN IP of the machine running `server.py` — not `localhost`, since the ESP is a different device. `secrets.h` is gitignored. Then flash to the ESP8266 from the Arduino IDE as usual.

## What ends up on the display

The pages rotate automatically every few seconds, with a small pixel-Claude animation on the transition:

- **SESSION** — 5-hour limit as a percentage, progress bar, time until reset
- **WEEK** — 7-day limit plus a rough projection to end of window (`end~%`)
- **MODELS** — per-model breakdown (Opus, Sonnet, …), only shown when present
- **TREND** — sparkline history of the last samples for 5h and 7d
- **EXTRA** — extra-usage credit, only when enabled

Bottom-left has a tiny status glyph: WiFi down, server unreachable, data stale (>10 min), or all good. On a fresh fetch you get a brief checkmark top-right.

## Notes

This is built around Windows — `start.bat`/`stop.bat` are batch files. The server itself (`server.py`) is platform-independent though and runs fine on Linux/macOS, you just start it by hand.

The usage endpoint isn't officially documented; it's the same one Claude Desktop and Claude Code use for `/usage`. So if Anthropic changes the format, the display can break until `server.py` is updated. The weekday labels (`Mo`, `Di`, …) are currently hardcoded to German.

The server binds to `0.0.0.0` so the ESP can reach it. Keep it behind your router/firewall — `/raw` returns whatever Anthropic sends, unfiltered.
