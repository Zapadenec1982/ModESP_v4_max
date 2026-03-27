# Current Task

## Goal: Prepare for production release

**Priority:** Stability, documentation completeness, and code quality.

## Release criteria

- All 5 modules fully tested on KC868-A6 board
- WebUI polished: premium dark theme, responsive, UA/EN
- Documentation complete and matches code state
- All tests passing: host doctest + pytest
- No secrets in git history
- CHANGELOG.md updated with all changes since last release
- Clean build with zero warnings (`-Werror=all`)
- NVS persist works across reboots
- OTA update verified
- WiFi STA + AP fallback stable

## Current state (Phase 18+)

- 5 modules, 6 drivers — fully operational
- 126 state keys (63 STATE_META), 108 host C++ tests, 254+ pytest
- KC868-A6 board: I2C PCF8574 expanders (6 relays, 6 inputs)
- WebUI: 76KB gzipped, premium dark theme, UA/EN
- Protection: 10 monitors, 2-level escalation, all alarms logged
- MQTT: HA Auto-Discovery, TLS, LWT, delta-publish
- WiFi: STA watchdog, AP>STA probe with backoff

## Board for release

- KC868-A6 (I2C PCF8574 expanders)
- `boards/kc868a6/board.json` + `boards/kc868a6/bindings.json`
- Flash: `idf.py -p COM15 flash monitor`
