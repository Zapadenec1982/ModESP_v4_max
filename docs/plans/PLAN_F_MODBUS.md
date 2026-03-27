# Блок F: Modbus RTU Slave — ДЕТАЛЬНИЙ ПЛАН

> **Статус: READY FOR IMPLEMENTATION (90%)**
> Досліджено: esp-modbus API, SharedState internals, MqttService pattern, register map design, UART hardware.
> Існуюча специфікація: `docs/ROADMAP_NEXT.md` F-11.2
> **TODO при виході з plan mode:** зберегти цей план як `docs/plans/PLAN_F_MODBUS.md`

---

## Контекст

Партнеру потрібен промисловий інтерфейс для інтеграції з існуючими системами моніторингу (Carel/Danfoss supervisor). Modbus RTU Slave — найпростіший шлях: зовнішній Master читає/пише будь-який параметр контролера через RS-485.

## Файлова структура

```
components/modesp_modbus/
├── include/modesp/net/
│   └── modbus_service.h          # ModbusService class (~80 рядків)
├── src/
│   └── modbus_service.cpp         # Implementation (~250 рядків)
├── CMakeLists.txt                 # component build config
└── idf_component.yml              # espressif/esp-modbus: "^2.0.0"

generated/
└── modbus_regmap.h                # AUTO-GENERATED register map

tools/
└── generate_ui.py                 # + ModbusRegmapGenerator class
```

**Модифіковані файли:**
| Файл | Зміна |
|---|---|
| `main/Kconfig.projbuild` | + `config MODESP_MODBUS_ENABLED` bool |
| `main/CMakeLists.txt` | + conditional `target_link_libraries(modesp_modbus)` |
| `main/main.cpp` | + `#if CONFIG_MODESP_MODBUS_ENABLED` static instance + registration |
| `tools/generate_ui.py` | + `ModbusRegmapGenerator` class (~80 рядків) |

## Register Map Design (manifest-driven SSoT)

**Scaling:** x10 для float (42.5°C → 425). Промисловий стандарт Danfoss/Dixell/Carel.
**Розмір:** 1 register (uint16_t) per value. Float → int16 з x10 scale. Int → int16 direct. Bool → coil bit.

| Modbus Range | Type | Count | Source | Content |
|---|---|---|---|---|
| **30001-30055** | Input Registers (RO) | 55 | `MQTT_PUBLISH[]` | Temperatures, states, readings |
| **40001-40067** | Holding Registers (RW) | 67 | `STATE_META[]` writable | Setpoints, parameters, thresholds |
| **00001-00012** | Coils (RO) | 12 | `protection.*_alarm` | Alarm status bits |
| **10001-10006** | Discrete Inputs (RO) | 6 | `equipment.*` | Relay states (comp, fan, etc.) |

**Generator output** (`generated/modbus_regmap.h`):
```cpp
// AUTO-GENERATED from manifests by tools/generate_ui.py
struct ModbusRegEntry {
    const char* state_key;     // SharedState key
    uint16_t address;          // Modbus register address
    int16_t scale;             // multiplier (10 for float, 1 for int)
    uint8_t type;              // 0=int16, 1=float_x10, 2=bool
};

static constexpr ModbusRegEntry MODBUS_INPUT_REGS[] = {
    {"equipment.air_temp",    30001, 10, 1},
    {"equipment.evap_temp",   30002, 10, 1},
    {"equipment.compressor",  30003,  1, 2},
    // ... 55 entries from MQTT_PUBLISH
};
static constexpr size_t MODBUS_INPUT_REG_COUNT = 55;

static constexpr ModbusRegEntry MODBUS_HOLDING_REGS[] = {
    {"thermostat.setpoint",   40001, 10, 1},
    {"thermostat.differential", 40002, 10, 1},
    {"protection.high_limit", 40003, 10, 1},
    // ... 67 entries from STATE_META where writable=true
};
static constexpr size_t MODBUS_HOLDING_REG_COUNT = 67;
```

## Sync Mechanism (SharedState ↔ Shadow Arrays)

