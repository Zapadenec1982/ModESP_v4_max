# Блок A: Manifest-driven namespace — ДЕТАЛЬНИЙ ПЛАН

> **Статус: DESIGNING (75%)**
> Досліджено: всі hardcoded keys (181 total), ModuleManager capacity (16/16 зайнято),
> SharedState RAM (~14KB), generator architecture, main.cpp creation pattern.

---

## Контекст

Поточна архітектура: 1 module instance per type, hardcoded `"module.key"` strings в C++.
Для 2-zone: потрібні 2 екземпляри thermostat, defrost (+ eev) з різними namespace.
Без namespace рефакторингу мультизонність неможлива — два thermostat не можуть писати в `"thermostat.setpoint"`.

## Verified Numbers (з exploration)

| Модуль | Own keys | Cross-module reads | Total | Cross-module keys |
|---|---|---|---|---|
| thermostat | 27 | 8 | 35 | equipment.air_temp, equipment.compressor, equipment.evap_temp, equipment.night_input, equipment.sensor1_ok, equipment.sensor2_ok, defrost.active, protection.lockout |
| protection | 42 | 12 | 54 | equipment.air_temp/compressor/cond_temp/door_open/evap_temp/has_cond_temp/has_evap_temp, defrost.active/demand_temp/manual_start/phase/type |
| equipment | 22 | 14 | 36 | thermostat.req.*, defrost.*, protection.lockout/compressor_blocked/condenser_block/door_comp_blocked, lighting.req.light |
| defrost | 29 | 5 | 34 | equipment.compressor/evap_temp/has_defrost_relay, protection.lockout/compressor_blocked |
| lighting | 3 | 2 | 5 | thermostat.night_active, equipment.light |
| datalogger | 6 | 11 | 17 | equipment.compressor/door_open, protection.* (8 alarms), defrost.active |
| **TOTAL** | **129** | **52** | **181** | |

## Capacity Changes Required

| Parameter | Current | 2-Zone | Why |
|---|---|---|---|
| `MODESP_MAX_MODULES` | 16 (**ALL USED**) | **24** | +thermostat_z2 +defrost_z2 +eev_z1 +eev_z2 +modbus = +5..8 |
| `MODESP_MAX_STATE_ENTRIES` | 169 | **255** | +86 zone2 keys (+27 thermo +29 defrost +15 eev ×2) |
| `MODESP_MAX_BUS_ROUTERS` | 24 | 24 (OK) | Already sufficient |
| SharedState RAM | ~14KB | ~21KB | +7KB (255×82B vs 169×82B) |
| Resolved keys RAM/zone | 0 | ~2.2KB | 27+29+15 = 71 keys × 32B per zone |

## Покрокова реалізація (8 кроків, від безпечного до складного)

**A1: Збільшення capacity (zero-risk)**
- `generated/state_meta.h`: MODESP_MAX_STATE_ENTRIES 169 → 255
- `components/modesp_core/include/modesp/module_manager.h`: MODESP_MAX_MODULES 16 → 24
- Не ламає нічого, просто резервує RAM
- **Тест:** build + existing 108 host tests + 254 pytest = all pass

**A2: Generator → key enums per module**
- Файл: `tools/generate_ui.py` — новий `ModuleKeyGenerator`
- Input: `modules/thermostat/manifest.json` → `state: {"setpoint": {...}, ...}`
- Output: `generated/thermostat_keys.h`:
  ```cpp
  // AUTO-GENERATED from modules/thermostat/manifest.json
  namespace modesp::gen::thermostat {
      enum class Key : uint8_t {
          SETPOINT = 0, DIFFERENTIAL, STATE, TEMPERATURE, ...
          COUNT = 27
      };
      inline constexpr const char* SHORT_NAMES[] = {
          "setpoint", "differential", "state", "temperature", ...
      };
  }
  ```
- Один header per module: thermostat_keys.h, defrost_keys.h, protection_keys.h, equipment_keys.h
- **~80 рядків в generator**
- **Тест:** pytest validate generated enums match manifest keys

**A3: BaseModule extensions (backward-compatible)**
- Файл: `components/modesp_core/include/modesp/base_module.h`
- Додати `ns_` field (etl::string<32>), default = name_
- Додати constructor overload: `BaseModule(const char* name, const char* ns, ModulePriority)`
- Додати `ns_key()` helper для швидкого прототипування:
  ```cpp
  const StateKey& ns_key(const char* short_name) {
      ns_buf_ = ns_; ns_buf_ += "."; ns_buf_ += short_name;
      return ns_buf_;
  }
  ```
