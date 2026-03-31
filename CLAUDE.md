# ModESP v4 — Industrial Refrigeration Controller

## Overview

Modular ESP32 firmware framework for commercial/industrial refrigeration control.
Replaces proprietary Danfoss/Dixell PLCs with an open-source ESP32 platform.
Manages compressor cycling, defrost sequences, EEV control, alarm monitoring, and data logging.

## Quick Reference

```bash
# Build firmware (ESP-IDF)
idf.py build

# Flash + monitor
idf.py -p /dev/ttyUSB0 flash monitor

# WebUI build + deploy
cd webui && npm run build && npm run deploy

# Run Python generator (auto-runs at build)
python tools/generate_ui.py --project project.json --modules-dir modules --output-data data --output-gen generated

# Run pytest (generator/manifest tests)
cd tools && python -m pytest tests/ -v

# Host C++ tests (doctest)
cd tests/host && mkdir -p build && cd build && cmake .. && make && ./modesp_tests

# Rollback .rules/ if agent degradation
git checkout HEAD~1 -- .rules/
```

## Tech Stack

| Layer | Technology |
|-------|-----------|
| MCU | ESP32-WROOM-32 (Xtensa dual-core 240 MHz, 4 MB flash) |
| Framework | ESP-IDF v5.5, C++17 |
| Containers | ETL (Embedded Template Library) — zero-heap |
| Board | Kincony KC868-A6 (6 relays, 6 inputs via I2C PCF8574) |
| Web UI | Svelte 4, Rollup, dark/light theme, 4 languages (UK/EN/DE/PL) |
| Connectivity | WiFi STA/AP, HTTP REST (21 endpoints), WebSocket, MQTT+TLS, Modbus RTU |
| Tests | doctest (C++ host, 108 tests), pytest (Python, 310 tests) |
| Build | CMake (ESP-IDF), Python 3 code generator |

## Project Structure

```
ModESP_v4_max/
├── main/main.cpp              # Boot sequence (3-phase init), main loop
├── components/                # ESP-IDF components (core framework)
│   ├── modesp_core/           #   BaseModule, ModuleManager, SharedState, types
│   ├── modesp_services/       #   PersistService, ErrorService, WatchdogService, Config, NVS
│   ├── modesp_hal/            #   HAL abstraction, DriverManager
│   ├── modesp_net/            #   WiFiService, HttpService (21 endpoints), WsService
│   ├── modesp_mqtt/           #   MQTT client (TLS, HA Auto-Discovery)
│   ├── modesp_modbus/         #   Modbus RTU slave (284 input + 252 holding registers)
│   ├── modesp_modbus_master/  #   Modbus RTU master
│   ├── modesp_refrigerant/    #   23 refrigerants, Antoine equation, dew/bubble point
│   ├── modesp_aws/            #   AWS IoT Core (mTLS, Shadow, Jobs OTA)
│   ├── modesp_json/           #   JSON serialization helpers
│   └── jsmn/                  #   Lightweight JSON parser (header-only)
├── drivers/                   # HAL drivers (11 drivers)
│   ├── ds18b20/               #   OneWire temperature (MATCH_ROM, SEARCH_ROM)
│   ├── ntc/                   #   ADC thermistor (B-parameter)
│   ├── pressure_adc/          #   Ratiometric ADC (Carel SPKT 0.5-4.5V)
│   ├── digital_input/         #   GPIO input (50ms debounce)
│   ├── pcf8574_input/         #   I2C PCF8574 digital input
│   ├── relay/                 #   GPIO relay (min on/off time)
│   ├── pcf8574_relay/         #   I2C PCF8574 relay
│   ├── eev_stepper/           #   GPIO stepper (TMC2209/DRV8825/A4988)
│   ├── eev_pcf8574_stepper/   #   I2C PCF8574 H-bridge stepper (L298N)
│   ├── eev_analog/            #   DAC 0-10V via LM258 op-amp
│   └── akv_pulse/             #   PWM solenoid (Danfoss AKV, Emerson EX2)
├── modules/                   # Business modules (7 modules)
│   ├── equipment/             #   HAL owner, sensor reads, relay arbitration (P=0)
│   ├── protection/            #   10 alarm monitors, lockout/blocking (P=1)
│   ├── thermostat/            #   Temperature control, differential, night setback (P=2)
│   ├── defrost/               #   7-phase defrost FSM, 3 types (P=2)
│   ├── eev/                   #   Electronic expansion valve, PI controller (P=2)
│   ├── lighting/              #   Lighting control, 3 modes (P=2)
│   └── datalogger/            #   6-channel data recorder, LittleFS (P=3)
├── webui/                     # Svelte 4 SPA source
│   ├── src/                   #   Components, stores, i18n
│   └── rollup.config.js       #   Build config
├── tools/
│   ├── generate_ui.py         #   Manifest -> artifacts generator (~1700 lines)
│   └── tests/                 #   310 pytest tests + fixtures
├── tests/host/                # 108 doctest C++ test cases
├── data/
│   ├── board.json             #   Active board config (copied from boards/)
│   ├── bindings.json          #   Active driver bindings (copied from boards/)
│   ├── ui.json                #   GENERATED — do not edit
│   ├── i18n/                  #   System i18n packs (en, de, pl + manifest)
│   └── www/                   #   Deployed web assets (gzipped bundles)
├── boards/
│   ├── dev/                   #   Dev board (GPIO relays)
│   └── kc868a6/               #   Production KC868-A6 (I2C expanders)
├── generated/                 #   GENERATED C++ headers — do not edit
├── docs/                      #   Architecture docs (01-12), changelog, features
├── .rules/                    #   Agent rules and conventions (structured)
├── project.json               #   Active module list + zone config
├── partitions.csv             #   Flash layout: NVS 32K + OTA 2x1472K + LittleFS 960K
├── sdkconfig.defaults         #   ESP-IDF defaults (board, flash, TLS optimization)
├── CMakeLists.txt             #   Root build file (board selection, generator invocation)
└── ARCHITECTURE.md            #   Full system architecture documentation
```

