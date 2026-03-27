# ModESP v4 — Industrial Refrigeration Controller

## Commands

```bash
# Build firmware
powershell -ExecutionPolicy Bypass -File run_build.ps1

# Flash + monitor
idf.py -p COM15 flash monitor

# WebUI
cd webui && npm run build && npm run deploy

# Tests
cd tools && python -m pytest tests/ -v          # pytest
# Host tests: build + run from tests/host/build/

# Generator (auto-runs at build)
python tools/generate_ui.py
```

## Stack

- **Firmware:** ESP-IDF 5.5 / C++17 / ETL (Embedded Template Library)
- **Hardware:** ESP32-WROOM-32, 4MB flash, KC868-A6 board
- **WebUI:** Svelte 4 / Rollup / Dark theme / UA+EN i18n
- **Tests:** doctest (C++ host), pytest (Python)
- **Tools:** Python 3 (generator, validators)

## Project Shape

- 7 ESP-IDF components in `components/`
- 5 business modules in `modules/` (equipment, protection, thermostat, defrost, datalogger)
- 6 HAL drivers in `drivers/`
- Manifest-driven: `manifest.json` → Python generator → `generated/` headers + `data/ui.json`
- Svelte WebUI in `webui/src/` → gzipped to `data/www/`

## Code Style

- **Zero heap in hot path:** ETL containers only (etl::string, etl::vector), no std::string/new/malloc
- **Logging:** `ESP_LOGI/W/E/D(TAG, ...)` with `static const char* TAG`
- **State keys:** `module.key` format (e.g., `thermostat.temperature`)
- **Comments:** Ukrainian; Doxygen headers in English
- **Commits:** conventional (`feat(module):`, `fix(module):`) + Ukrainian body

## Boundaries

| | Actions |
|---|---|
| **Allowed** | Edit source code, tests, docs, manifests, board configs, i18n |
| **Ask First** | `.rules/`, `CLAUDE.md`, `partitions.csv`, root CMakeLists, delete files, push |
| **Forbidden** | Commit secrets, edit `generated/`, `data/ui.json`, `data/www/bundle.*` |

## Patterns

- Modules extend `BaseModule` (on_init, on_update, on_stop, on_message)
- Only Equipment accesses HAL drivers — others write requests to SharedState
- Arbitration: lockout > compressor_blocked > defrost > thermostat
- Interlocks: defrost_relay + compressor NEVER both ON
- SharedState: `state_->set("key", value)` / `state_->get_or("key", default)`

## Testing

- C++ business logic → host doctest in `tests/host/`
- Manifest/generator → pytest in `tools/tests/`
- WebUI → manual browser check
- Before commit → verify no secrets in `git diff --cached`

## Git

- Conventional commits, push after confirmation
- Never commit `.env`, API keys, tokens, certificates, private keys

→ Full rules: `.rules/`
