# ModESP v4 — Copilot Instructions

You are working on an industrial ESP32 refrigeration controller (ESP-IDF 5.5, C++17, ETL, Svelte 4).

## Critical Rules

1. **Zero heap in hot path:** Use `etl::string<N>`, `etl::vector<T,N>` — never `std::string`, `new`, `malloc` in `on_update()` / `on_message()`
2. **Never edit generated files:** `generated/`, `data/ui.json`, `data/www/bundle.*` — edit manifests instead
3. **Never commit secrets:** No passwords, API keys, tokens, or certificates in code
4. **Only Equipment touches HAL:** Business modules write requests to SharedState, Equipment arbitrates relay outputs
5. **State keys:** `module.key` format with min/max/step for readwrite floats/ints

## Style

- `static const char* TAG = "ModuleName";` in every .cpp
- Logging: `ESP_LOGI(TAG, ...)`, not printf
- Comments: Ukrainian; Doxygen: English
- Commits: `feat(module):` / `fix(module):` conventional format

## Architecture

- Modules extend `BaseModule` → `on_init`, `on_update`, `on_stop`, `on_message`
- Update order: Equipment(0) → Protection(1) → Thermostat(2) + Defrost(2)
- Arbitration: lockout > compressor_blocked > defrost > thermostat
- Interlock: defrost_relay + compressor NEVER both ON

## Full rules: `.rules/`