- Додати `resolve_keys<N>()` для batch init:
  ```cpp
  template<size_t N>
  void resolve_keys(const char* const short_names[],
                    etl::array<StateKey, N>& out) {
      for (size_t i = 0; i < N; ++i) {
          out[i] = ns_; out[i] += "."; out[i] += short_names[i];
      }
  }
  ```
- Додати InputBinding support:
  ```cpp
  struct InputBinding { const char* role; const char* key; };
  void set_input_bindings(etl::span<const InputBinding> bindings);
  const char* input_key(const char* role) const;  // lookup or fallback
  ```
- **Зворотна сумісність:** існуючий конструктор `BaseModule(name, priority)` → `ns_ = name_`
- **~60 рядків додано, 0 рядків змінено**
- **Тест:** existing tests pass + new test for ns_key(), resolve_keys(), input_key()

**A4: Параметризація конструкторів модулів**
- Файли: всі 6 module headers + cpp
- Додати optional `ns` parameter:
  ```cpp
  // До:
  ThermostatModule() : BaseModule("thermostat", ModulePriority::NORMAL) {}
  // Після:
  ThermostatModule(const char* ns = "thermostat")
      : BaseModule("thermostat", ns, ModulePriority::NORMAL) {}
  ```
- `name_` залишається "thermostat" (для features lookup)
- `ns_` стає параметром ("thermostat" або "thermo_z2")
- **Не ламає існуючий код — default parameter**
- **Тест:** build + existing tests (default ns = name)

**A5: Рефакторинг ThermostatModule (найчистіший для першого)**
- Файл: `modules/thermostat/src/thermostat_module.cpp`
- Додати member: `etl::array<StateKey, modesp::gen::thermostat::Key::COUNT> keys_;`
- В `on_init()`: `resolve_keys(modesp::gen::thermostat::SHORT_NAMES, keys_);`
- Замінити 27 own keys: `"thermostat.setpoint"` → `keys_[Key::SETPOINT]`
- Замінити 8 cross-module reads: `"equipment.air_temp"` → `input_key("air_temp")`
- InputBindings в main.cpp при створенні zone2:
  ```cpp
  static constexpr InputBinding thermo_z2_inputs[] = {
      {"air_temp", "equipment.air_temp_z2"},
      {"evap_temp", "equipment.evap_temp_z2"},
      {"compressor", "equipment.compressor"},  // shared
  };
  thermostat_z2.set_input_bindings(thermo_z2_inputs);
  ```
- **Логіка НЕ змінюється. 35 точкових замін.**
- **Тест:** existing thermostat tests pass + new test for zone2 key generation
- RAM: 27 × 32B = 864B per instance

**A6: Рефакторинг DefrostModule**
- Аналогічно A5: 29 own keys + 5 cross-module reads
- InputBindings для zone2: equipment.evap_temp → equipment.evap_temp_z2
- **34 точкових замін**
- RAM: 29 × 32B = 928B per instance

**A7: Рефакторинг ProtectionModule**
- 42 own keys + 12 cross-module reads
- **АРХІТЕКТУРНЕ ПИТАННЯ:** Protection per-zone чи shared?
  - Per-zone: кожна зона має свій high_temp alarm → 2 instances
  - Shared: один Protection читає обидві зони → складна логіка
  - **Рекомендація:** Per-zone (простіше, як Thermostat/Defrost)
- **54 точкових замін** (найбільший модуль)
- RAM: 42 × 32B = 1.3KB per instance

**A8: Equipment multi-zone arbitration**
- Файл: `modules/equipment/src/equipment_module.cpp`
- Equipment залишається ОДИН instance (shared compressor, shared cond fan)
- **Замість hardcoded `thermostat.req.compressor`:**
  ```cpp
  struct ZoneRef {
      const char* ns;                    // "thermostat" or "thermo_z2"
      const char* req_compressor_key;    // resolved at init
      const char* req_defrost_relay_key;
      const char* req_evap_fan_key;
  };
  etl::vector<ZoneRef, 4> zones_;       // max 4 zones

  bool any_zone_needs_compressor() {
      for (auto& z : zones_)
          if (read_bool(z.req_compressor_key)) return true;
      return false;
  }
  ```
- Zone registration в main.cpp:
  ```cpp
  equipment.register_zone("thermostat", "defrost");
  equipment.register_zone("thermo_z2", "defrost_z2");
  ```
- Арбітраж: `lockout > comp_blocked_any_zone > defrost_any_zone > thermostat_any_zone`
- Coordinated defrost: одна зона за раз (пріоритет у порядку реєстрації)
- **Це найскладніший крок — ~150 рядків нового коду в equipment**
- **Тест:** new host tests for multi-zone arbitration

