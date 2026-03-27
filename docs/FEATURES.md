# ModESP v4 — Firmware Features

**Modular embedded framework for commercial refrigeration controllers on ESP32, replacing proprietary Danfoss/Dixell hardware with open architecture.**

> Built with ESP-IDF 5.5, C++17, ETL (zero heap allocation), FreeRTOS — runs on ESP32-WROOM-32 with 4MB flash.

---

## At a Glance

| Metric | Value |
|--------|-------|
| State keys | 126 total (63 STATE_META entries) |
| MQTT topics | 50 publish, 62 subscribe |
| HTTP endpoints | 23 REST + OTA upload |
| Modules | 5 (equipment, thermostat, defrost, protection, datalogger) |
| Drivers | 6 (DS18B20, NTC, relay, digital input, PCF8574 relay, PCF8574 input) |
| Tests | 491 total (181 host C++ / 418 assertions + 310 pytest) |
| WebUI | 80KB gzipped (Svelte 4, dark/light theme) |
| Firmware binary | ~1.2MB, free heap 77–90KB operational |
| License | PolyForm Noncommercial 1.0.0 |

---

## 1. Manifest-Driven Architecture

All UI, state, MQTT topics, and C++ headers generated from JSON manifests — zero manual synchronization.

- **Single source of truth** — module and driver manifests define parameters, state keys, MQTT topics, UI layout, and persistence in one place
- **5 generated artifacts** — `ui.json`, state metadata headers, MQTT topic maps, parameter defaults, persistence keys
- **Code generation pipeline** — Python generator runs at build time; output committed to `generated/` and `data/`
- **Zero drift** — adding a parameter to a manifest automatically propagates to UI, MQTT, state engine, and NVS persistence
- **Board abstraction** — `board.json` + `bindings.json` map logical names (e.g., `compressor_relay`) to physical GPIO pins

---

## 2. Refrigeration Control

Industrial thermostat and defrost logic purpose-built for commercial refrigeration applications.

### Thermostat
- **4-state FSM** — startup, idle, cooling, safety_run
- **Asymmetric differential** — separate high/low differential values around setpoint for precise temperature band control
- **Night setback** — scheduled setpoint offset reduces energy consumption during off-hours
- **Safety run** — timed compressor cycling on sensor failure to prevent product loss

### Defrost
- **7-phase FSM** — idle, pre-drip, heating, drip, fan delay, post-drip, recovery
- **3 defrost types** — natural (off-cycle), electric heater, hot-gas
- **4 initiation modes** — timed interval, adaptive (based on evaporator frost accumulation), manual, external trigger
- **Module interaction** — defrost suspends thermostat cooling; equipment manager arbitrates relay conflicts

---

## 3. Equipment Manager

Hardware abstraction layer that decouples control logic from physical I/O.

- **Driver binding** — `bindings.json` maps logical actuator/sensor names to driver instances at boot
- **Sensor management** — DS18B20 (1-Wire), NTC (ADC) with configurable calibration offsets
- **Actuator management** — GPIO relay, PCF8574 I2C expander relay, with active-high/low configuration
- **Digital inputs** — door switch, external defrost trigger, alarm reset — with debouncing
- **Update order** — Equipment(0) → Protection(1) → Thermostat(2) + Defrost(2) — deterministic execution every cycle

---

## 4. Protection System

10 independent alarm monitors with 2-level escalation and compressor lifecycle tracking.

### Alarm Monitors

| Severity | Monitors |
|----------|----------|
| **Critical** | High temperature, Low temperature, Sensor failure (S1, S2) |
| **Warning** | Door open, Continuous run, Pulldown failure |
| **Info** | Rate-of-rise alarm, Short cycle, Rapid cycle |

### Escalation
- **Level 1: compressor_blocked** — short cycle or rapid cycle detected → compressor disabled for configurable cooldown
- **Level 2: lockout** — persistent fault or critical alarm → full compressor lockout until manual reset
- **CompressorTracker** — tracks on/off cycles, run duration, idle duration for short-cycle and rapid-cycle detection

### Arbitration Priority
- Protection LOCKOUT > compressor_blocked > Defrost active > Thermostat request

### Persistence
- **17 protection parameters** persisted to NVS — alarm thresholds, delay timers, escalation settings survive power cycles
- **38 state keys** — real-time alarm status, tracker counters, escalation state exposed via MQTT and WebUI

---

## 5. DataLogger

On-device telemetry recording with chart generation — no cloud dependency required.

- **6 channels** — air temperature, evaporator temperature, condenser temperature, setpoint, compressor state, defrost state
- **18 event types** — compressor cycles, defrost phases, all 10 alarm raised/cleared events, system boot, OTA
- **LittleFS storage** — circular buffer on flash, survives power loss
- **Streaming JSON API** — `GET /api/datalog` with time range and channel filters, chunked transfer for large datasets
- **SVG chart generation** — on-device SVG rendering for temperature history without JavaScript
- **ALARM_CLEAR tracking** — alarm duration calculated from raised→cleared event pairs

