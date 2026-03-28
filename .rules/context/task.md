# Current Task

## Goal: Feature parity with CAREL MPXPRO + production release

**Priority:** Close critical feature gaps vs MPXPRO, then stability and release.
**Reference:** CAREL MPXPRO manual +0300160EN rel. 1.0 (2025-07-09)

## Phase 1 — MPXPRO Parity + Перевершення

### Session 1: Continuous Cycle + Early Defrost Termination ✅
- [x] **F1: Continuous Cycle** — 5-й стан FSM CONTINUOUS_CYCLE, suppress low_temp + continuous_run
- [x] **F2: Early Defrost Termination** — safety limit T_cabinet, early_term_count counter
- [x] **Protection suppress** — cc_suppress_active_ з bypass timer

### Session 2: Sensor Redundancy ✅
- [x] **F3: Backup Probe + Auto Offset** — air_temp_backup, EMA ro, drift alarm, failover

### Session 3: Multi-Zone Air ✅
- [x] **F4: Multi-Zone** — air_zone_1..4, 4 agg modes, zone_count auto-detect

## Phase 2 — Smart Defrost

- [ ] Pump Down (Phase::PUMP_DOWN, EEV=0% before defrost)
- [ ] Running Time defrost (initiation mode 4)
- [ ] Skip Defrost — MPXPRO algorithm (counter-based)
- [ ] Power Defrost (enhanced night defrost)

## Phase 3 — Extended

- [ ] Staggered Defrost, Defrost by ΔT
- [ ] EEV Modbus Registers, HACCP Events, Smooth Lines
- [ ] RS-485 Modbus Master (I/O expansion bus)

## Current state

- 7 modules (equipment, thermostat, defrost, protection, eev, lighting, datalogger), 9+ drivers
- EEV: PI controller (velocity form), 23 refrigerants, 4 valve driver types, MOP/LowSH protection
- Modbus RTU slave: 284 input + 252 holding registers, auto-sync 500ms
- Multi-zone: 2 independent zones (EEV, fans, pressure, defrost per zone)
- 126 state keys (63 STATE_META), 170 host TEST_CASEs, 310 pytest
- KC868-A6 board: I2C PCF8574 expanders (6 relays, 6 inputs)
- WebUI: 76KB gzipped, premium dark theme, 4 languages (UK/EN/DE/PL)
- i18n: per-module translations, lazy-load language packs, cycleLanguage()
- Protection: 10 monitors, 2-level escalation, all alarms logged
- MQTT: HA Auto-Discovery, TLS, LWT, delta-publish
- WiFi: STA watchdog, AP>STA probe with backoff

## Release criteria

- All 5 modules fully tested on KC868-A6 board
- Phase 1 features implemented and tested
- WebUI polished: premium dark theme, responsive, 4 languages
- Documentation complete and matches code state
- All tests passing: host doctest + pytest
- Clean build with zero warnings (`-Werror=all`)
- NVS persist works across reboots

## Board for release

- KC868-A6 (I2C PCF8574 expanders)
- `boards/kc868a6/board.json` + `boards/kc868a6/bindings.json`
- Flash: `idf.py -p COM15 flash monitor` <!-- VERIFY: COM port -->
