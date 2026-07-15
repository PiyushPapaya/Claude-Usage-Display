---
name: Bug report
about: Something isn't working with the server or the display
title: "[bug] "
labels: bug
---

**What happened**
A clear description of the problem.

**Expected**
What you expected instead.

**Which part**
- [ ] Server (`server.py`)
- [ ] Firmware / display (`claude_usage_display`)
- [ ] Dashboard (web `/`)

**Environment**
- OS (for the server):
- Python version (`python --version`):
- Board / OLED (e.g. NodeMCU + SH1106):
- Relevant config / env vars (do **not** paste your token or credentials):

**Logs**
Server: relevant lines from `server.log`. Display: anything on screen / serial.
Check `/health` output too (it makes no API call).

**Steps to reproduce**
1.
2.
