# Architecture: ModESP v4

## System Overview

Modular framework for industrial ESP32 controllers (refrigeration).
Replaces expensive Danfoss/Dixell controllers with ESP32 + professional architecture.

## Module Pipeline

```
Equipment(0) → Protection(1) → Thermostat(2) + Defrost(2)
    │                │                │              │
    ├─ reads sensors ├─ monitors      ├─ cooling     ├─ 7-phase cycle
    ├─ publishes T   │  10 alarms     │  state       │  3 types
    └─ arbitrates    └─ lockout/block │  machine     └─ timer/demand
       relay outputs                  └─ night mode
```

## Equipment Layer (priority=CRITICAL)

- Only module with HAL driver access
- Reads sensors → publishes `equipment.air_temp`, `equipment.sensor1_ok`, etc.
- Reads requests from business modules: `thermostat.req.compressor`, `defrost.req.*`, `protection.lockout`
- **Arbitration:** Protection LOCKOUT > compressor_blocked > Defrost active > Thermostat
- **Interlocks:** defrost_relay and compressor NEVER active simultaneously
  - **WHY:** hot gas through inactive compressor = liquid slugging → mechanical damage
- **Anti-short-cycle (output-level):** COMP_MIN_OFF_MS=180s, COMP_MIN_ON_MS=120s
  - **WHY:** compressor needs oil return + pressure equalization before restart; industry standard 3 min off / 2 min on
- **EMA filter:** alpha = 1/(filter_coeff+1), roundf() to 0.01C
  - **WHY:** EMA smooths sensor noise without lag of moving average; roundf to 0.01 reduces SharedState version bumps
- `equipment.has_*` state keys: has_defrost_relay, has_cond_fan, has_door_contact, has_evap_temp, has_cond_temp, has_night_input, has_ntc_driver, has_ds18b20_driver

## Thermostat v2

- **Asymmetric differential:** ON at T >= SP + differential, OFF at T <= SP
  - **WHY:** symmetric differential causes hunting around setpoint; asymmetric = industry standard (Danfoss/Dixell compatible behavior)
- **State machine:** STARTUP > IDLE <> COOLING, SAFETY_RUN (sensor failure)
- **Night Setback:** 4 modes (0=off, 1=SNTP schedule, 2=DI, 3=manual)
- **Display during defrost:** 3 modes (real T / frozen T / "-d-")
- Requests: `thermostat.req.compressor`, `thermostat.req.evap_fan`, `thermostat.req.cond_fan`

## Protection (10 monitors)

- **Delayed:** High Temp, Low Temp, Door (separate delays in minutes)
- **Instant:** Sensor1, Sensor2
- **Compressor safety:** Short Cycle, Rapid Cycle, Continuous Run, Pulldown, Rate-of-Change
- **Continuous Run Escalation:** Level 1 compressor_blocked > Level 2 lockout
- **Defrost blocking:** High Temp + Rate suppressed during heating phases + post_defrost_delay
- Alarm code priority: lockout > comp_blocked > err1 > rate_rise > high_temp > pulldown > short_cycle > rapid_cycle > low_temp > continuous_run > err2 > door > none

## Defrost (7-phase)

- **Phases:** IDLE > [STABILIZE > VALVE_OPEN >] ACTIVE > [EQUALIZE >] DRIP > FAD > IDLE
- **3 types:** 0=natural (stop), 1=electric heater, 2=hot gas (7 phases)
  - **WHY 7 phases for hot gas:** stabilize (equalize pressure) → valve open (redirect hot gas) → active (melt ice) → equalize (pressure balance) → drip (drain water) → fan delay (evaporate residual). Skipping phases = ice re-freeze or liquid slugging
- **4 initiation modes:** timer, demand (T_evap < demand_temp), combined, manual
- **Termination:** by T_evap >= end_temp (primary), by max_duration timer (safety)
- **defrost_relay** — unified relay for both electric heater and hot gas valve

## Features System

- Manifest-driven feature flags with `requires_roles`
- `FeatureResolver` (Python): checks `bindings.json` → active/inactive features
- `features_config.h`: constexpr array + `is_feature_active()` inline lookup
- C++ modules: `has_feature("name")` for runtime guards

## DataLogger (6 channels)

- Channels: air (always) + evap + cond + setpoint + humidity + reserved
- TempRecord 16 bytes, LittleFS storage (temp.bin + events.bin)
- 18 event types, RAM buffer (16 temp + 32 events), flush every 10 min
- JSON v3: dynamic channels, streaming chunked response

## EEV Module (Electronic Expansion Valve)