## main.cpp 2-Zone Configuration (після A4-A8)

```cpp
// Zone 1 (default, backward-compatible)
static ThermostatModule thermostat_z1;        // ns = "thermostat" (default)
static DefrostModule defrost_z1;              // ns = "defrost" (default)
static ProtectionModule protection_z1;        // ns = "protection" (default)

// Zone 2
static ThermostatModule thermostat_z2("thermo_z2");
static DefrostModule defrost_z2("defrost_z2");
static ProtectionModule protection_z2("protect_z2");

// Zone 2 input bindings (which SharedState keys to read)
static constexpr InputBinding thermo_z2_inputs[] = {
    {"air_temp",   "equipment.air_temp_z2"},
    {"evap_temp",  "equipment.evap_temp_z2"},
    {"compressor", "equipment.compressor"},     // shared
    {"sensor1_ok", "equipment.sensor3_ok"},      // zone2 sensors
};
thermostat_z2.set_input_bindings(thermo_z2_inputs);

// Equipment knows about both zones
equipment.register_zone("thermostat", "defrost");
equipment.register_zone("thermo_z2", "defrost_z2");

// Register all
app.modules().register_module(thermostat_z1);
app.modules().register_module(thermostat_z2);
// ... etc
```

**Kconfig для zone count:**
```kconfig
config MODESP_ZONE_COUNT
    int "Number of cooling zones"
    default 1
    range 1 4
```

## Manifest Extensions (для generator)

Поточний manifest `inputs` section вже визначає cross-module залежності:
```json
"inputs": {
    "equipment.air_temp": {"source_module": "equipment", "type": "float"}
}
```

Для namespace: inputs стають **ролями**, не конкретними ключами:
```json
"inputs": {
    "air_temp": {"source_module": "equipment", "type": "float", "default_key": "equipment.air_temp"}
}
```

Generator використовує `default_key` для single-zone, InputBinding override для multi-zone.

## Порядок і залежності

```
A1 (capacity)     ──→  безпечно, build test
    ↓
A2 (generator)    ──→  pytest validate
    ↓
A3 (BaseModule)   ──→  host tests ns_key + resolve_keys
    ↓
A4 (constructors) ──→  build, existing tests
    ↓
A5 (Thermostat)   ──→  host tests + single-zone regression
A6 (Defrost)      ──→  паралельно з A5
A7 (Protection)   ──→  після A5/A6
    ↓
A8 (Equipment)    ──→  new multi-zone arbitration tests
    ↓
Integration test  ──→  2-zone full stack
```

**Critical path:** A1→A2→A3→A4→A5→A8 (~6 кроків серійно)
**Паралелізація:** A5+A6 одночасно, A7 після них

## Відкриті питання

1. **Protection: per-zone чи shared?** Рекомендую per-zone для простоти. Але деякі алярми (HP/LP) можуть бути спільними (один компресор). Можливо hybrid: per-zone temperature alarms + shared pressure alarms.
2. **DataLogger:** per-zone чи один на всі зони? Рекомендую один, але з zone tag per event.
3. **Lighting:** завжди shared (один світильник на камеру), не потребує namespace.
4. **WebUI:** як показувати 2 зони? Окремі сторінки? Tabs? Потрібен окремий план (Block J).
5. **MQTT topics:** `modesp/v1/{tenant}/{device}/thermo_z2/setpoint` — generator потрібно оновити.

## Verification Strategy

| Крок | Тест | Критерій |
|---|---|---|
| A1 | `idf.py build` + all tests | 0 failures, 0 warnings |
| A2 | `python tools/generate_ui.py` + pytest | Generated enums match manifest keys |
| A3 | Host doctest | ns_key(), resolve_keys(), input_key() correct |
| A4 | Build + existing tests | Default constructors unchanged |
| A5 | Host + pytest | Thermostat single-zone = identical behavior |
| A6 | Host + pytest | Defrost single-zone = identical behavior |
| A7 | Host + pytest | Protection single-zone = identical behavior |
| A8 | **New** host tests | Multi-zone arbitration: OR compressor, coordinated defrost |
| Integration | Hardware test | 2-zone on KC868-A6: zone1 cooling, zone2 defrosting |

## Reusable Patterns

| Що | Звідки | Для чого |
|---|---|---|
| StateMetaGenerator class | `tools/generate_ui.py:1256` | ModuleKeyGenerator pattern |
| BaseModule constructor | `base_module.h:35` | Extended constructor with ns |
| equipment read_requests() | `equipment_module.cpp:247` | Template for multi-zone reads |
| features_config.h lookup | `features_config.h` | Pattern for generated constexpr arrays |