---

## 6. Web Interface

Embedded Svelte 4 SPA served from flash — 80KB gzipped (64KB JS + 16KB CSS).

### Pages
| Page | Description |
|------|-------------|
| **Dashboard** | Live temperatures, compressor/defrost status, active alarms |
| **Settings** | Thermostat, defrost, protection parameters — grouped by module |
| **DataLog** | Temperature charts, event timeline, SVG export |
| **System** | WiFi config, MQTT settings, firmware info, OTA upload |
| **Network** | WiFi scan, AP/STA configuration, signal strength |

### UX Features
- **Bento-card layout** — modular card grid with GroupAccordion for parameter sections
- **Dark / Light theme** — CSS custom properties, user preference persisted
- **WebSocket real-time** — state updates pushed to browser without polling
- **Responsive** — mobile-optimized layout for on-site technicians
- **4 languages** — Ukrainian, English, German, Polish. Lazy-loaded language packs from LittleFS (~8KB gzip each). Adding a new language requires only translation files — no code changes

---

## 7. Connectivity

WiFi, MQTT, HTTP, and WebSocket — all hardened for industrial environments.

### WiFi
- **STA + AP fallback** — connects to configured network; falls back to AP mode if unavailable
- **AP→STA periodic probe** — while in AP mode, periodically attempts STA reconnection (backoff 30s→5min, heap guard 50KB, 15s timeout)
- **STA watchdog** — restarts WiFi subsystem after 10 minutes of STA disconnect
- **RSSI reporting** — signal strength published in heartbeat for remote diagnostics
- **mDNS** — device discoverable as `modesp-{id}.local` on LAN
- **Country code** — configurable regulatory domain (default UA, channels 1–13)

### MQTT
- **TLS support** — `mqtts://` on port 8883 with server certificate validation
- **Tenant-aware topics** — `modesp/v1/{tenant}/{device_id}/state/{key}`
- **Delta-publish** — only changed values transmitted, reducing bandwidth
- **Heartbeat** — periodic publish with firmware version, uptime, free heap, RSSI
- **LWT (Last Will)** — broker publishes offline status on unexpected disconnect
- **Exponential backoff** — reconnect delay 5s→300s on broker disconnect

### HTTP REST
- **23 endpoints** — parameter CRUD, state queries, WiFi scan, system info, datalog, OTA
- **WebSocket** — real-time state push to connected browsers

---

## 8. OTA Updates

Dual-partition firmware updates with rollback protection and integrity verification.

- **Dual-partition scheme** — OTA writes to inactive partition; active partition preserved for rollback
- **SHA-256 verification** — firmware checksum validated before marking new partition as bootable
- **Board compatibility check** — `board.json` board type compared against firmware metadata; mismatched boards rejected
- **Rollback** — if new firmware fails health check on first boot, device reverts to previous partition automatically
- **HTTP upload** — `POST /api/ota` accepts firmware binary from browser or CLI
- **Cloud-initiated** — ModESP Cloud or AWS IoT Jobs can push OTA URL over MQTT

---

## 9. Cloud Integration

Works with ModESP Cloud (default) or AWS IoT Core — compile-time backend selection via Kconfig.

### ModESP Cloud
- **Signed MQTT credentials** — cloud generates per-device MQTT username/password, delivered over-the-air
- **Signed firmware URLs** — OTA download URLs signed with expiry, preventing unauthorized firmware access
- **Auto-discovery** — device self-registers with shared bootstrap key; admin assigns to tenant

### AWS IoT Core (alternative backend)
- **mTLS authentication** — X.509 client certificates stored in NVS, uploaded via `/api/cloud`
- **Device Shadow** — reported state synced to AWS; delta callbacks for remote parameter changes
- **IoT Jobs** — OTA firmware deployment via AWS IoT Jobs with status reporting
- **Compile-time switch** — `idf.py menuconfig` → `MODESP_CLOUD_BACKEND` → Mosquitto or AWS IoT Core

---

## 10. Build & Testing

491 tests across two layers — host-native C++ and on-target Python integration.

### Host Tests (C++)
- **181 test cases, 418 assertions** — doctest framework compiled with g++ (no ESP32 hardware required)
- **Coverage** — thermostat FSM transitions, defrost phase logic, protection escalation, equipment arbitration
- **Fast iteration** — full suite runs in seconds on developer machine

### Target Tests (Python)
- **310 pytest cases** — HTTP API validation, MQTT message flow, WiFi behavior, OTA workflow
- **Real hardware** — tests run against ESP32 over serial/network

### Build Pipeline
- **Code generation** — Python generator produces headers and UI JSON from manifests before C++ compilation
- **ESP-IDF integration** — standard `idf.py build` / `idf.py flash monitor` workflow
- **Reproducible** — CMake + Ninja, pinned ESP-IDF v5.5 toolchain

---

## 11. Hardware Support

Board abstraction layer — switch hardware by changing one JSON file, zero code changes.

