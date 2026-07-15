## What & why

Briefly, what does this change and why.

## Which part
- [ ] Server (`server.py`)
- [ ] Firmware / display (`claude_usage_display`)
- [ ] Dashboard (web `/`)
- [ ] Docs / tooling

## Checklist
- [ ] `ruff check server.py tests` passes
- [ ] `pytest` passes (added/updated tests where it made sense)
- [ ] Firmware still compiles (CI runs `arduino-cli`); tested on hardware if relevant
- [ ] Default behavior unchanged for existing users, or the change is documented
- [ ] Updated `CHANGELOG.md`
