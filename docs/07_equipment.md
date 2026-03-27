# ModESP v4 — Equipment Manager & Protection

## Equipment Manager (equipment_module)

### Архітектура

Equipment Manager (EM) — єдиний модуль з прямим доступом до HAL drivers (ISensorDriver,
IActuatorDriver). Бізнес-модулі (Thermostat, Defrost, Protection) спілкуються з обладнанням
виключно через SharedState.

- **Priority:** CRITICAL (0) — ініціалізується першим, оновлюється першим
- **Bind:** `bind_drivers(DriverManager&)` — знаходить drivers за role name з bindings.json
- **Обов'язкові drivers:** `air_temp` (sensor), `compressor` (actuator)
- **Опціональні drivers:** evap_temp, condenser_temp, defrost_relay, evap_fan, cond_fan,
  door_contact, night_input

### Цикл on_update (кожна ітерація main loop)

```
1. read_sensors()      — читає сенсори (з EMA фільтром) → SharedState
2. read_requests()     — читає req.* від бізнес-модулів з SharedState
3. apply_arbitration() — арбітраж + інтерлоки → визначає бажані outputs
4. apply_outputs()     — застосовує outputs до relay (в тому самому циклі!)
5. publish_state()     — публікує ФАКТИЧНИЙ стан актуаторів (get_state())
```

Порядок 3-4 критичний: arbitration визначає outputs, apply одразу їх застосовує.
Попередній порядок (apply першим) створював осциляцію через однотактову затримку.

### Читання сенсорів

- **EMA (Exponential Moving Average)** фільтр з коефіцієнтом `equipment.filter_coeff` (0-10)
  - alpha = 1 / (coeff + 1); coeff=0 означає фільтр вимкнено (alpha=1.0)
  - Конфігурується через `equipment.filter_coeff` (persist, MQTT subscribe)
- **Float rounding:** `roundf(T * 100.0f) / 100.0f` — округлення до 0.01°C
  - Зменшує кількість SharedState version bumps (WsService не відправляє дані без зміни)
- **Оптимістична ініціалізація:** sensor1_ok = sensor2_ok = true при старті
  - DS18B20 потребує ~750ms на першу конверсію, перші read() фейляться
  - Запобігає хибному ERR1/ERR2 від Protection до першого успішного зчитування
- **Здоров'я датчика:** `is_healthy()` (consecutive_errors tracking у драйвері)

### Арбітраж запитів

Кожен цикл EM читає requests від трьох бізнес-модулів і визначає фінальний стан виходів:

| Пріоритет | Джерело | Умова | Дія |
|-----------|---------|-------|-----|
| 1 (найвищий) | Protection | `protection.lockout = true` | Все OFF |
| 2 | Defrost | `defrost.active = true` | defrost.req.* визначають outputs |
| 3 (найнижчий) | Thermostat | нормальний режим | thermostat.req.* визначають outputs |

**SharedState keys, що читаються (requests):**

| Key | Джерело | Опис |
|-----|---------|------|
| `thermostat.req.compressor` | Thermostat | Запит на компресор |
| `thermostat.req.evap_fan` | Thermostat | Запит на вент. випарника |
| `thermostat.req.cond_fan` | Thermostat | Запит на вент. конденсатора |
| `defrost.active` | Defrost | Defrost активний (пріоритетний режим) |
| `defrost.req.compressor` | Defrost | Запит на компресор (FAD фаза) |
| `defrost.req.defrost_relay` | Defrost | Запит на реле відтайки |
| `defrost.req.evap_fan` | Defrost | Запит на вент. випарника |
| `defrost.req.cond_fan` | Defrost | Запит на вент. конденсатора |
| `protection.lockout` | Protection | Аварійна зупинка (зарезервовано) |

### Compressor anti-short-cycle (output-level)

Захищає компресор незалежно від джерела запиту (thermostat або defrost).
Працює на фактичному стані реле, а не на запитах бізнес-модулів.

| Константа | Значення | Опис |
|-----------|----------|------|
| `COMP_MIN_OFF_MS` | 180000 (3 хв) | Мінімальний час компресор OFF |
| `COMP_MIN_ON_MS` | 120000 (2 хв) | Мінімальний час компресор ON |

- Запит ON блокується, якщо компресор був OFF менше 3 хвилин
- Запит OFF блокується (тримає ON), якщо компресор був ON менше 2 хвилин
- Доповнює (не замінює) таймери thermostat (ті працюють для state machine логіки)
- Таймер `comp_since_ms_` скидається при зміні фактичного стану реле
- Виконується ДО інтерлоків, щоб інтерлоки мали фінальне слово

