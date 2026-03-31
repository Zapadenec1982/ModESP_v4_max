# ModESP v4 — Project Guide for Claude Code

**Version:** 1.0  
**Updated:** 2026-03-31  
**Project:** Industrial ESP32 refrigeration controller (replaces Danfoss/Dixell with open firmware)

---

## Quick Identity

| Attribute | Value |
|-----------|-------|
| **Domain** | Industrial refrigeration control (cold rooms, freezers, display cases) |
| **Target MCU** | ESP32-WROOM-32 (4 MB flash, dual-core 240 MHz) |
| **Framework** | ESP-IDF v5.5, FreeRTOS, C++17 |
| **Architecture** | Manifest-driven (JSON → Python generator → UI + C++ headers) |
| **Status** | Production release v1.0.1 (verified on KC868-A6 + real equipment) |
| **Code Comments** | Ukrainian; Doxygen headers in English |
| **Build** | CMake + ESP-IDF, PowerShell script on Windows |
| **Tests** | 491 total: 310 pytest (Python) + 181 doctest (C++ host) |

---

## What This Project Does

ModESP is a complete firmware framework for commercial refrigeration controllers. It replaces expensive proprietary controllers ($200–500) with a $4 ESP32 microcontroller while maintaining industrial reliability.

**Key capabilities:**
- Temperature control via thermostat (4-state FSM with asymmetric differential)
- 7-phase defrost FSM (natural, electric, hot-gas heating)
- 10 independent alarm monitors with 2-level escalation
- MQTT with TLS + Home Assistant Auto-Discovery
- Embedded Svelte 4 WebUI (83 KB gzipped, 4 languages, dark/light theme)
- DataLogger: 6-channel temperature + 18 event types, LittleFS storage
- Multi-zone support (2+ temperature zones)
- Electronic expansion valve (EEV) control with PI algorithm
- Modbus RTU slave for legacy integrations
- Over-the-air firmware updates with rollback

**Hardware abstraction:** Same firmware binary runs on different boards (dev PCB vs KC868-A6) by swapping `board.json` and `bindings.json` files.

---

## System Architecture

```
                    ┌──────────────────────────────────────────────┐
                    │              ESP32 (FreeRTOS)                │
                    │                                              │
  ┌──────────┐     │  ┌──────────┐  ┌───────────┐  ┌──────────┐  │
  │ DS18B20  │◄───►│  │Equipment │  │Thermostat │  │ Defrost  │  │
  │ NTC      │     │  │ Manager  │  │  4-state  │  │ 7-phase  │  │
  │ ADC      │     │  │  (HAL)   │  │   FSM     │  │   FSM    │  │
  └──────────┘     │  └────┬─────┘  └─────┬─────┘  └────┬─────┘  │
                   │       │              │              │        │
  ┌──────────┐     │  ┌────▼──────────────▼──────────────▼────┐   │
  │  Relay   │◄───►│  │         SharedState (231 keys)       │   │
  │  PCF8574 │     │  │  etl::unordered_map, thread-safe     │   │
  │  GPIO    │     │  └────┬──────────┬──────────┬────────────┘   │
  └──────────┘     │       │          │          │                │
                   │  ┌────▼────┐ ┌───▼────┐ ┌──▼──────┐       │
                   │  │Protection│ │DataLog │ │Lighting │  ...  │
                   │  │10 alarms │ │6-ch    │ │Schedules│       │
                   │  └─────────┘ └────────┘ └─────────┘       │
                   │                                              │
                   │  ┌──────────────────────────────────────┐    │
                   │  │          Services Layer               │    │
                   │  │ WiFi · MQTT · HTTP · WebSocket · OTA │    │
                   │  └───────┬──────────┬───────────┬───────┘    │
                   └──────────┼──────────┼───────────┼────────────┘
                              │          │           │
                         ┌────▼───┐ ┌────▼────┐ ┌───▼────────┐
                         │  MQTT  │ │ Browser │ │ ModESP     │
                         │ Broker │ │ (WebUI) │ │ Cloud /    │
                         │ (TLS)  │ │ Svelte  │ │ AWS IoT    │
                         └────────┘ └─────────┘ └────────────┘
```

### Core Components