- **PI controller** (velocity form): `delta = Kp*(err - prev_err) + Ki*Ts*err` — anti-windup by design
- **7 states:** IDLE → STARTUP (feed-forward 50%, 120s) → RUNNING (PI) → LOW_SH_PROTECT → DEFROST → SENSOR_FAULT (safe 40%) → EXERCISE (24h unblock cycle)
- **Settings:** target SH 8K, Kp 3.0, Ki 0.5, interval 3s, min/max 5-95%, deadband 1K
- **MOP protection:** proportional close (1-8%/interval) when pressure exceeds threshold
- **LowSH protection:** aggressive close (3%/interval) when SH < 2K; emergency close when SH < 0K
- **WHY velocity form:** No integral accumulator = no windup, bumpless transfer between states

## Refrigerant Library (23 refrigerants)

- Antoine equation: `T_sat = A2 / (ln(P*100000) - A1) - A3 - 273.15` (from NIST RefProp 10.0)
- Zeotropic blend handling: dew point = T_mid + glide/2 for superheat calculation
- All tables constexpr in flash — zero heap
- Includes: R134a, R404A, R507A, R290, R448A, R449A, R452A, R407A/C/F, R410A, R32, R454A/B, R744 (CO2), R717 (NH3), R22, R600a, R513A, R1234yf/ze, R455A, R1270

## Lighting Module

- Modes: off (0), on (1), auto (2)
- Auto mode: follows night setback schedule or door contact

## Modbus RTU Slave

- Protocol: Modbus RTU over UART1 RS-485 (MAX13487EESA auto-direction)
- 284 input registers (read-only: sensors, alarms, status) + 252 holding registers (read-write: settings)
- Auto-sync SharedState ↔ shadow arrays every 500ms, thread-safe lock
- Configurable: slave address (1-247), baud (9600-115200), parity (even/none/odd)
- HTTP API: `GET/POST /api/modbus` for config

## Multi-Zone Support

- 1-2 independent zones: separate EEV, fans, defrost relay, evap/pressure sensors per zone
- Zone 2 enabled when `equipment.active_zones >= 2`
- Per-zone state keys: `equipment.evap_temp_z1/z2`, `equipment.suction_bar_z1/z2`

## Key Services

| Service | Role |
|---|---|
| SharedState | Central state store, thread-safe (FreeRTOS mutex), version counter |
| PersistService | Auto-persist settings to NVS, djb2 hash keys, debounce 5s |
| ModuleManager | Lifecycle: register > init_all > update_all > stop_all |
| HttpService | REST API (port 80), 17+ endpoints |
| WsService | Delta broadcasts (1500ms), heartbeat (20s), max 3 clients |
| WiFiService | STA + AP fallback, country UA, AP>STA probe with backoff |
| MqttService | HA Discovery, TLS, LWT, exponential backoff, delta-publish |
| ModbusService | RTU slave, 284+252 registers, auto-sync 500ms |

## Drivers (9+)

| Driver | Type | Hardware |
|---|---|---|
| ds18b20 | sensor | OneWire (MATCH_ROM multi-sensor, SEARCH_ROM scan) |
| ntc | sensor | ADC thermistor (B-parameter equation) |
| pressure_adc | sensor | Ratiometric ADC (Carel SPKT 0.5-4.5V, open/short detection) |
| digital_input | sensor | GPIO input (50ms debounce) |
| pcf8574_input | sensor | I2C PCF8574 digital input |
| relay | actuator | GPIO relay (min on/off time) |
| pcf8574_relay | actuator | I2C PCF8574 relay (8-bit port expander) |
| eev_stepper | actuator | GPIO STEP+DIR → stepper IC (TMC2209/DRV8825/A4988), homing, NVS persist |
| eev_pcf8574_stepper | actuator | I2C PCF8574 → H-bridge (L298N), repurposed DIN positions |
| eev_analog | actuator | ESP32 DAC → LM258 op-amp → 0-10V → EVD Mini positioner |
| akv_pulse | actuator | GPIO PWM → MOSFET → 24V solenoid (Danfoss AKV, Emerson EX2) |

## AWS IoT Core (feature/aws-iot branch)

Compile-time alternative to Mosquitto via Kconfig `MODESP_CLOUD_BACKEND`.

- **Component:** `components/modesp_aws/` — AwsIotService (BaseModule)
- **mTLS:** AmazonRootCA1 embedded + client cert/key from NVS
- **Topics:** `modesp/{device_id}/state/{key}`, `cmd/{key}`, `status`, `heartbeat`
- **Shadow:** reported (62 writable params) + delta (desired → validate → apply)
- **OTA:** IoT Jobs → ota_handler::start_ota() (provider-agnostic)
- **HTTP:** GET/POST `/api/cloud` (endpoint, thing_name, cert upload)
- **NVS:** 32KB partition, certs in namespace "awscert"
- Docs: [docs/12_aws_iot.md](docs/12_aws_iot.md)

## Svelte WebUI

- Svelte 4, Rollup bundler, Light/Dark theme, UA/EN i18n
- Build: `cd webui && npm run build && npm run deploy`
- Output: `data/www/bundle.js.gz` (~63KB) + `bundle.css.gz` (~13KB)
- Premium dark theme, bento-card dashboard, responsive accordions
