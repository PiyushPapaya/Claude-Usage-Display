# Changelog

All notable changes to this project are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added
- Web dashboard at `/` — theme-aware (auto/light/dark), live bars, sparklines,
  per-model breakdown, and connection status, mirroring the OLED.
- Environment-variable configuration: `USAGE_HOST`, `USAGE_PORT`,
  `USAGE_CACHE_TTL`, `USAGE_CACHE_TTL_ERR`, `USAGE_TREND_MAX`,
  `CLAUDE_CREDENTIALS_PATH`, `USAGE_EXPOSE_RAW`.
- Locale support for reset labels: `DISPLAY_LOCALE` (en/de) and `WEEKDAY_LABELS`.
- Firmware: SSD1306 support alongside SH1106 (`OLED_SSD1306`), configurable I2C
  pins (`OLED_SDA`/`OLED_SCL`), display brightness (`OLED_CONTRAST`), and
  optional captive-portal WiFi setup (`USE_WIFI_MANAGER`).
- Cross-platform tooling: `run.sh`, systemd unit, Dockerfile, `.dockerignore`.
- GitHub Actions CI: ruff + pytest for the server, arduino-cli compile for the
  firmware.
- Test suite for the pure server helpers.
- `pyproject.toml`, `LICENSE` (MIT), `.gitattributes`, richer README.

### Changed
- Weekday reset labels now default to English (were hardcoded German).
- `/health` no longer blocks while an upstream fetch is in flight, and reports
  `last_token_refresh_ago_sec`.

### Security
- `/raw` (unfiltered upstream payload, includes account info) is disabled by
  default and localhost-only when enabled via `USAGE_EXPOSE_RAW=1`.

### Fixed
- Cache lock is no longer held across the network fetch, so `/health` stays
  instant and concurrent `/usage` requests don't serialize or stampede upstream.