**SharedState**
- Central key-value store: `etl::unordered_map<StateKey, StateValue, 158>`
- Thread-safe with FreeRTOS mutex
- No heap allocation (fixed capacity at compile time)
- Versioned for WebSocket delta broadcasts

**Equipment Manager** (Priority = CRITICAL)
- Only module with direct HAL access
- Reads sensors via drivers (EMA filter, per-zone)
- Arbitrates requests from Thermostat/Defrost/Protection (per-zone OR aggregation)
- Applies outputs to relays (per-zone defrost relay, evap fan, EEV)
- Enforces interlocks (electric defrost relay ↔ compressor never both ON)
- Anti-short-cycle: output-level 180s OFF / 120s ON (independent of thermostat)
- Condenser fan head pressure control: temperature-based hysteresis (Mode 1) or follows compressor (Mode 0)
- Head pressure recovery: pauses hot gas defrost when cond_temp < low_limit, resumes after 3 min or recovery
- Multi-zone: natural defrost blocked in 2+ zone systems (fallback to electric)

**Business Modules**
- Thermostat (Priority = NORMAL): 4-state FSM, night setback, safety run
- Defrost (Priority = NORMAL): 7-phase FSM, 3 types (natural/electric/hot gas), 6 initiation modes
- Protection (Priority = HIGH): 10 independent alarm monitors, CompressorTracker, 2-level escalation
- DataLogger (Priority = LOW): 6-channel recorder, LittleFS, streaming API
- EEV (Priority = NORMAL): velocity-form PI, MOP/LOP/Low SH protection, 4 valve drivers, 23 refrigerants
- Lighting (Priority = LOW): Schedule-based control
- Equipment: as above

**Drivers** (11 types)
- **Sensors:** DS18B20 (OneWire), NTC (ADC thermistor), pressure ADC
- **Actuators:** Relay (GPIO), PCF8574 relay/input (I2C), EEV (analog, stepper, PCF8574+stepper, AKV pulse)
- **Inputs:** Digital input (debounced)

---

## Project Structure (Detailed)