```
SharedState (thread-safe, FreeRTOS mutex)
    ↕ sync in on_update() (~100Hz)
Shadow Arrays (uint16_t[], protected by esp-modbus lock)
    ↕ esp-modbus internal task reads/writes
Modbus Master (external device via RS485)
```

**CRITICAL: esp-modbus creates its own FreeRTOS task internally.** Shadow arrays accessed from 2 tasks:
- Main module loop (on_update) — writes SharedState → shadow
- esp-modbus internal task — serves shadow to Modbus master

**Thread safety: використовувати `mbc_slave_lock()/unlock()` (вбудований в esp-modbus), НЕ окремий mutex.**

**SharedState → Shadow (read path, every 500ms в on_update):**
1. `mbc_slave_lock(handle_)` — esp-modbus built-in lock
2. Iterate `MODBUS_INPUT_REGS[]` + `MODBUS_HOLDING_REGS[]`
3. `state_->get(entry.state_key)` → convert → `shadow_input_[i]` / `shadow_holding_[i]`
4. `mbc_slave_unlock(handle_)`

**Shadow → SharedState (write path, master write event):**
1. `mbc_slave_check_event(handle_, MB_EVENT_HOLDING_REG_WR, pdMS_TO_TICKS(0))` — **NON-BLOCKING** (timeout=0)
2. If event detected: `mbc_slave_get_param_info(handle_, &info, 10)` → which register changed
3. `mbc_slave_lock(handle_)`, read changed value from shadow
4. Convert: `float val = (int16_t)shadow_holding_[i] / (float)entry.scale`
5. Validate against STATE_META min/max
6. `state_set(entry.state_key, val)` → triggers NVS persist + WS broadcast
7. `mbc_slave_unlock(handle_)`

**`mbc_slave_check_event()` is BLOCKING by default.** Обов'язково використовувати `pdMS_TO_TICKS(0)` для non-blocking check в on_update(). Альтернатива: окремий FreeRTOS task для event processing.

**Concurrent writes (MQTT + Modbus + WebUI):** SharedState = last-write-wins, без source tracking. Прийнятно для MVP. Для production потрібно буде додати write source priority або lockout mechanism.

## UART Configuration (KC868-A6 specific)

**ROADMAP_NEXT.md помилка:** ROADMAP каже GPIO17/16 — це RS232 порт KC868-A6, НЕ RS485!

**Правильні піни RS485 на KC868-A6:**
| Signal | GPIO | Chip | Примітка |
|---|---|---|---|
| **TX** | **GPIO 27** | MAX13487EESA DI | RS485 Driver Input |
| **RX** | **GPIO 14** | MAX13487EESA RO | RS485 Receiver Output |
| DE/RE | — | MAX13487EESA internal | Auto-direction, NO GPIO needed |

**Для порівняння, RS232 порт (НЕ використовуємо):**
| Signal | GPIO | Примітка |
|---|---|---|
| TX | GPIO 17 | RS232 debug |
| RX | GPIO 16 | RS232 debug |

**CRITICAL:** MAX13487EESA = auto-direction → `UART_MODE_UART`, НЕ `UART_MODE_RS485_HALF_DUPLEX`. RTS pin = `UART_PIN_NO_CHANGE`.

```cpp
// esp-modbus v2 communication config
// UART_NUM_1 (UART0 = console, UART2 = free for future Master)
uart_set_pin(UART_NUM_1, 27/*TX*/, 14/*RX*/,
             UART_PIN_NO_CHANGE/*RTS*/, UART_PIN_NO_CHANGE/*CTS*/);

mb_communication_info_t comm_info = {
    .ser_opts = {
        .port = UART_NUM_1,
        .data_bits = UART_DATA_8_BITS,
        .stop_bits = UART_STOP_BITS_1,
        .baudrate = baudrate_,       // NVS, default 9600
        .parity = parity_,           // NVS, default EVEN (8E1)
        .mode = MB_MODE_RTU,
        .uid = slave_address_,       // NVS, default 1
    },
};
```

**Перевірити перед реалізацією:** завантажити PDF schematic з https://www.kincony.com/download/KC868-A6-schematic.pdf і верифікувати GPIO27→DI, GPIO14→RO на MAX13487EESA.