## Architecture Overview

### Core Pattern: SharedState Hub

All inter-module communication flows through SharedState (126 keys, versioned, thread-safe).
Modules never call each other directly — they read/write state keys.

```
Equipment(P=0) → reads sensors, publishes temperatures, arbitrates relay outputs
Protection(P=1) → monitors 10 alarms, sets lockout/compressor_blocked
Thermostat(P=2) → cooling state machine, writes thermostat.req.compressor
Defrost(P=2) → 7-phase FSM, writes defrost.req.*
EEV(P=2) → PI controller, writes eev.req.position
DataLogger(P=3) → records 6 channels to LittleFS
```

### Arbitration Priority (Equipment enforces)

```
LOCKOUT (permanent) > COMPRESSOR_BLOCKED (temporary) > DEFROST > THERMOSTAT
```

**Critical interlock:** defrost_relay + compressor are NEVER active simultaneously.

### Manifest-Driven Code Generation

```
modules/*/manifest.json ──┐
drivers/*/manifest.json ──┼──► generate_ui.py ──► 5 artifacts:
boards/*/board.json ──────┤     ├── data/ui.json            (UI schema)
boards/*/bindings.json ───┘     ├── generated/state_meta.h   (constexpr metadata)
                                ├── generated/mqtt_topics.h   (pub/sub topics)
                                ├── generated/display_screens.h
                                └── generated/features_config.h
```

Change the manifest, rebuild — UI, state metadata, MQTT topics all update automatically.

### Three-Phase Boot (main.cpp)

1. System services (Error, Watchdog, Logger, Config, Persist) → `init_all()`
2. WiFi + HAL + Equipment + business modules → `init_all()`
3. HTTP + WebSocket + MQTT → `init_all()`

`init_all()` is idempotent — skips modules already initialized.

### Multi-Zone Support

1-2 independent zones with per-zone instances of Thermostat, Defrost, EEV, Protection.
Zone modules use `InputBindings` for automatic state key remapping (e.g., `equipment.air_temp` → `equipment.air_temp_z2`).

## Code Conventions

### C++ Style

```cpp
// Every .cpp file MUST have:
static const char* TAG = "ModuleName";

// Logging — always use ESP-IDF macros with TAG
ESP_LOGI(TAG, "Setpoint: %.1f°C", setpoint);
ESP_LOGW(TAG, "Sensor timeout");
ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));

// State access
float temp = state_->get_or("equipment.air_temp", 0.0f);
state_->set("thermostat.temperature", temp);
state_->set("system.uptime", uptime, /*track_change=*/false);  // no WS broadcast

// ETL containers only in hot path — NEVER std::string, std::vector, new, malloc
etl::string<32> key = "thermostat.setpoint";
etl::vector<float, 8> readings;
```

### Zero Heap in Hot Path (CRITICAL)

ESP32 has 320KB RAM. Heap fragmentation in 24/7 industrial operation causes crashes.

| NEVER in on_update()/on_message() | Use instead |
|---|---|
| `std::string`, `std::vector` | `etl::string<N>`, `etl::vector<T,N>` |
| `new`, `malloc`, `std::make_shared` | Stack allocation, `etl::optional` |
| `std::map`, `std::unordered_map` | `etl::unordered_map<K,V,N>` |
| `printf`, `std::cout` | `ESP_LOGI/W/E/D(TAG, ...)` |