```
ModESP_v4_max/
├── README.md                  # Feature list + metrics
├── ARCHITECTURE.md            # System deep-dive
├── CMakeLists.txt            # ESP-IDF root (auto-detects board from sdkconfig)
├── project.json              # Active modules list + zone config
├── sdkconfig.defaults        # ESP-IDF defaults (WiFi, OTA, etc.)
├── partitions.csv            # Flash layout (dual OTA + LittleFS)
│
├── main/
│   └── main.cpp              # Boot sequence (3-phase init) + main loop
│
├── components/               # ESP-IDF components (7)
│   ├── modesp_core/          # BaseModule, ModuleManager, SharedState, types.h, message_types.h
│   ├── modesp_services/      # ErrorService, LoggerService, ConfigService, PersistService, WatchdogService, NVS
│   ├── modesp_hal/           # HAL, DriverManager, driver interfaces, message bus
│   ├── modesp_net/           # WiFiService, HttpService, WsService (23 endpoints)
│   ├── modesp_mqtt/          # MqttService (TLS, HA Discovery, delta-publish)
│   ├── modesp_modbus/        # Modbus RTU slave (284 input, 252 holding registers)
│   ├── modesp_refrigerant/   # 23 refrigerants, saturation props, dew point
│   ├── modesp_json/          # JSON helpers
│   └── jsmn/                 # Lightweight JSON parser
│
├── drivers/                  # Hardware drivers (11 types, each a component)
│   ├── ds18b20/              # OneWire temperature (SEARCH_ROM, MATCH_ROM, CRC8)
│   ├── ntc/                  # NTC thermistor (B-parameter, ADC)
│   ├── relay/                # GPIO relay (min on/off time protection)
│   ├── digital_input/        # GPIO input (50ms debounce)
│   ├── pcf8574_relay/        # I2C expander relays (KC868-A6)
│   ├── pcf8574_input/        # I2C expander inputs
│   ├── pressure_adc/         # Pressure sensor (0-10V → bar)
│   ├── eev_analog/           # EEV valve (analog 0-10V output)
│   ├── eev_stepper/          # EEV valve (stepper motor)
│   ├── eev_pcf8574_stepper/  # EEV valve (PCF8574 relay matrix for stepper)
│   └── akv_pulse/            # AKV pulse counter (frequency to RPM)
│
├── modules/                  # Business modules (7, each a component)
│   ├── equipment/            # HAL owner, arbitration, interlocks
│   ├── thermostat/           # 4-state FSM, night setback, safety run
│   ├── defrost/              # 7-phase FSM, 3 types, 4 initiations
│   ├── protection/           # 10 alarm monitors, CompressorTracker
│   ├── eev/                  # PI control loop, 23 refrigerants
│   ├── lighting/             # Schedule-based relay control
│   └── datalogger/           # 6-ch recorder, LittleFS, streaming API
│
├── boards/                   # Board configurations
│   ├── dev/                  # ESP32-DevKit (GPIO relays)
│   │   ├── board.json        # GPIO pin definitions
│   │   └── bindings.json     # Role → driver mappings
│   └── kc868a6/              # KC868-A6 (I2C PCF8574)
│       ├── board.json
│       └── bindings.json
│
├── data/
│   ├── board.json            # GENERATED from boards/*/board.json (selected via sdkconfig)
│   ├── bindings.json         # GENERATED from boards/*/bindings.json
│   ├── ui.json               # GENERATED by generate_ui.py (manifest → UI schema)
│   ├── i18n/                 # Language files (lazy-load from browser)
│   └── www/                  # Deployed WebUI (gzipped bundles)
│
├── generated/                # GENERATED by generate_ui.py (do NOT edit)
│   ├── state_meta.h          # State key metadata (read/write, type, range, persist)
│   ├── mqtt_topics.h         # MQTT pub/sub topic arrays
│   ├── display_screens.h     # LCD display menu (future)
│   └── features_config.h     # Feature flags based on bindings
│
├── webui/                    # Svelte 4 source
│   ├── src/
│   │   ├── App.svelte        # Root component
│   │   ├── pages/            # Dashboard, Settings, Logs, Network, System
│   │   ├── components/       # Shared widgets (GaugeCard, SettingGroup, etc.)
│   │   ├── i18n/             # Translation keys
│   │   ├── api.ts            # HTTP/WS client
│   │   └── store.ts          # Svelte store (state, UI config)
│   ├── package.json
│   ├── rollup.config.js
│   └── public/               # Static assets
│
├── tools/
│   ├── generate_ui.py        # Manifest → UI + C++ headers (~1700 lines)
│   └── tests/                # 310 pytest test cases
│       ├── conftest.py
│       ├── test_generator.py
│       ├── test_features.py
│       ├── test_modules.py
│       ├── test_hil.py       # Hardware-in-the-loop simulation
│       └── ... (8 test files total)
│
├── tests/
│   └── host/                 # 108 doctest C++ test cases (run on desktop)
│       ├── CMakeLists.txt
│       ├── test_shared_state.cpp
│       ├── test_thermostat.cpp
│       ├── test_defrost.cpp
│       ├── test_protection.cpp
│       └── ...
│
├── docs/
│   ├── FEATURES.md           # Complete feature matrix
│   ├── FEATURES_UA.md        # Ukrainian translation
│   ├── 05_cooling_defrost.md # Thermostat + Defrost specification
│   ├── 07_equipment.md       # Equipment Manager + Protection
│   ├── 10_manifest_standard.md
│   └── ... (schema docs)
│
├── .rules/                   # Agent team rules (7 roles, permissions, patterns)
│   ├── _index.md             # Identity, permissions summary
│   ├── core/
│   │   ├── constraints.md    # Zero heap, ETL, style, SSoT
│   │   ├── architecture.md   # System overview
│   │   └── permissions.md    # 3-tier access control
│   ├── context/
│   │   ├── roles.md          # 7 agent roles (Firmware Dev, WebUI Dev, etc.)
│   │   ├── references.md     # API endpoints, state keys, structure
│   │   └── memory.md         # Decisions, errors (cross-session tracking)
│   └── generation/
│       ├── patterns.md       # Code patterns with examples
│       └── output.md         # Commit convention, docs rules
│
├── .github/
│   └── copilot-instructions.md  # GitHub Copilot adapter
│
├── AGENTS.md                 # Universal hub for all agents
├── CLAUDE.md                 # THIS FILE (project guide)
├── LICENSE                   # PolyForm Noncommercial 1.0.0
└── .gitignore               # Excludes build/, generated/, secrets, etc.
```

