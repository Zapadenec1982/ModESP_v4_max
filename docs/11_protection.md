# Protection Module — Alarm Monitoring & Compressor Safety

## Огляд

Protection module — незалежна система моніторингу аварій для промислового холодильного обладнання.
Пріоритет HIGH(1) — виконується ПІСЛЯ Equipment(0), ПЕРЕД Thermostat(2).

**Ключові характеристики:**
- 10 незалежних моніторів аварій
- CompressorTracker: ring buffer 30 starts, sliding 1h window, duty cycle
- RateTracker: EWMA lambda=0.3 для швидкості зміни температури
- Кумулятивний наробіток компресора (мотогодини)
- 17 persist параметрів, 38 state keys
- 2-рівнева ескалація continuous run: compressor_blocked → lockout
- Кумулятивний наробіток компресора (мотогодини)

**WebUI:** окрема вкладка "Захист" (shield icon, order: 3) з 4 картками.

## Три групи моніторів

### Група 1: Температурні (HAL / LAL)

| # | Монітор | Код | Затримка | Auto-clear | Блокування |
|---|---------|-----|----------|------------|------------|
| 1 | High Temp (HAL) | `high_temp` | high_alarm_delay хв | Так (T < high_limit) | Defrost active + post-defrost |
| 2 | Low Temp (LAL) | `low_temp` | low_alarm_delay хв | Так (T > low_limit) | — |

**High Temp (HAL):**
- Умова: `equipment.air_temp >= protection.high_limit`
- Затримка: NORMAL → PENDING (відлік) → ALARM
- Auto-clear: температура повертається нижче high_limit → ALARM → NORMAL
- Блокування: під час defrost.active=true та протягом post_defrost_delay хвилин після відтайки
- При блокуванні: pending скидається (не накопичується)

**Low Temp (LAL):**
- Умова: `equipment.air_temp <= protection.low_limit`
- Працює аналогічно HAL, але без defrost-блокування

### Група 2: Сенсорні (ERR1 / ERR2 / Door)

| # | Монітор | Код | Затримка | Auto-clear |
|---|---------|-----|----------|------------|
| 3 | Sensor1 (ERR1) | `err1` | Миттєво | Так (sensor1_ok=true) |
| 4 | Sensor2 (ERR2) | `err2` | Миттєво | Так (sensor2_ok=true) |
| 5 | Door | `door` | door_delay хв | Так (door_open=false) |

- ERR1 — обрив датчика камери (критично, найвищий пріоритет)
- ERR2 — обрив датчика випарника (інформаційно)
- Door — двері відкриті занадто довго (потрібен has_door_contact)

### Група 3: Компресорні (6-10)

| # | Монітор | Код | Умова | Auto-clear |
|---|---------|-----|-------|------------|
| 6 | Short Cycling | `short_cycle` | 3 послідовних циклів < min_compressor_run | manual_reset? ні : так |
| 7 | Rapid Cycling | `rapid_cycle` | >max_starts_hour запусків за 1 год | manual_reset? ні : так |
| 8 | Continuous Run | `continuous_run` | Безперервна робота > max_continuous_run | Так (при вимкненні) |
| 9 | Pulldown Failure | `pulldown` | ON > pulldown_timeout, T не впала > pulldown_min_drop | manual_reset? ні : так |
| 10 | Rate-of-Change | `rate_rise` | T росте > max_rise_rate протягом rate_duration | Так (rate знижується) |

