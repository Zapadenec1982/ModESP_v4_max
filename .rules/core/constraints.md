# Constraints: ESP32 Firmware Rules

## Zero Heap in Hot Path (CRITICAL)

**WHY:** ESP32 has 320KB RAM, heap fragmentation causes crashes in 24/7 industrial operation.

| NEVER in on_update() / on_message() | ALWAYS use instead |
|---|---|
| `std::string`, `std::vector` | `etl::string<N>`, `etl::vector<T,N>` |
| `new`, `malloc`, `std::make_shared` | Stack allocation, `etl::optional`, `etl::variant` |
| `std::map`, `std::unordered_map` | `etl::unordered_map<K,V,N>` |
| `printf`, `std::cout` | `ESP_LOGI/W/E/D(TAG, ...)` |

## SharedState Types

```cpp
// Key: etl::string<32>, Value: etl::variant<int32_t, float, bool, etl::string<32>>
SharedState: etl::unordered_map<StateKey, StateValue, 158>  // MODESP_MAX_STATE_ENTRIES (126 manifest + 32 runtime)
```

- `set()` increments version counter — WsService compares versions
- `set()` with `track_change=false` for timers/diagnostics (no WS broadcast)
- Persist callback fires OUTSIDE mutex
- Float rounding: `roundf(T * 100) / 100` — reduces version bumps

## ESP-IDF Style

- Every `.cpp` file: `static const char* TAG = "ModuleName";`
- Logging: `ESP_LOGI(TAG, ...)`, `ESP_LOGW(TAG, ...)`, `ESP_LOGE(TAG, ...)`
- Build with `-Werror=all`: every warning = compile error
- Comments in **Ukrainian**, Doxygen headers in **English**

## Module System

- Every module extends `BaseModule` (on_init, on_update, on_stop, on_message)
- Every module = separate ESP-IDF component with `CMakeLists.txt`
- Priorities: CRITICAL(0) > HIGH(1) > NORMAL(2) > LOW(3)
- Update order: Equipment(0) > Protection(1) > Thermostat(2) + Defrost(2)
- `init_all()` is idempotent: skips modules with state != CREATED

## Message Bus

- `etl::message_bus` for publish/subscribe between modules
- Message ID ranges: System 0-49, Services 50-99, HAL 100-109, Drivers 110-149, Modules 150-249
- Message definitions next to module (`*_messages.h`)

## State Keys

- Format: `<module>.<key>` — e.g. `thermostat.temperature`, `system.uptime`
- Read-write float/int MUST have `min`, `max`, `step` (or `options` for enum/select)
- Persist keys use djb2 hash in NVS: `"s" + 7 hex chars`

## Single Source of Truth (SSoT)

```
module manifest.json ─┐
driver manifest.json ─┼→ Python generator → ui.json + C++ headers
board.json + bindings.json ─┘
```

**NEVER** edit generated files manually — they are overwritten on every build:
- `data/ui.json`
- `generated/state_meta.h`
- `generated/mqtt_topics.h`
- `generated/display_screens.h`
- `generated/features_config.h`

To change UI/state/MQTT: edit the **manifest**, then rebuild.

## Secrets Protection (CRITICAL)

**WHY:** Leaked credentials in git history are permanent — even force-push doesn't remove them from clones.

| NEVER commit | Where it belongs |
|---|---|
| WiFi passwords, MQTT credentials | NVS (runtime config via WebUI) |
| AWS IoT certs/keys (*.pem) | NVS partition "awscert" |
| API tokens, `.env` files | `.gitignore` (already listed) |
| Private keys, passwords in code | `Kconfig` with `default ""` + NVS |

**Before every commit:** verify `git diff --cached` contains no secrets.
If accidentally committed: notify user immediately, do NOT push.