---

## Key Design Decisions

### 1. Manifest-Driven Architecture (Single Source of Truth)

All module capabilities are declared in JSON manifests:

```
module/*/manifest.json ─┐
driver/*/manifest.json ─┼→ generate_ui.py ──→ ui.json
board.json             ─┤                    state_meta.h
bindings.json          ─┘                    mqtt_topics.h
                                             features_config.h
```

**Why:** Change a manifest, rebuild, and the UI, state metadata, MQTT topics, and feature flags all update consistently. Zero manual sync.

### 2. Hardware Abstraction via Board Config

Same firmware binary on different hardware:

```
board.json      → GPIO pin assignments (relay_1 = GPIO 14, etc.)
bindings.json   → Role mappings (role "compressor" → driver "relay" → GPIO "relay_1")
```

Switch `boards/dev/` to `boards/kc868a6/`, rebuild, same binary runs on I2C expanders.

### 3. Equipment Manager as HAL Gatekeeper

**No other module touches drivers directly.** Business modules (thermostat, defrost) publish *requests* to SharedState; Equipment arbitrates and applies outputs.

```
Protection.lockout ──┐
Defrost.req.*    ────┼→ Equipment arbitrator → apply_outputs() → drivers
Thermostat.req.* ────┘     │
                           ├── Arbitration: LOCKOUT > COMP_BLOCKED > DEFROST > THERMOSTAT
                           ├── Interlocks: electric defrost + compressor never both ON
                           ├── Anti-short-cycle: 180s min OFF, 120s min ON
                           └── Head pressure control: cond_fan by T_cond + defrost recovery pause
```

**Multi-zone arbitration:**
- Compressor: defrost priority (natural=OFF), OR for thermostat
- Cond fan: OR(defrost, thermostat) + safety rule (ON when comp ON)
- Evap fan: per-zone (defrost controls own zone fan)
- Defrost relay: per-zone (independent per zone)
- Natural defrost blocked in 2+ zone systems (shared compressor)
- Head pressure recovery: pauses hot gas defrost when cond_temp drops below limit

**Zone namespace:** Equipment reads `thermo_z1.req.*`, `defrost_z1.*`, `eev_z1.*` (not `thermostat.*`)

**Why:** Safety by architecture. A bug in thermostat cannot cause a short-cycle—the interlocks are structural.

### 4. Zero Heap Allocation in Hot Path

All control-loop code uses ETL (Embedded Template Library) containers with compile-time capacity bounds:

| Replace | With | Capacity |
|---------|------|----------|
| `std::string` | `etl::string<32>` | 32 chars |
| `std::vector` | `etl::vector<T, N>` | N elements |
| `std::unordered_map` | `etl::unordered_map<K, V, N>` | N entries |

**Why:** ESP32 has 320 KB RAM. Heap fragmentation in 24/7 industrial operation = crash.

### 5. SharedState as Event Bus

No direct module-to-module calls. All inter-module communication via a central key-value store:

```cpp
// Zone 1 thermostat publishes request (namespace thermo_z1):
state_->set("thermo_z1.req.compressor", true);
// Equipment reads via zone namespace table, applies arbitration, publishes:
state_->set("equipment.compressor", true);       // actual state
```

**InputBindings:** Cross-module key remapping (e.g., EEV reads `equipment.suction_bar` → resolved to `equipment.suction_bar_z1` via InputBinding). Zero-cost, compile-time.

**Why:** Decoupling. Modules are isolated; can be tested independently.

---

## Module Architecture

Every module extends `BaseModule`:

```cpp
class ThermostatModule : public BaseModule {
    bool on_init() override;           // Called once at startup
    void on_update(uint32_t dt_ms) override;  // Called every ~10ms
    void on_message(const etl::imessage& msg) override;  // Message bus
    void on_stop() override;           // Graceful shutdown
};
```

### Module Update Cycle

1. **Read** input from SharedState (sensor readings from Equipment)
2. **Business logic** (FSM, control algorithm)
3. **Write** request to SharedState (`thermostat.req.compressor`)
4. Equipment reads requests, arbitrates, writes actual state

### Update Priorities

```
CRITICAL(0) ─ Equipment (must read sensors first)
HIGH(1)     ─ Protection (must alarm before thermostat acts)
NORMAL(2)   ─ Thermostat, Defrost
LOW(3)      ─ DataLogger, Lighting
```

