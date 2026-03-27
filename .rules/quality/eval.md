# Eval: Quality Criteria

## Good change checklist

- [ ] No heap allocation in hot path (on_update, on_message)
- [ ] No edits to generated files
- [ ] No secrets in staged changes
- [ ] State keys follow `module.key` format
- [ ] Readwrite keys have min/max/step or options
- [ ] Persist keys have correct type in manifest
- [ ] New features guarded by `has_feature()` if conditional
- [ ] Equipment arbitration order preserved (lockout > blocked > defrost > thermostat)
- [ ] Interlocks respected (defrost_relay + compressor never both ON)
- [ ] Tests pass: pytest + host doctest
- [ ] Manifest changes → regenerate (`python tools/generate_ui.py`)

## Red flags (reject or fix)

- `std::string`, `std::vector`, `new` in module code
- Direct HAL access from business module (only Equipment touches HAL)
- SharedState key without manifest declaration
- Missing `static const char* TAG` in new .cpp
- Modifying generated files directly
- `printf` or `std::cout` instead of ESP_LOGx
- Blocking calls (vTaskDelay, sleep) in on_update()
- Secrets (passwords, keys, tokens) in committed code

## Test requirements

- Business logic changes → add/update host doctest in `tests/host/`
- Manifest changes → verify pytest passes
- WebUI changes → manual check in browser (no automated UI tests)
- New HTTP endpoint → test with curl

## Release readiness criteria

### Firmware
- [ ] Clean boot without errors in serial log
- [ ] All sensors reading (no -327.68 values)
- [ ] Thermostat cycles compressor correctly
- [ ] Defrost cycle completes all phases
- [ ] Protection alarms trigger and clear properly
- [ ] NVS persist survives reboot
- [ ] OTA update completes successfully
- [ ] Build with zero warnings (`-Werror=all`)

### Network
- [ ] WiFi STA connects and reconnects after router restart
- [ ] AP fallback works when STA fails
- [ ] MQTT connects with TLS, publishes state
- [ ] WebSocket broadcasts delta updates
- [ ] HA Auto-Discovery creates entities

### WebUI
- [ ] Loads < 3s on local network
- [ ] All pages render without JS errors
- [ ] Settings persist through page reload
- [ ] Responsive on mobile (360px+)
- [ ] Dark theme consistent across all pages
- [ ] i18n: UA and EN complete

### Documentation
- [ ] CHANGELOG.md current
- [ ] README.md metrics match code
- [ ] All docs reflect actual features (no phantom docs)
- [ ] API endpoints documented in references