### Інтерлоки (hardcoded, неможливо обійти)

Виконуються останніми в apply_arbitration() — мають найвищий пріоритет після lockout:

1. **Електрична відтайка (defrost_relay) + компресор НІКОЛИ одночасно** — якщо обидва ON
   і `defrost.type == 1` (електричний тен), компресор вимикається.
   Для `defrost.type == 2` (гарячий газ) обидва ON — це нормально,
   бо компресор потрібен для роботи циклу ГГ.

### Safe Mode

При отриманні повідомлення `SYSTEM_SAFE_MODE` від MessageBus:
- Всі outputs скидаються в OFF (`out_ = {}`)
- Негайно застосовується до relay
- Публікується фактичний стан

### equipment.has_* state keys

При ініціалізації EM публікує наявність опціонального обладнання та тип драйверів.
Ці keys використовуються для runtime UI visibility (visible_when) та per-option disabled
(requires_state) у WebUI.

**Наявність обладнання (за підключеними drivers):**

| State key | Тип | Опис | Визначається |
|-----------|-----|------|--------------|
| `equipment.has_defrost_relay` | bool | Реле відтайки доступне | `defrost_relay_ != nullptr` |
| `equipment.has_cond_fan` | bool | Вент. конденсатора доступний | `cond_fan_ != nullptr` |
| `equipment.has_door_contact` | bool | Контакт дверей доступний | `door_sensor_ != nullptr` |
| `equipment.has_evap_temp` | bool | Датчик випарника доступний | `sensor_evap_ != nullptr` |
| `equipment.has_cond_temp` | bool | Датчик конденсатора доступний | `sensor_cond_ != nullptr` |
| `equipment.has_night_input` | bool | Вхід нічного режиму доступний | `night_sensor_ != nullptr` |

**Тип драйверів (для visibility карток налаштувань в UI):**

| State key | Тип | Опис | Визначається |
|-----------|-----|------|--------------|
| `equipment.has_ntc_driver` | bool | Використовується NTC драйвер | `ISensorDriver::type() == "ntc"` для будь-якого сенсора |
| `equipment.has_ds18b20_driver` | bool | Використовується DS18B20 драйвер | `ISensorDriver::type() == "ds18b20"` для будь-якого сенсора |

Визначення типу драйвера: on_init() ітерує sensor_air_, sensor_evap_, sensor_cond_
та перевіряє `type()` кожного — якщо хоча б один повертає "ntc" або "ds18b20",
відповідний has_*_driver встановлюється в true.

Runtime ланцюг: Bindings page -> Save -> Restart -> equipment.has_* = true -> option enabled / card visible.

### Hardware roles (requires section маніфесту)

| Role | Type | Driver | Optional | Опис |
|------|------|--------|----------|------|
| `air_temp` | sensor | ds18b20, ntc | **Ні** | Температура камери |
| `compressor` | actuator | relay, pcf8574_relay | **Ні** | Компресор |
| `evap_temp` | sensor | ds18b20, ntc | Так | Температура випарника |
| `condenser_temp` | sensor | ds18b20, ntc | Так | Температура конденсатора |
| `defrost_relay` | actuator | relay, pcf8574_relay | Так | Реле відтайки (тен або клапан ГГ) |
| `evap_fan` | actuator | relay, pcf8574_relay | Так | Вентилятор випарника |
| `cond_fan` | actuator | relay, pcf8574_relay | Так | Вентилятор конденсатора |
| `door_contact` | sensor | digital_input, pcf8574_input | Так | Контакт дверей |
| `night_input` | sensor | digital_input, pcf8574_input | Так | Вхід нічного режиму |

### State keys (повна таблиця)

**Read-only (значення сенсорів та стан актуаторів):**