Modules at same priority run in registration order.

---

## Code Style & Conventions

### C++ / ESP-IDF Style

**Mandatory:**

```cpp
// Every .cpp file
static const char* TAG = "ModuleName";

// Logging (no printf)
ESP_LOGI(TAG, "Thermostat started");
ESP_LOGW(TAG, "Temperature rising at %.1f°C/min", rate);
ESP_LOGE(TAG, "Sensor failure");

// Zero heap — use ETL
etl::string<32> key = "thermostat.setpoint";
state_->set(key, 5.0f);

// NOT: std::string key = "thermostat." + name;  // heap!
```

**Comments:**
- Implementation details: Ukrainian
- Doxygen headers (`@file`, `@brief`, `@param`): English
- Build with `-Werror=all`: every warning = compile failure

### State Key Naming

```
<module>.<key>

Examples:
  equipment.air_temp          (float, °C)
  equipment.compressor        (bool, actual relay state)
  thermostat.req.compressor   (bool, requested state)
  thermostat.setpoint         (float, °C)
  thermostat.state            (int, enum: IDLE/COOLING/etc.)
  protection.lockout          (bool, alarms active)
  datalogger.temp_ch0         (float, last logged temperature)
```

**Rules:**
- Read-write int/float MUST have `min`, `max`, `step` in manifest
- Float rounding: `roundf(T * 100) / 100` (limits version bumps)
- Use `track_change=false` for diagnostics (timers, counters)

### Manifest Structure

```json
{
  "manifest_version": 1,
  "module": "thermostat",
  "description": "Temperature control with asymmetric differential",
  
  "requires": [
    {"role": "air_temp", "type": "sensor", "driver": ["ds18b20", "ntc"]},
    {"role": "compressor", "type": "actuator", "driver": ["relay", "pcf8574_relay"]}
  ],
  
  "state": {
    "thermostat.setpoint": {
      "type": "float",
      "access": "readwrite",
      "persist": true,
      "min": -30, "max": 10, "step": 0.1,
      "unit": "°C"
    },
    "thermostat.state": {
      "type": "int",
      "access": "read",
      "options": [
        {"value": 0, "label": "IDLE"},
        {"value": 1, "label": "COOLING"}
      ]
    }
  }
}
```

### Driver Interface Pattern

```cpp
class ISensorDriver {
public:
    virtual ~ISensorDriver() = default;
    virtual bool init() = 0;
    virtual void update(uint32_t dt_ms) = 0;
    virtual bool read(float& value) = 0;
    virtual bool is_healthy() const = 0;
    virtual const char* role() const = 0;
    virtual const char* type() const = 0;
    virtual uint32_t error_count() const = 0;
};

// Implement in driver:
bool DS18B20Driver::read(float& value) override {
    // Read latest temperature from cache (populated by update())
    if (!is_healthy()) return false;
    value = latest_temp_;
    return true;
}
```

---

## Build System

**Prerequisites:** ESP-IDF v5.5, Python 3.8+, Node.js 18+ (for WebUI)

### Build Firmware

```bash
# Windows PowerShell
powershell -ExecutionPolicy Bypass -File run_build.ps1

# OR manually:
idf.py build

# Select board via menuconfig
idf.py menuconfig
# → Config → ModESP → Board selection: dev / kc868a6

# Flash + monitor
idf.py -p COM15 flash monitor
```

### WebUI Build (optional — pre-built included)

```bash
cd webui
npm install
npm run build      # → dist/bundle.js
npm run deploy     # → copy to data/www/
```

### Tests

```bash
# Pytest (manifests, generator, API)
cd tools
python -m pytest tests/ -v

# C++ doctest (business logic on desktop)
cd tests/host
cmake -B build
cmake --build build
ctest --test-dir build --verbose
```

### Generator (automatic, runs at build)

```bash
# Manual run:
python tools/generate_ui.py \
    --project project.json \
    --modules-dir modules \
    --output-data data \
    --output-gen generated
```

---

## Debugging & Troubleshooting

### Common Issues

