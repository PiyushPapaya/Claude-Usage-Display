# Contributing

Thanks for improving Claude-Token-OLED! This is a small two-part project (a
Flask server and an ESP8266 sketch), so the workflow is light.

## Server (`server.py`)

```bash
pip install -r requirements-dev.txt
ruff check server.py tests    # lint
pytest                        # tests
```

- Keep the pure helpers (label/percentage/reset-time/locale logic) covered by
  tests in `tests/`.
- The code uses aligned one-liner guards (`if x: return y`) on purpose — `ruff`
  is configured (in `pyproject.toml`) to allow them.
- Never log or return the OAuth token, and keep `/raw` gated behind
  `USAGE_EXPOSE_RAW`.

## Firmware (`claude_usage_display/`)

- Requires the ESP8266 core plus ArduinoJson and U8g2 (and WiFiManager only if
  you build the captive-portal path).
- CI compiles the sketch with `arduino-cli` for `esp8266:esp8266:nodemcuv2`. New
  compile-time options should default to the existing behavior so the default
  build stays green.
- Test on hardware when you can — CI only checks that it compiles.

## Pull requests

- Branch off `main`, keep changes focused, and update `CHANGELOG.md`.
- Make sure `ruff`, `pytest`, and the firmware compile all pass before opening
  the PR (CI runs all three).