| Key | Type | Unit | Опис |
|-----|------|------|------|
| `equipment.air_temp` | float | °C | Температура камери |
| `equipment.evap_temp` | float | °C | Температура випарника |
| `equipment.cond_temp` | float | °C | Температура конденсатора |
| `equipment.sensor1_ok` | bool | - | Датчик камери справний |
| `equipment.sensor2_ok` | bool | - | Датчик випарника справний |
| `equipment.compressor` | bool | - | Фактичний стан компресора |
| `equipment.defrost_relay` | bool | - | Фактичний стан реле відтайки |
| `equipment.evap_fan` | bool | - | Фактичний стан вент. випарника |
| `equipment.cond_fan` | bool | - | Фактичний стан вент. конденсатора |
| `equipment.door_open` | bool | - | Стан дверей |
| `equipment.night_input` | bool | - | Дискретний вхід нічного режиму |
| `equipment.has_defrost_relay` | bool | - | Реле відтайки доступне (runtime) |
| `equipment.has_cond_fan` | bool | - | Вент. конденсатора доступний |
| `equipment.has_door_contact` | bool | - | Контакт дверей доступний |
| `equipment.has_evap_temp` | bool | - | Датчик випарника доступний |
| `equipment.has_cond_temp` | bool | - | Датчик конденсатора доступний |
| `equipment.has_night_input` | bool | - | Вхід нічного режиму доступний |
| `equipment.has_ntc_driver` | bool | - | Використовується NTC драйвер |
| `equipment.has_ds18b20_driver` | bool | - | Використовується DS18B20 драйвер |

**Readwrite (налаштування, persist в NVS):**

| Key | Type | Unit | Min | Max | Step | Default | Опис |
|-----|------|------|-----|-----|------|---------|------|
| `equipment.filter_coeff` | int | - | 0 | 10 | 1 | 4 | Цифровий фільтр EMA (0=вимкнено) |
| `equipment.ntc_beta` | int | - | 2000 | 5000 | 1 | 3950 | B-коефіцієнт NTC |
| `equipment.ntc_r_series` | int | Ом | 1000 | 100000 | 100 | 10000 | Послідовний резистор NTC |
| `equipment.ntc_r_nominal` | int | Ом | 1000 | 100000 | 100 | 10000 | Номінальний опір NTC (25°C) |
| `equipment.ds18b20_offset` | float | °C | -5.0 | 5.0 | 0.1 | 0.0 | Корекція показань DS18B20 |

### MQTT

**Publish:** equipment.air_temp, equipment.evap_temp, equipment.cond_temp,
equipment.compressor, equipment.defrost_relay, equipment.sensor1_ok

**Subscribe:** equipment.ntc_beta, equipment.ntc_r_series, equipment.ntc_r_nominal,
equipment.ds18b20_offset, equipment.filter_coeff

---

## Protection Module (protection_module)

### Архітектура

Protection module моніторить 10 незалежних аварій і публікує стан через SharedState.
Не зупиняє обладнання напряму — `protection.lockout` зарезервований (завжди false).

- **Priority:** HIGH (1) — оновлюється ПІСЛЯ Equipment (0), ДО Thermostat (2)
- **Inputs:** читає equipment.air_temp, equipment.sensor1_ok, equipment.sensor2_ok,
  equipment.door_open, equipment.compressor, equipment.evap_temp, defrost.active,
  defrost.phase з SharedState
- **Features:** 4 features (basic_protection, door_protection, compressor_protection, rate_protection)

### 10 незалежних моніторів аварій

Кожен монітор має структуру AlarmMonitor: `active` (аварія зараз), `pending` (в затримці),
`pending_ms` (час в pending стані).

**Група 1 — Температурні:**

| Монітор | Код аварії | Тип затримки | Параметр затримки | Блокування defrost |
|---------|------------|--------------|-------------------|--------------------|
| High Temp (HAL) | `high_temp` | delayed | `high_alarm_delay` (хв) | Так (heating-фази + post-defrost) |
| Low Temp (LAL) | `low_temp` | delayed | `low_alarm_delay` (хв) | Ні |

**Група 2 — Сенсорні:**

| Монітор | Код аварії | Тип затримки | Параметр затримки |
|---------|------------|--------------|-------------------|
| Sensor1 (ERR1) | `err1` | instant | - |
| Sensor2 (ERR2) | `err2` | instant | - |
| Door | `door` | delayed | `door_delay` (хв) |

**Група 3 — Компресорні (Phase 17):**

| # | Монітор | Код аварії | Умова | Auto-clear |
|---|---------|------------|-------|------------|
| 6 | Short Cycling | `short_cycle` | 3 послідовних цикли < min_compressor_run | Так |
| 7 | Rapid Cycling | `rapid_cycle` | > max_starts_hour запусків за 1 год | Так |
| 8 | Continuous Run | `continuous_run` | Робота > max_continuous_run хв | Так (при OFF) |
| 9 | Pulldown Failure | `pulldown` | T не впала на pulldown_min_drop за pulldown_timeout | Так |
| 10 | Rate-of-Change | `rate_rise` | EWMA rate > max_rise_rate за rate_duration | Так |