| Problem | Cause | Solution |
|---------|-------|----------|
| "Board not found" | `board.json` missing in active board dir | Check `sdkconfig` `CONFIG_MODESP_BOARD_DIR` |
| "State key not found" | Typo or key not in manifest | Add to module's `manifest.json`, rebuild |
| "Heap corruption" | `std::string` in on_update() | Replace with `etl::string<N>` |
| MQTT won't connect | TLS cert path wrong | Check modesp_mqtt.c `MQTT_BROKER_CERT_PEM` |
| WebSocket drops | 3 client limit | Close extra browser tabs |
| Firmware size > 1.5 MB | WebUI bundle too large | Check `webui/dist/bundle.js` size < 80 KB gzipped |

### Logging

Enable debug logging via `sdkconfig`:

```bash
idf.py menuconfig
→ Logging → Default log verbosity → Debug (or higher)
```

Then:
```bash
idf.py monitor
```

Serial output includes timing info:
```
I (1234) Equipment: Compressor ON at 1234567 ms
W (1235) Thermostat: Setpoint mismatch: 5.0 vs 4.9
```

### Memory Profiling

Check free heap:
```
state_->get("system.heap_free")
state_->get("system.heap_largest")
```

The system logs warnings if free heap < 16 KB.

---

## Testing Strategy

### Unit Tests (Doctest, C++ host)

Test business logic **without hardware** on desktop:

```cpp
// tests/host/test_thermostat.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("Thermostat: cool when T > SP + diff") {
    ThermostatModule thermo;
    MockSharedState state;
    thermo.set_state(&state);
    
    state.set("equipment.air_temp", 5.0f);     // T
    state.set("thermostat.setpoint", 0.0f);    // SP
    state.set("thermostat.differential", 2.0f); // diff
    
    thermo.on_update(100);  // 100 ms delta
    
    auto req = state.get("thermostat.req.compressor");
    REQUIRE(etl::get<bool>(*req) == true);
}
```

Run:
```bash
cd tests/host
cmake -B build && cmake --build build && ctest --test-dir build
```

### Integration Tests (Pytest)

Validate manifests, generator output, API contracts:

```python
# tools/tests/test_modules.py
def test_thermostat_manifest_has_setpoint(module_manifests):
    m = module_manifests["thermostat"]
    assert "thermostat.setpoint" in m["state"]
    assert m["state"]["thermostat.setpoint"]["access"] == "readwrite"
    assert m["state"]["thermostat.setpoint"]["persist"] == True
```

Run:
```bash
cd tools
python -m pytest tests/ -v
```

### Hardware-in-the-Loop (HIL)

Simulate firmware logic against mock drivers:

```python
# tools/tests/test_hil.py
def test_equipment_arbitration_protection_wins():
    sim = SimulatedController()
    sim.set_state("equipment.air_temp", 5.0)
    sim.set_state("protection.lockout", True)  # lockout active
    sim.set_state("thermostat.req.compressor", True)  # thermo wants comp
    
    sim.step(dt_ms=100)
    
    assert sim.get_relay("compressor") == False  # protection wins
```

---

## Connectivity & Protocols

### WiFi

- **Mode:** STA (client) with AP fallback (`ModESP-XXXX`) after 3 failures
- **AP→STA probe:** While in AP, periodically attempts STA with exponential backoff (30s–5min)
- **STA watchdog:** Restarts WiFi if disconnected >10 min cumulative
- **mDNS:** Hostname `modesp.local`

### MQTT

- **Broker:** Configurable (default: `modesp.com.ua`)
- **Port:** 8883 (TLS) or 1883 (plain)
- **Topic prefix:** `modesp/v1/{tenant}/{device_id}/`
- **Features:**
  - Home Assistant Auto-Discovery
  - Last Will & Testament (LWT)
  - Delta-publish cache (16 entries, only changed values)
  - Alarm republish every 5 min with retain flag
  - Heartbeat every 60s

### HTTP / WebSocket

- **HTTP:** 23 REST endpoints (state, settings, OTA, logs, WiFi, MQTT config, etc.)
- **WebSocket:** Real-time delta broadcasts, 1500ms interval, 20s heartbeat PING
- **Max clients:** 3 concurrent WebSocket connections
- **Compression:** Full state gzipped to ~3.5 KB; delta ~200 bytes

### AWS IoT Core (optional)

If compiled with `-DCONFIG_MODESP_CLOUD_AWS`:
- MQTT over TLS to AWS IoT broker
- Certificate-based auth
- Shadow sync (reported state)