## Board.json Extension (SSoT для UART)

Поточна філософія: board.json = SSoT для hardware. UART піни мають бути там, не hardcoded.

**Додати в `boards/kc868a6/board.json`:**
```json
{
  "uart_buses": [
    {
      "id": "rs485_1",
      "uart_num": 1,
      "tx_gpio": 27,
      "rx_gpio": 14,
      "de_gpio": -1,
      "transceiver": "max13487",
      "auto_direction": true
    }
  ]
}
```

**Додати в `hal_types.h`:**
```cpp
struct UartBusConfig {
    const char* id;        // "rs485_1"
    uart_port_t uart_num;  // UART_NUM_1
    int tx_gpio;           // 27
    int rx_gpio;           // 14
    int de_gpio;           // -1 (auto-direction)
    bool auto_direction;   // true for MAX13487
};
```

**ModbusService** читає UartBusConfig з BoardConfig замість hardcoded GPIO.

## NVS Configuration

**Namespace:** `"modbus"`
| Key | Type | Default | Range | Persist |
|---|---|---|---|---|
| `enabled` | bool | false | — | yes |
| `address` | int32 | 1 | 1-247 | yes |
| `baudrate` | int32 | 9600 | 9600/19200/38400/57600/115200 | yes |
| `parity` | int32 | 1 (EVEN) | 0=none/1=even/2=odd | yes |

**Патерн (з MqttService):**
```cpp
void load_config() {
    nvs_helper::read_bool("modbus", "enabled", enabled_);
    nvs_helper::read_i32("modbus", "address", slave_address_);
    nvs_helper::read_i32("modbus", "baudrate", baudrate_);
    nvs_helper::read_i32("modbus", "parity", parity_);
}
```

## HTTP API

**GET /api/modbus** → JSON:
```json
{
  "enabled": true,
  "address": 1,
  "baudrate": 9600,
  "parity": 1,
  "status": "running",
  "input_reg_count": 55,
  "holding_reg_count": 67,
  "requests_total": 1234,
  "errors_total": 5
}
```

**POST /api/modbus** ← JSON:
```json
{"enabled": true, "address": 2, "baudrate": 19200, "parity": 1}
```
Зберігає в NVS, ставить прапор `restart_pending_` → перезапуск Modbus stack в наступному on_update().

## Kconfig

```kconfig
config MODESP_MODBUS_ENABLED
    bool "Enable Modbus RTU Slave (UART1, RS-485)"
    default n
    help
        Enables Modbus RTU Slave on UART1 via RS-485.
        Register map auto-generated from module manifests.
        Adds ~3.5KB RAM, ~23KB Flash.
```

## CMakeLists (conditional linking)

```cmake
# main/CMakeLists.txt — додати після cloud backend:
if(CONFIG_MODESP_MODBUS_ENABLED)
    target_link_libraries(${main_lib} PRIVATE idf::modesp_modbus)
endif()
```

```cmake
# components/modesp_modbus/CMakeLists.txt
idf_component_register(
    SRCS "src/modbus_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core esp_http_server
    PRIV_REQUIRES nvs_flash driver
)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/generated")
```

```yaml
# components/modesp_modbus/idf_component.yml
dependencies:
  espressif/esp-modbus: "^2.0.0"
```

## RAM/Flash Budget (CORRECTED)

| Компонент | RAM | Flash | Примітка |
|---|---|---|---|
| Shadow arrays (55+67 × 2B + 12 coils) | ~260B | — | static uint16_t[] |
| esp-modbus internal task stack | ~4-8KB | — | CONFIG_FMB_CONTROLLER_STACK_SIZE |
| esp-modbus event queue + buffers | ~2-4KB | — | CONFIG_FMB_CONTROLLER_NOTIFY_QUEUE_SIZE |
| esp-modbus controller object | ~2-4KB | ~15KB | Protocol state machine |
| UART driver buffers | ~512B | — | TX+RX ring buffers |
| ModbusService instance | ~200B | — | config, counters, handle |
| modbus_service.cpp code | — | ~8KB | Custom logic |
| **Всього** | **~10-17KB** | **~23KB** | **Значно більше ніж попередня оцінка 3.5KB** |