### Алгоритм delayed alarm (High Temp, Low Temp, Door)

```
if (condition exceeded) {
    pending = true
    pending_ms += dt_ms
    if (pending_ms >= delay_ms) → active = true
} else {
    pending = false, pending_ms = 0
    if (active && !manual_reset) → active = false  // auto-clear
}
```

### Алгоритм instant alarm (Sensor1, Sensor2)

```
if (!sensor_ok && !active)  → active = true    // аварія негайно
if (sensor_ok && active && !manual_reset) → active = false  // auto-clear
```

### CompressorTracker (Phase 17)

Ring buffer із 30 timestamp запусків компресора (oldest evicted при переповненні).
На кожен ON edge: запис timestamp, перевірка short cycle, підрахунок starts у 1h window.

Діагностика публікується кожні 5 сек:
- `protection.compressor_starts_1h` — скользне вікно 1 год
- `protection.compressor_duty` — duty cycle %
- `protection.compressor_run_time` — поточний час ON (0 якщо OFF)
- `protection.last_cycle_run` / `protection.last_cycle_off` — тривалість попередніх циклів
- `protection.compressor_hours` — кумулятивний наробіток (persist, зберігається раз на годину)

### RateTracker (Phase 17)

EWMA (lambda=0.3) згладжує миттєву швидкість зміни T. Якщо rate > `max_rise_rate` при
працюючому компресорі протягом `rate_duration` хвилин → alarm. Блокується під час
defrost.active та post_defrost_delay.

### Defrost blocking

High Temp + rate_rise алarms блокуються у два етапи:

1. **Під час heating-фаз defrost** (stabilize, valve_open, active, equalize):
   - pending скидається, нові аварії не ставляться в чергу
2. **Post-defrost suppression** (після завершення defrost):
   - Таймер `post_defrost_delay` (хвилини, default 30)
   - Дозволяє температурі стабілізуватися після відтайки

### Auto-clear vs Manual reset

- **Auto-clear** (manual_reset = false, default): аварія знімається автоматично при поверненні умови в норму
- **Manual reset** (manual_reset = true): потрібна команда `protection.reset_alarms = true` через WebUI або API
- Виняток: continuous_run auto-clear при вимкненні компресора навіть в manual режимі

### Пріоритет alarm_code (11 рівнів)

```
err1 > rate_rise > high_temp > pulldown > short_cycle > rapid_cycle >
low_temp > continuous_run > err2 > door > none
```

### State keys (повна таблиця)

**Read-only (статус):**

| Key | Type | Опис |
|-----|------|------|
| `protection.lockout` | bool | Аварійна зупинка (зарезервовано, завжди false) |
| `protection.alarm_active` | bool | Є хоча б одна активна аварія |
| `protection.alarm_code` | string | Код найвищої аварії (11 варіантів) |
| `protection.high_temp_alarm` | bool | Аварія верхньої температури |
| `protection.low_temp_alarm` | bool | Аварія нижньої температури |
| `protection.sensor1_alarm` | bool | Обрив датчика камери |
| `protection.sensor2_alarm` | bool | Обрив датчика випарника |
| `protection.door_alarm` | bool | Двері відкриті занадто довго |
| `protection.short_cycle_alarm` | bool | Короткі цикли компресора |
| `protection.rapid_cycle_alarm` | bool | Часті запуски компресора |
| `protection.continuous_run_alarm` | bool | Безперервна тривала робота |
| `protection.pulldown_alarm` | bool | Відмова відтягування температури |
| `protection.rate_alarm` | bool | Швидке зростання температури |
| `protection.compressor_starts_1h` | int | Запусків за останню годину |
| `protection.compressor_duty` | float | Duty cycle компресора (%) |
| `protection.compressor_run_time` | int | Поточний час роботи (сек) |
| `protection.last_cycle_run` | int | Тривалість останнього циклу ON (сек) |
| `protection.last_cycle_off` | int | Тривалість останнього циклу OFF (сек) |

**Readwrite (налаштування, persist в NVS — 15 параметрів):**