---

## Firmware Partitions & Storage

```
Flash Layout (4 MB):
  0x009000  NVS           16 KB    Settings (DJB2-hashed keys)
  0x00D000  OTA Data       8 KB    Boot selector
  0x00F000  PHY Init       4 KB    RF calibration
  0x010000  OTA Slot 0   1.5 MB    Active firmware
  0x190000  OTA Slot 1   1.5 MB    Update target (rollback)
  0x310000  LittleFS    960 KB    WebUI + DataLogger
```

**Persistence:**
- **NVS:** Readwrite state keys (cached at boot, dirty-flag debounce 5s)
- **LittleFS:** WebUI gzipped bundles, DataLogger records (18KB RAM buffer flushed every 10 min)

---

## Common Development Tasks

### Add a New State Key

1. Edit module manifest (`modules/*/manifest.json`):
```json
"state": {
  "thermostat.hysteresis": {
    "type": "float",
    "access": "readwrite",
    "persist": true,
    "min": 0.1, "max": 5.0, "step": 0.1,
    "unit": "°C",
    "default": 2.0
  }
}
```

2. Rebuild (generator auto-creates metadata):
```bash
idf.py build
```

3. In module code:
```cpp
float hysteresis = state_->get_or("thermostat.hysteresis", 2.0f);
```

4. UI automatically shows setting card with slider.

### Add a New Driver

1. Create driver component (`drivers/my_sensor/`):
```
drivers/my_sensor/
  ├── CMakeLists.txt
  ├── idf_component.yml
  ├── include/
  │   └── my_sensor.h
  ├── src/
  │   └── my_sensor.cpp
  └── manifest.json
```

2. Implement `ISensorDriver` interface:
```cpp
class MySensor : public modesp::ISensorDriver {
    bool init() override;
    void update(uint32_t dt_ms) override;
    bool read(float& value) override;
    bool is_healthy() const override;
    // ...
};
```

3. Register in DriverManager (HAL):
```cpp
// In modesp_hal/src/driver_manager.cpp
if (type == "my_sensor") {
    driver = new MySensor();
    // ...
}
```

4. Add to manifest:
```json
{
  "manifest_version": 1,
  "driver": "my_sensor",
  "provides": [
    {"role": "my_input", "type": "sensor", "label": "My Sensor"}
  ]
}
```

5. Use in binding:
```json
{
  "hardware": "my_gpio",
  "driver": "my_sensor",
  "role": "my_input",
  "module": "equipment"
}
```

### Add a WebUI Page

1. Create Svelte page (`webui/src/pages/MyPage.svelte`):
```svelte
<script>
  import { state } from "../store";
</script>

<div class="page">
  <h1>My Page</h1>
  <p>Temperature: {$state["equipment.air_temp"]?.toFixed(1)}°C</p>
</div>

<style>
  .page {
    padding: 1rem;
  }
</style>
```

2. Add route in `App.svelte`:
```svelte
<Router>
  <Route path="/mypage" component={MyPage} />
</Router>
```

3. Add i18n keys in `data/i18n/en.json`:
```json
{
  "menu.mypage": "My Page",
  "mypage.title": "My Page"
}
```

4. Build:
```bash
cd webui
npm run build && npm run deploy
```

---

## Code Review Checklist

Before committing, verify:

- **Zero heap:** No `std::string`, `std::vector`, `new`, `malloc` in `on_update()` / `on_message()`
- **Logging:** Every error/warning uses `ESP_LOG*()` with TAG, not `printf`
- **Secrets:** No `.env`, API keys, tokens, certificates in code
- **State keys:** Follow `module.key` naming; read-write int/float have `min`/`max`/`step`
- **Thread-safe:** SharedState access through `state_->set()` / `get()`; not direct member access
- **Tests:** New features have doctest (C++) or pytest (Python)
- **Manifests:** Valid JSON, no typos in state key names
- **Generated:** DO NOT commit `generated/`, `data/ui.json`, `data/www/bundle.*`
- **Comments:** Implementation = Ukrainian, Doxygen = English

---

## Permissions & Boundaries

### Allowed (Edit Freely)