**ESP32 RAM budget:**
- Загально: ~320KB SRAM
- Поточне використання: ~200KB (FreeRTOS + WiFi + MQTT + modules + SharedState)
- Вільно: ~120KB
- Modbus: ~15KB → залишається ~105KB — **достатньо**
- Для 2-zone (~40KB додатково): ~65KB — **на межі, потрібен точний вимір**

## Покрокова реалізація

| Крок | Що робити | Файли | Тести |
|---|---|---|---|
| **F1** | Generator: `ModbusRegmapGenerator` → `modbus_regmap.h` | `tools/generate_ui.py` | pytest: validate generated register map |
| **F2** | Kconfig + CMake skeleton (порожній component) | `Kconfig.projbuild`, `CMakeLists.txt` × 3, `idf_component.yml` | `idf.py build` з `MODESP_MODBUS_ENABLED=y` |
| **F3** | `modbus_service.h` — class declaration за MqttService pattern | `modbus_service.h` | Компіляція |
| **F4** | `modbus_service.cpp` — load_config, on_init (UART + esp-modbus start) | `modbus_service.cpp` | Hardware: UART TX verify з осцилографом |
| **F5** | Sync: SharedState → shadow arrays (read path) | `modbus_service.cpp` | Hardware: pymodbus read registers |
| **F6** | Sync: shadow → SharedState (write path + validation) | `modbus_service.cpp` | Hardware: pymodbus write + verify WebUI |
| **F7** | HTTP API handlers (GET/POST /api/modbus) | `modbus_service.cpp` | curl GET/POST |
| **F8** | main.cpp integration + NVS defaults | `main.cpp` | Full integration test |
| **F9** | Host tests: conversion functions | `tests/host/` | doctest |
| **F10** | Hardware test script (pymodbus) | `tools/tests/test_modbus.py` | pytest з USB-RS485 adapter |
| **F11** | Documentation: register map, changelog | `docs/`, `CHANGELOG.md` | — |

## Verification

1. **Host tests (doctest):** float→x10 conversion, int16 overflow, bool→coil, address validation, STATE_META coverage check
2. **pytest:** generated register map validation (all state_meta keys mapped, no address collisions)
3. **Hardware:** USB-RS485 adapter + pymodbus script:
   - Read all input registers → verify match WebUI values
   - Write holding register (setpoint) → verify WebUI updates
   - Read coils → verify alarm states
   - Test invalid addresses → verify exception response
4. **Tools:** QModMaster (free GUI) для manual testing

## Reusable Patterns

| Що | Звідки | Для чого |
|---|---|---|
| NVS load/save | `mqtt_service.cpp:32-55` | Config persistence |
| HTTP handler registration | `mqtt_service.cpp` register_http_handlers | GET/POST /api/modbus |
| jsmn JSON parsing | `mqtt_service.cpp` handle_post | POST body parsing |
| Kconfig conditional linking | `main/CMakeLists.txt:10-15` (AWS/MQTT) | Modbus enable/disable |
| Generator class | `tools/generate_ui.py` StateMetaGenerator | ModbusRegmapGenerator |
| BaseModule lifecycle | `base_module.h` | on_init/on_update/on_stop |

## Відкриті питання (вирішити при реалізації)

1. **~~GPIO17/16 vs GPIO27/14~~** → ВИРІШЕНО: RS485 = GPIO27 (TX) + GPIO14 (RX). GPIO17/16 = RS232 (debug). ROADMAP_NEXT.md потрібно виправити.
2. **esp-modbus v2 vs v1 API:** v2 handle-based API доступний з esp-modbus >=2.0.0. Перевірити сумісність з ESP-IDF v5.5.
3. **Coils read-only:** esp-modbus дозволяє area як RO через descriptor. Перевірити чи це працює для coils.
4. **Float precision:** x10 scaling (0.1°C). Для superheat PID може знадобитись x100 (0.01°C) — додамо окрему register range пізніше.