| Key | Type | Unit | Min | Max | Step | Default | Опис |
|-----|------|------|-----|-----|------|---------|------|
| `protection.high_limit` | float | °C | -50.0 | 99.0 | 0.5 | 12.0 | Верхня межа температури |
| `protection.low_limit` | float | °C | -99.0 | 50.0 | 0.5 | -35.0 | Нижня межа температури |
| `protection.high_alarm_delay` | int | хв | 0 | 120 | 1 | 30 | Затримка аварії високої T |
| `protection.low_alarm_delay` | int | хв | 0 | 120 | 1 | 30 | Затримка аварії низької T |
| `protection.door_delay` | int | хв | 0 | 60 | 1 | 5 | Затримка аварії дверей |
| `protection.manual_reset` | bool | - | - | - | - | false | Ручне скидання (false=auto) |
| `protection.post_defrost_delay` | int | хв | 0 | 120 | 5 | 30 | Suppress HAL alarm після defrost |
| `protection.min_compressor_run` | int | сек | 30 | 600 | 10 | 120 | Мін. тривалість циклу ON |
| `protection.max_starts_hour` | int | разів | 4 | 30 | 1 | 12 | Макс. запусків за годину |
| `protection.max_continuous_run` | int | хв | 60 | 720 | 30 | 360 | Макс. безперервна робота |
| `protection.pulldown_timeout` | int | хв | 15 | 240 | 5 | 60 | Таймаут зниження T |
| `protection.pulldown_min_drop` | float | °C | 0.5 | 10.0 | 0.5 | 2.0 | Мін. зниження T |
| `protection.max_rise_rate` | float | °C/хв | 0.1 | 2.0 | 0.1 | 0.5 | Макс. швидкість росту T |
| `protection.rate_duration` | int | хв | 1 | 30 | 1 | 5 | Тривалість rate alarm |
| `protection.compressor_hours` | float | год | 0 | 999999 | 1 | 0 | Кумулятивні мотогодини |

**Readwrite (команда):**

| Key | Type | Опис |
|-----|------|------|
| `protection.reset_alarms` | bool | Write-only trigger скидання всіх аварій |

### Features

| Feature | Always active | Requires roles | Controls settings |
|---------|---------------|----------------|-------------------|
| `basic_protection` | Так | - | high_limit, low_limit, high/low_alarm_delay, manual_reset, post_defrost_delay |
| `door_protection` | Ні | door_contact | door_delay |
| `compressor_protection` | Ні | compressor | min_compressor_run, max_starts_hour, max_continuous_run, pulldown_timeout, pulldown_min_drop |
| `rate_protection` | Ні | compressor, air_temp | max_rise_rate, rate_duration |

### MQTT

**Publish:** protection.lockout, protection.alarm_active, protection.alarm_code,
protection.high_temp_alarm, protection.low_temp_alarm, protection.sensor1_alarm,
protection.sensor2_alarm, protection.door_alarm, protection.short_cycle_alarm,
protection.rapid_cycle_alarm, protection.continuous_run_alarm, protection.pulldown_alarm,
protection.rate_alarm, protection.compressor_starts_1h, protection.compressor_duty,
protection.compressor_run_time, protection.last_cycle_run, protection.last_cycle_off,
protection.compressor_hours

**Subscribe:** protection.high_limit, protection.low_limit, protection.high_alarm_delay,
protection.low_alarm_delay, protection.door_delay, protection.manual_reset,
protection.post_defrost_delay, protection.reset_alarms, protection.min_compressor_run,
protection.max_starts_hour, protection.max_continuous_run, protection.pulldown_timeout,
protection.pulldown_min_drop, protection.max_rise_rate, protection.rate_duration,
protection.compressor_hours

---

## Порядок виконання в main loop

```
Equipment (priority 0)  → читає сенсори, арбітраж, публікує стан
Protection (priority 1) → моніторить аварії, публікує alarm state
Thermostat (priority 2) → регулювання, публікує req.compressor/fan
Defrost (priority 2)    → цикл відтайки, публікує req.defrost_relay
```

Equipment завжди оновлюється першим, бо Protection та Thermostat залежать від
свіжих значень equipment.air_temp та equipment.sensor1_ok. Protection оновлюється
до Thermostat, бо Thermostat перевіряє protection.lockout.

## Changelog

- 2026-03-07 — Phase 17 (Compressor Safety): розширено Protection до 10 моніторів, CompressorTracker, RateTracker, 15 persist params, 4 features, alarm priority 11 рівнів
- 2026-03-01 — Рефакторинг: defrost_relay merger (heater+hg_valve->defrost_relay), додано NTC/DS18B20 settings, has_* keys оновлено, EMA filter + rounding, pcf8574_relay/pcf8574_input drivers
- 2026-02-25 — Створено: Equipment Manager + Protection документація