- `components/*/src/`, `components/*/include/`
- `modules/*/src/`, `modules/*/include/`, `modules/*/manifest.json`
- `drivers/*/src/`, `drivers/*/include/`, `drivers/*/manifest.json`
- `main/`, `webui/src/`, `tools/`, `tests/`, `docs/`, `data/i18n/`
- `data/board.json`, `data/bindings.json`, `boards/*/`

### Ask First

- `.rules/` — changes agent behavior globally
- `CLAUDE.md` — changes project guide
- `partitions.csv` — wrong partition = bricked device
- Root `CMakeLists.txt` — affects entire build
- Delete files — irreversible
- Push to remote — shared state

### Forbidden

- Commit secrets (keys, passwords, certs, `.env`)
- Edit `generated/` or `data/ui.json` (auto-generated, overwritten)
- Edit `data/www/bundle.*` (built from `webui/` source)
- Direct HAL access from non-Equipment modules
- Heap allocation in hot path

---

## Resources & References

### Documentation

- **README.md** — Feature matrix + metrics
- **ARCHITECTURE.md** — Deep-dive system design
- **AGENTS.md** — Universal hub for all agents
- **docs/FEATURES.md** — Complete feature list (English + Ukrainian)
- **docs/05_cooling_defrost.md** — Thermostat + Defrost specification
- **docs/07_equipment.md** — Equipment Manager + Protection
- **docs/10_manifest_standard.md** — Manifest schema v2
- **.rules/*** — Agent team rules (7 roles, patterns, permissions)

### External

- **ESP-IDF docs:** https://docs.espressif.com/projects/esp-idf/en/v5.5/
- **ETL (Embedded Template Library):** https://www.etlcpp.com/
- **Svelte 4:** https://svelte.dev/
- **FreeRTOS:** https://www.freertos.org/

### Key Files to Know

| Task | Read These |
|------|-----------|
| Understand architecture | `ARCHITECTURE.md`, `components/modesp_core/include/modesp/app.h` |
| Add C++ code | `.rules/generation/patterns.md`, existing module (e.g., `modules/thermostat/`) |
| Add state key | `modules/*/manifest.json`, `components/modesp_core/include/modesp/shared_state.h` |
| Add WebUI page | `webui/src/pages/`, `webui/src/api.ts` |
| Debug issue | `esp_log.h` patterns, `tests/host/` for unit tests |
| Release | `.rules/quality/eval.md` checklist |

---

## Quick Answers

**Q: How do I add a thermostat parameter?**  
A: Edit `modules/thermostat/manifest.json`, add key to `state`, rebuild. UI auto-generates setting card.

**Q: Why can't I use `std::string`?**  
A: ESP32 has 320 KB RAM. Heap fragmentation in 24/7 operation causes crashes. Use `etl::string<32>`.

**Q: How do I read a sensor value in a module?**  
A: Equipment publishes it to SharedState. Read via `state_->get_or("equipment.air_temp", 0.0f)`.

**Q: Can I call Equipment driver methods directly from my module?**  
A: **NO.** Only Equipment touches drivers. Publish requests to SharedState; Equipment arbitrates.

**Q: How do I test my code?**  
A: C++ logic → doctest in `tests/host/`. Manifests → pytest in `tools/tests/`. WebUI → manual browser check.

**Q: What should I commit?**  
A: Source code, tests, docs, manifests, board configs. **NOT:** `generated/`, secrets, large binaries.

**Q: How do I debug MQTT issues?**  
A: Check `mqtt_service.cpp` logs. Verify broker URL + cert. Test with `mosquitto_sub` on desktop.

**Q: Can I edit the WebUI bundle directly?**  
A: **NO.** Edit `webui/src/`, then build: `npm run build && npm run deploy`.

---

## Final Notes

- **This is production code.** 500+ tests, running on real equipment. Changes should be conservative and tested.
- **Team of agents.** 7 roles with specific scopes. Check `.rules/context/roles.md` for your role.
- **Manifest-driven.** JSON is single source of truth. Python generator makes it consistent across layers.
- **Hardware abstraction matters.** Test on dev board first (GPIO relays are simpler than I2C PCF8574).
- **Zero heap critical.** ESP32's 320 KB RAM can't afford fragmentation. ETL only.

---

**Last Updated:** 2026-03-31  
**Version:** 1.0  
**Maintainer:** Claude Code (Anthropic)