**Short Cycling (#6):**
CompressorTracker відстежує тривалість кожного циклу ON. Якщо 3 послідовних цикли коротші за
`min_compressor_run` секунд — спрацьовує alarm. Лічильник short_cycle_count скидається при
нормальному циклі (≥ min_compressor_run) або після тривалого простою (10× min_compressor_run OFF).

**Rapid Cycling (#7):**
Ring buffer зберігає timestamps до 30 останніх запусків. Скользне вікно 1 година —
підраховує кількість запусків. Якщо > `max_starts_hour` — alarm.

**Continuous Run (#8) — 2-рівнева ескалація:**
Якщо компресор безперервно працює довше `max_continuous_run` хвилин — alarm + ескалація:
- **Level 1 (compressor_blocked):** примусова зупинка компресора на `forced_off_min` хв. Вентилятори продовжують працювати. Після закінчення forced off — компресор може перезапуститись.
- **Level 2 (lockout):** після `max_retries` послідовних спрацювань — перманентна блокіровка всього обладнання. Потрібен ручний reset (`protection.reset_alarms`).
- Equipment Module перевіряє `protection.compressor_blocked` та `protection.lockout` при арбітражі.

**Pulldown Failure (#9):**
Після запуску компресора фіксується початкова температура з matched baseline:
якщо evap_temp доступна — порівнюється evap_at_start vs evap_now; інакше air_at_start vs air_now.
Якщо через `pulldown_timeout` хвилин температура не впала на `pulldown_min_drop` градусів — alarm.

**Rate-of-Change (#10):**
EWMA (Exponential Weighted Moving Average) з lambda=0.3 згладжує миттєву швидкість
зміни температури. Якщо EWMA rate > `max_rise_rate` °C/хв при працюючому компресорі
протягом `rate_duration` хвилин — alarm. Блокується під час defrost + post_defrost_delay.

## CompressorTracker

```
Ring buffer: uint32_t start_timestamps[30]
             head → circular write
             count → actual entries

На кожен ON edge:
  1. Записати timestamp (esp_timer_get_time / 1000)
  2. Перевірити short cycle (last_run < min_compressor_run)
  3. Підрахувати starts в 1h window

Кожні 5 сек (DIAG_INTERVAL):
  - starts_1h = count_starts_in_window(3600000)
  - duty% = total_on_1h / window_ms * 100
  - run_time = current_run_ms / 1000 (або 0 якщо OFF)
  - compressor_hours += 5.0f / 3600.0f (якщо ON)
```

## RateTracker

```
EWMA: rate = lambda * instant_rate + (1-lambda) * ewma_rate
  lambda = 0.3
  instant_rate = (temp - prev_temp) / dt_minutes  [°C/хв]

Якщо rate > max_rise_rate && компресор ON:
  rising_duration_ms += dt
  Якщо rising_duration_ms > rate_duration_ms → ALARM
Інакше:
  rising_duration_ms = 0

Блокування: defrost.active=true || post_defrost_suppression
```

## Alarm Code Priority

Від найвищого до найнижчого:

```
lockout > comp_blocked > err1 > rate_rise > high_temp > pulldown >
short_cycle > rapid_cycle > low_temp > continuous_run > err2 > door > none
```

`protection.alarm_code` завжди показує код найвищого пріоритету серед активних аварій.

## Defrost Interaction

- **suppress_high**: High Temp + Rate alarms блокуються коли `defrost.active=true`
- **post_defrost_delay**: Після завершення відтайки, блокування продовжується на N хвилин
- При блокуванні: pending стан скидається, таймер затримки не накопичується
- Low Temp, Sensor, Door — працюють незалежно від defrost

## Manual Reset

- `protection.manual_reset = false` (default): аварії знімаються автоматично при поверненні в норму
- `protection.manual_reset = true`: аварії залишаються активними поки не скинуть вручну
- `protection.reset_alarms = true` → скидає всі 10 аварій одночасно (WebUI / API / MQTT)
- Виняток: continuous_run auto-clear після закінчення forced off period
- При permanent_lockout — тільки manual reset скидає блокування

## Таблиця параметрів (17 persist)

| Key | Default | Min | Max | Step | Unit | Опис |
|-----|---------|-----|-----|------|------|------|
| protection.high_limit | 12.0 | -50 | 99 | 0.5 | °C | Верхня межа температури |
| protection.low_limit | -35.0 | -99 | 50 | 0.5 | °C | Нижня межа температури |
| protection.high_alarm_delay | 30 | 0 | 120 | 1 | хв | Затримка аварії HAL |
| protection.low_alarm_delay | 30 | 0 | 120 | 1 | хв | Затримка аварії LAL |
| protection.door_delay | 5 | 0 | 60 | 1 | хв | Затримка аварії дверей |
| protection.manual_reset | false | — | — | — | bool | Ручне скидання аварій |
| protection.post_defrost_delay | 30 | 0 | 120 | 5 | хв | Блокування HAL після відтайки |
| protection.min_compressor_run | 120 | 30 | 600 | 10 | сек | Мін. цикл (short cycle) |
| protection.max_starts_hour | 12 | 4 | 30 | 1 | разів | Макс. запусків за годину |
| protection.max_continuous_run | 360 | 60 | 720 | 30 | хв | Макс. безперервна робота |
| protection.pulldown_timeout | 60 | 15 | 240 | 5 | хв | Таймаут зниження T |
| protection.pulldown_min_drop | 2.0 | 0.5 | 10.0 | 0.5 | °C | Мін. зниження T |
| protection.max_rise_rate | 0.5 | 0.1 | 2.0 | 0.1 | °C/хв | Макс. швидкість росту T |
| protection.rate_duration | 5 | 1 | 30 | 1 | хв | Тривалість rate alarm |
| protection.forced_off_min | 20 | 5 | 60 | 5 | хв | Тривалість примусової зупинки |
| protection.max_retries | 3 | 1 | 5 | 1 | разів | Макс. спроб перед lockout |

Додатково: `protection.compressor_hours` (float, persist, readwrite) — кумулятивні мотогодини.
Зберігається в NVS раз на годину для мінімізації зносу flash.

## Таблиця state keys (38)

### Readonly (22)

| Key | Type | Опис |
|-----|------|------|
| protection.lockout | bool | Перманентна блокіровка (Level 2 ескалації) |
| protection.compressor_blocked | bool | Примусова зупинка компресора (Level 1) |
| protection.continuous_run_count | int | Лічильник послідовних continuous run |
| protection.alarm_active | bool | Є хоча б одна аварія |
| protection.alarm_code | string | Код найвищого пріоритету |
| protection.high_temp_alarm | bool | Аварія HAL |
| protection.low_temp_alarm | bool | Аварія LAL |
| protection.sensor1_alarm | bool | Обрив датчика камери |
| protection.sensor2_alarm | bool | Обрив датчика випарника |
| protection.door_alarm | bool | Двері відкриті |
| protection.short_cycle_alarm | bool | Короткі цикли |
| protection.rapid_cycle_alarm | bool | Часті запуски |
| protection.continuous_run_alarm | bool | Безперервна робота |
| protection.pulldown_alarm | bool | Pulldown failure |
| protection.rate_alarm | bool | Rate-of-change |
| protection.compressor_starts_1h | int | Запусків за останню годину |
| protection.compressor_duty | float | Duty cycle (0-100%) |
| protection.compressor_run_time | int | Поточний час роботи (сек) |
| protection.last_cycle_run | int | Останній цикл ON (сек) |
| protection.last_cycle_off | int | Останній цикл OFF (сек) |

### Readwrite — параметри (16)

17 persist параметрів (14 в таблиці + `forced_off_min` + `max_retries` + `compressor_hours`) +
`protection.reset_alarms` (write-only trigger, не persist).

## Features

| Feature | requires_roles | controls_settings |
|---------|---------------|-------------------|
| basic_protection | [] (always_active) | high_limit, low_limit, high/low_alarm_delay, manual_reset, post_defrost_delay |
| door_protection | [door_contact] | door_delay |
| compressor_protection | [compressor] | min_compressor_run, max_starts_hour, max_continuous_run, pulldown_timeout, pulldown_min_drop |
| rate_protection | [compressor, air_temp] | max_rise_rate, rate_duration |

## MQTT Topics

**Publish (21):** всі alarm + diagnostics + escalation state keys.
**Subscribe (18):** всі readwrite параметри + reset_alarms + compressor_hours + escalation settings.

Prefix: `modesp/{device_id}/protection/`

## Діагностика (кожні 5 сек)

CompressorTracker публікує:
- `compressor_starts_1h` — скользне вікно 1 година
- `compressor_duty` — % часу роботи за вікно
- `compressor_run_time` — поточний безперервний час ON (0 якщо OFF)
- `last_cycle_run` / `last_cycle_off` — тривалість попередніх циклів
- `compressor_hours` — кумулятивний наробіток

Діагностика публікується з `track_change=false` щоб не тригерити WS broadcasts.
`compressor_hours` зберігається в NVS раз на годину (720 циклів × 5 сек).

## Changelog

- 2026-03-08 — Phase 17b: 2-рівнева ескалація continuous run (compressor_blocked → lockout), 3 bugfixes (pulldown matched baseline, short cycle idle reset, alarm_code escalation). 17 persist params, 38 state keys, 63 host tests.
- 2026-03-07 — Виправлено кількість persist params: 14→15 (compressor_hours persist, reset_alarms не persist).
- 2026-03-02 — Initial documentation. 10 monitors, CompressorTracker, RateTracker, 15 persist params.
- 2026-03-02 — Separate UI tab "Захист" (shield, order 3) with 4 cards.