### Supported Board
| Board | I/O | Interface |
|-------|-----|-----------|
| **KC868-A6** | 6 relay outputs, 6 digital inputs | GPIO + PCF8574 I2C |

### Drivers

| Driver | Type | Details |
|--------|------|---------|
| `ds18b20` | Temperature sensor | 1-Wire, multi-drop, 12-bit resolution |
| `ntc` | Temperature sensor | ADC, Steinhart-Hart calibration |
| `relay` | Actuator | GPIO, active-high/low configurable |
| `digital_input` | Sensor | GPIO, pull-up/down, debounce |
| `pcf8574_relay` | Actuator | I2C expander, 8-bit port |
| `pcf8574_input` | Sensor | I2C expander, interrupt-driven |

### Board Abstraction
- **`board.json`** — defines physical pin mapping, I2C addresses, 1-Wire bus pins
- **`bindings.json`** — maps logical names (`air_sensor`, `compressor_relay`) to driver instances
- **New board** — create `board.json` + `bindings.json`, rebuild — no C++ changes required

---

## 12. Developer Experience

From manifest edit to running firmware in three commands — no manual wiring of state, UI, or MQTT.

- **Manifest-first workflow** — edit `manifest.json` → `idf.py build` → generated code updated automatically
- **Zero-config board switch** — swap `board.json` + `bindings.json` for different hardware targets
- **Module template** — consistent BaseModule interface: `on_init()`, `on_update()`, `on_message()`, `on_set()`
- **ETL everywhere** — `etl::string<N>`, `etl::vector<T,N>`, `etl::variant`, `etl::optional` — zero heap allocation in hot paths
- **State engine** — publish a key with `state.set("module.key", value)` — automatically available via MQTT, HTTP, WebSocket, and WebUI
- **NVS persistence** — mark parameter as `"persist": true` in manifest — value saved/restored across reboots automatically

---

## Technical Stack

| Layer | Technology |
|-------|-----------|
| **SoC** | ESP32-WROOM-32 (4MB flash, dual-core 240MHz) |
| **Framework** | ESP-IDF 5.5, FreeRTOS |
| **Language** | C++17 |
| **Containers** | ETL (Embedded Template Library) — zero heap allocation |
| **WebUI** | Svelte 4, Vite (build only), 80KB gzipped |
| **Storage** | NVS (parameters), LittleFS (datalog), SPIFFS (WebUI) |
| **Connectivity** | WiFi (STA/AP), MQTT + TLS, HTTP REST, WebSocket, mDNS |
| **Cloud** | ModESP Cloud (default) or AWS IoT Core (compile-time) |
| **Testing** | doctest (host C++), pytest (target integration) |
| **Code Generation** | Python, JSON manifests → C++ headers + UI JSON |
| **Build** | CMake, Ninja, ESP-IDF toolchain |

---

## Architecture Diagram

```
                    ┌──────────────────────────────────────────────┐
                    │              ESP32 (FreeRTOS)                │
                    │                                              │
  ┌──────────┐     │  ┌──────────┐  ┌───────────┐  ┌──────────┐  │
  │ DS18B20  │◄───►│  │Equipment │  │Thermostat │  │ Defrost  │  │
  │ NTC      │     │  │ Manager  │  │  4-state  │  │ 7-phase  │  │
  └──────────┘     │  │  (HAL)   │  │   FSM     │  │   FSM    │  │
                   │  └────┬─────┘  └─────┬─────┘  └────┬─────┘  │
  ┌──────────┐     │       │              │              │        │
  │  Relay   │◄───►│  ┌────▼──────────────▼──────────────▼────┐   │
  │  PCF8574 │     │  │         State Engine (126 keys)       │   │
  │  GPIO    │     │  └────┬──────────┬──────────┬────────────┘   │
  └──────────┘     │       │          │          │                │
                   │  ┌────▼────┐ ┌───▼────┐ ┌──▼─────────┐      │
                   │  │Protection│ │DataLog │ │ NVS        │      │
                   │  │10 alarms│ │6-ch    │ │ Persistence │      │
                   │  │2-level  │ │LittleFS│ │             │      │
                   │  └─────────┘ └────────┘ └─────────────┘      │
                   │                                              │
                   │  ┌──────────────────────────────────────┐    │
                   │  │          Services Layer               │    │
                   │  │  WiFi · MQTT · HTTP · WebSocket · OTA │    │
                   │  └───────┬──────────┬───────────┬───────┘    │
                   └──────────┼──────────┼───────────┼────────────┘
                              │          │           │
                         ┌────▼───┐ ┌────▼────┐ ┌───▼────────┐
                         │  MQTT  │ │ Browser │ │ ModESP     │
                         │ Broker │ │ (WebUI) │ │ Cloud /    │
                         │ (TLS)  │ │ Svelte  │ │ AWS IoT    │
                         └────────┘ └─────────┘ └────────────┘
```

---

*ModESP v4 — industrial refrigeration control on a $4 microcontroller.*