### Module Pattern

All modules extend `BaseModule` with lifecycle methods:

```cpp
class MyModule : public BaseModule {
    void on_init() override;     // One-time setup
    void on_update() override;   // Called every main loop iteration
    void on_stop() override;     // Cleanup
    void on_message(const etl::imessage& msg) override;  // Message bus handler
};

// on_update() pattern:
void MyModule::on_update() {
    // 1. Read inputs from SharedState
    float air_temp = state_->get_or("equipment.air_temp", 0.0f);
    // 2. Business logic
    update_state_machine(air_temp);
    // 3. Write requests (Equipment arbitrates)
    state_->set("thermostat.req.compressor", compressor_request_);
    // 4. Write status
    state_->set("thermostat.state", static_cast<int32_t>(state_));
}
```

**Only Equipment accesses HAL drivers.** Business modules write requests to SharedState.

### State Keys

- Format: `<module>.<key>` (e.g., `thermostat.setpoint`, `equipment.air_temp`)
- Read-write float/int keys MUST have `min`, `max`, `step` (or `options` for enums)
- Persisted keys use DJB2 hash in NVS: `"s" + 7 hex chars`

### Comments & Language

- **Code comments:** Ukrainian
- **Doxygen headers:** English
- **Commit messages:** `feat(module):` / `fix(module):` prefix in English, body in Ukrainian
- **UI text:** 4 languages (UK/EN/DE/PL) via i18n JSON files

### Git Convention

```
feat(thermostat): add night setback mode

- Додано 4 режими нічного зсуву
- Налаштування зберігаються в NVS
```

Prefixes: `feat`, `fix`, `refactor`, `perf`, `docs`, `test`, `chore`, `release`

## Permissions

### Allowed — edit freely

- `components/*/src/`, `modules/*/src/`, `drivers/*/src/`, `main/`
- `components/*/include/`, `modules/*/include/`, `drivers/*/include/`
- `modules/*/manifest.json`, `drivers/*/manifest.json`
- `webui/src/`, `data/i18n/`, `tools/`, `tests/`, `docs/`, `README.md`
- `boards/`, `data/board.json`, `data/bindings.json`

### Ask First — confirm before editing

- `.rules/`, `CLAUDE.md` — changes agent behavior globally
- `partitions.csv` — wrong partition = bricked device
- Root `CMakeLists.txt` — breaks build for all components
- `sdkconfig*`, `Kconfig.projbuild` — affects all modules
- Deleting any file, creating new components/modules
- Pushing to remote

### Forbidden — NEVER do this

- **Edit generated files:** `generated/`, `data/ui.json`, `data/www/bundle.*`
- **Commit secrets:** passwords, API keys, tokens, certificates, `.env` files
- **Direct HAL from business modules:** only Equipment touches drivers
- **Heap allocation in hot path:** no `std::string`/`new`/`malloc` in `on_update()`/`on_message()`

## Testing

### Host C++ Tests (doctest)

```bash
cd tests/host && mkdir -p build && cd build && cmake .. && make && ./modesp_tests
```

108 test cases covering Thermostat, Defrost, Protection, Equipment, DataLogger business logic against mock SharedState and stub HAL.

### Python Generator Tests (pytest)

```bash
cd tools && python -m pytest tests/ -v
```

310 test cases covering manifest parsing, feature resolution, constraint propagation, UI schema generation, and 7 validation passes.

### Build Verification

Firmware compiles with `-Werror=all` — every warning is a build failure.

### Quality Checklist

- [ ] No heap allocation in hot path
- [ ] No edits to generated files
- [ ] No secrets in staged changes
- [ ] State keys follow `module.key` format
- [ ] RW keys have min/max/step or options
- [ ] Equipment arbitration order preserved
- [ ] Interlocks respected (defrost + compressor never both ON)
- [ ] Tests pass (pytest + doctest)
- [ ] Manifest changes → regenerate artifacts

## Key Files for Context

| When working on... | Read these |
|---|---|
| Any C++ code | `.rules/core/constraints.md`, `.rules/generation/patterns.md` |
| Architecture decisions | `ARCHITECTURE.md`, `.rules/core/architecture.md` |
| API / state keys | `.rules/context/references.md` |
| Manifests / generator | `tools/generate_ui.py`, `modules/*/manifest.json` |
| Commits / docs | `.rules/generation/output.md` |
| Code review | `.rules/quality/eval.md` |
| Board/hardware | `boards/kc868a6/board.json`, `boards/kc868a6/bindings.json` |
| WebUI | `webui/src/`, `data/i18n/` |

## Full Rules

Detailed rules, patterns, and examples are in `.rules/` (structured by core/context/generation/quality).
