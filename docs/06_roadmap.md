# ModESP v4 — Дорожня карта розвитку

> Останнє оновлення: 2026-03-07
> Платформа: ESP32-WROOM-32, ESP-IDF v5.5, C++17 + ETL

---

## Загальне бачення

ModESP v4 — модульна платформа для промислових ESP32-контролерів
холодильного обладнання. Замінює контролери Danfoss/Dixell дешевим
ESP32 з manifest-driven архітектурою, автогенерованим WebUI
та стандартними протоколами (MQTT, Modbus).

**Цільова аудиторія:** OEM виробники (UBC Group, Modern Expo),
сервісні інженери, інтегратори.

---

## ✅ Завершені фази

### Phase 1-5c — Фундамент (завершено 2026-02-17)

Ядро (ModuleManager, SharedState, EventBus), системні сервіси
(Error, Watchdog, Logger, NVS), HAL + драйвери (DS18B20, Relay),
WiFi STA/AP, HTTP REST API (12 endpoints), WebSocket broadcast,
UI generation pipeline (manifest → 4 артефакти), pytest suite.

**Milestone M1: "Лабораторний прототип" — ДОСЯГНУТО.**

### Phase 6 — MQTT + OTA (завершено 2026-02-17)

MQTT TLS (528 рядків, esp_mqtt_client, HiveMQ Cloud), OTA upload
з dual-partition rollback, SNTP (EET-2EEST), WebUI сторінки для MQTT та OTA.

**Milestone M2: "Connected Controller" — ДОСЯГНУТО.**

### Phase 6.5 — Auto-persist settings (завершено 2026-02-17)

PersistService: boot restore NVS → SharedState, debounce 5s flush,
NVS hash-based keys (djb2). POST /api/settings з state_meta валідацією.

### Phase 7a — Svelte WebUI (завершено 2026-02-17)

Svelte 4 + Rollup, 15 widget components, tile-based Dashboard,
sidebar/bottom tabs, dark theme, 17KB gzipped. Deploy: webui/ → data/www/.

### Phase 9 — Equipment Layer + Промислові модулі (завершено 2026-02-18)

**Equipment Manager** — єдиний HAL owner з арбітражем (Protection > Defrost > Thermostat)
та інтерлоком (defrost_relay↔компресор). Compressor anti-short-cycle на output level.

**Thermostat v2** — асиметричний диференціал, state machine (STARTUP→IDLE→COOLING→SAFETY_RUN),
вент. випарника (3 режими), вент. конденсатора (затримка cond_fan_delay), Safety Run, 11 persist params.

**Defrost** — 7-фазна FSM, 3 типи (зупинка/тен/гарячий газ), 4 ініціації
(таймер/demand/комбінований/ручний), 13 persist params + 2 runtime persist.

**Protection** — 5 alarm monitors (HAL, LAL, ERR1, ERR2, Door), delayed alarms,
defrost blocking, auto-clear + manual reset, 5 persist params.

### Phase 9.5 — Audit + Bugfixes (завершено 2026-02-20)

Architecture review + business logic review vs spec_v3.
27 bugs знайдено (незалежна верифікація), 14 виправлено.
10 critical audit fixes (relay min_switch, EM anti-short-cycle, JSON escaping,
WebUI alarm banner, Dashboard). 7 quick-wins (manifest ranges, CORS, security).
Деталі: `docs/archive/BUGFIXES_VERIFIED.md`

### Phase 10.5 — Features System + Select Widgets (завершено 2026-02-20)

Features в маніфестах (requires_roles → bindings → active/inactive).
Constraints (enum_filter) для dropdown options. Select widgets з людськими labels.
FeatureResolver + FeaturesConfigGenerator (5-й артефакт). has_feature() в C++ модулях.
SelectWidget.svelte + disabled state в WebUI. 209 pytest тестів.

**Milestone M3: "Production Ready" — ДОСЯГНУТО.**

---

### Phase 11a — Danfoss-level Improvements (завершено 2026-02-21)

Алгоритми на рівні топових контролерів Danfoss EKC 202/ERC 213/AK-CC 550:
- **Night Setback** — 4 режими (off/schedule/DI/manual), зміщення setpoint, SNTP schedule
- **Post-defrost alarm suppression** — блокування HAL alarm після розморозки (0-120 хв)
- **Display during defrost** — "заморожена" температура або "-d-" на Dashboard

Equipment: night_input role + digital input. 80 state keys, 206 pytest тестів.

### Phase 11b — Multi DS18B20 + NTC/ADC + DigitalInput (завершено 2026-02-22)

Розширення HAL для підтримки кількох сенсорів та нових типів драйверів:
- **DS18B20 MATCH_ROM** — адресне читання конкретного датчика на шині (64-bit ROM address)
- **NTC driver** — аналоговий термістор через ADC1 (B-parameter equation, 12-bit)
- **DigitalInput driver** — GPIO input з 50ms debounce (door_contact, night_input)
- **HAL types** — Binding.address, GpioInputConfig, AdcChannelConfig, ресурси
- **config_service** — парсинг address, gpio_inputs, adc_channels з board.json/bindings.json
- **Equipment** — condenser_temp (NTC/DS18B20) + door_contact (DigitalInput)

4 драйвери (DS18B20, Relay, DigitalInput, NTC). 81 state keys, 206 pytest тестів.

### Phase 13a — Runtime UI Visibility (завершено 2026-02-23)

Повністю runtime-механізм для керування видимістю і доступністю UI елементів:
- **visible_when** — cards та widgets ховаються/показуються на основі state key значень (eq/neq/in)
- **requires_state (per-option disabled)** — select options завжди в ui.json, disabled якщо hardware відсутнє
- **FEATURE_TO_STATE mapping** — feature name → equipment.has_* state key (8 mappings)
- **Equipment has_* keys** — has_defrost_relay, has_cond_fan, has_door_contact, has_evap_temp, has_cond_temp, has_night_input, has_ntc_driver, has_ds18b20_driver
- **Svelte** — isVisible() utility, SelectWidget per-option disabled з hint, DynamicPage visible_when
- **Runtime ланцюг** — Bindings page → Save → Restart → equipment.has_* = true → option/card enabled

84 state keys, 178 pytest тестів.

### Phase 14 + 14a + 14b — DataLogger 6-channel (завершено 2026-02-24)

Повноцінний логер температури та подій з LittleFS:
- **6-channel dynamic logging** — air (завжди) + evap + cond + setpoint + humidity + reserved
- **TempRecord 16 bytes** — timestamp(4) + int16_t ch[6] (12 bytes), ChannelDef compile-time table
- **Append-only + rotate** — temp.bin/events.bin з auto-rotate при перевищенні retention
- **10 event types** — compressor, defrost, alarm, door, power_on (edge-detect)
- **Streaming chunked JSON v3** — GET /api/log?hours=24, dynamic channels, null for TEMP_NO_DATA
- **ChartWidget** — SVG Catmull-Rom curves, PALETTE colors, channel toggles, downsample 720 buckets
- **Setpoint dual-mode** — logged as channel → polyline; not logged → horizontal line from live state
- **Event list** — текстові мітки подій під графіком (останні 50)
- **CSV export** — client-side download (dynamic channels + events), zero ESP32 навантаження
- **Auto-migration** — старі формати (8/12 bytes) видаляються при boot

97 state keys, 264 pytest тестів. Деталі: `docs/09_datalogger.md`

### Phase 7b-c — WebUI Polish (завершено 2026-02-24)

- Light/Dark theme toggle (stores/theme.js, CSS custom properties, localStorage + prefers-color-scheme)
- i18n UA/EN (~75 keys per language, derived `$t` store, ui.json text translation via `uiEn` map)
- Animations: page fly/fade, staggered card entrance, card slide collapse, value flash on change
- Real-time chart updates (appendLivePoint), Catmull-Rom smooth curves
- Bundle: 37.5→44KB gzipped (within 50KB limit). Деталі: `docs/08_webui.md`

### Phase 12a — KC868-A6 Board Support (завершено 2026-03-01)

I2C bus + PCF8574 expander підтримка в HAL:
- **pcf8574_relay driver** — relay через I2C PCF8574 (8-bit port expander)
- **pcf8574_input driver** — digital input через I2C PCF8574
- **board_kc868a6.json** — 6 реле (PCF8574 @0x24), 6 входів (PCF8574 @0x22)
- **100% backward compatible** з dev board (GPIO) — той же firmware, різний board.json
- **defrost_relay merger** — heater + hg_valve → єдиний defrost_relay role

6 драйверів (DS18B20, Relay, DigitalInput, NTC, PCF8574 Relay, PCF8574 Input).

### Heap Optimization (завершено 2026-03-01)

Виправлення фрагментації heap та оптимізація пам'яті:
- **NVS batch API** — batch_open/batch_close замість 30+ окремих nvs_open/close
- **WS broadcast interval** — 3000→1500ms (delta ~200B замість 3.5KB full state)
- **Float rounding** — roundf(T×100)/100 зменшує SharedState version bumps
- **Thermostat publish debounce** — effective_setpoint та display_temp публікуються лише при зміні
- **WS heap guard** — 16KB (full state) / 8KB (delta/ping) мінімум перед malloc
- **system.heap_largest** — моніторинг фрагментації heap (найбільший вільний блок)

### Phase 17 Phase 1 — Compressor Safety (завершено 2026-03-02)

5 нових моніторів аварій в Protection Module:
- **Short Cycling** — 3 послідовних циклів < min_compressor_run → alarm
- **Rapid Cycling** — > max_starts_hour запусків за 1 годину → alarm
- **Continuous Run** — безперервна робота > max_continuous_run → alarm
- **Pulldown Failure** — T не впала на pulldown_min_drop після pulldown_timeout → alarm
- **Rate-of-Change** — EWMA lambda=0.3, T росте > max_rise_rate → alarm
- **CompressorTracker** — ring buffer 30 starts, sliding 1h window, duty cycle
- **Motor hours** — compressor_hours (float, persist, інкремент 5 сек)
- **Дефрост інтеграція** — rate alarm блокується під час heating-фаз + post_defrost_delay
- 2 features (compressor_protection, rate_protection), 18 нових state keys
- 122 state keys, 61 STATE_META, 48 MQTT pub, 60 MQTT sub, 254 pytest + 51 host C++

### WebUI Premium Redesign R1 (завершено 2026-03-07)

Повний ребрендинг інтерфейсу до рівня промислового HMI:
- **Premium dark theme** — нові CSS токени, bento-card dashboard, unified color system
- **Card icons** — shield (Protection), flame (Defrost), thermometer (Thermostat), database (DataLogger)
- **Responsive accordions** — desktop = open, mobile (< 768px) = collapsed (GroupAccordion)
- **System & Network pages** — wide status card at top, balanced layout, card icons
- **Widget grouping** — same-type widgets in columns, logical ordering
- **Duplicate removal** — Compressor Diagnostics, Alarm Status, defrost.state
- **Uptime format** — HH:MM:SS
- Bundle: 63KB JS gz + 13KB CSS gz, 32 Svelte компоненти, ~120 i18n keys per language

---

## 📋 Заплановані фази

### Phase 7d — Svelte WebUI: Remaining
**Пріоритет:** НИЗЬКИЙ.

- [x] ~~Графіки температури~~ (Phase 14+14a+14b)
- [x] ~~Animations/transitions~~ (Phase 7b-c)
- [x] ~~Light/Dark theme toggle~~ (Phase 7b-c)
- [x] ~~i18n UA/EN~~ (Phase 7b-c)
- [ ] PWA / Service Worker
- [ ] CMake інтеграція (auto-build Svelte перед idf.py build)

**Оцінка:** 1 сесія

### Phase 8 — LCD/OLED Display
**Пріоритет:** СЕРЕДНІЙ — потрібний для standalone контролерів.

- [ ] Display Service (абстракція SSD1306/SH1106/ST7920)
- [ ] display_screens.h вже генерується — підключити до рендеру
- [ ] Навігація: кнопки або енкодер
- [ ] Екрани: Dashboard, Settings, Alarms, System Info

**Оцінка:** 3-4 сесії

### Phase 11c — PID + Advanced Sensors
**Пріоритет:** СЕРЕДНІЙ.

- [ ] PID регулювання (замість on/off гістерезису)
- [ ] Pressure sensor підтримка
- [x] ~~SEARCH_ROM — auto-learn DS18B20 через WebUI (scan → assign → save)~~ (Phase 11b)

**Оцінка:** 2-3 сесії

### Phase 11d — Advanced Algorithms (Danfoss-inspired)
**Пріоритет:** СЕРЕДНІЙ.

- [ ] Fan cycling (duty cycle коли компресор OFF) — ERC 213
- [ ] Adaptive Defrost (пропуск відтайки за аналізом випарника) — AK-CC 550
- [ ] Condenser overheat protection (alarm при перегріві) — ERC 214
- [ ] Pump Down перед defrost (спорожнення випарника) — AK-CC 550

**Оцінка:** 2-3 сесії

### Phase 12b — mDNS + Cloud
**Пріоритет:** НИЗЬКИЙ.

Вже реалізовано: WiFi STA, AP fallback, SNTP.
- [ ] mDNS (modesp-xxxx.local)
- [ ] MQTT auto-discovery (Home Assistant)
- [ ] Cloud connector (опціонально)

**Оцінка:** 2-3 сесії

### Phase 13 — Modbus + Industrial Protocols
**Пріоритет:** НИЗЬКИЙ (для великих інсталяцій).

- [ ] Modbus RTU (RS485) — slave mode
- [ ] Modbus TCP — slave mode
- [ ] Register map з manifest (auto-generated)

**Оцінка:** 3-4 сесії

---

## Візуальна дорожня карта

```
2026 Q1                    Q2                     Q3
─────────────────────────────────────────────────────────
 ✅ Phase 1-6.5           ✅ Phase 9             📋 Phase 8
 (фундамент, MQTT,        (4 промислові           (LCD/OLED)
  OTA, persist,             модулі)
  Svelte UI)              ✅ Phase 9.5           📋 Phase 11c-d
                           (audit + bugfixes)      (PID, advanced)
 M1 ✅  M2 ✅             ✅ Phase 10.5
                           (features system)      📋 Phase 12b-13
                          ✅ Phase 11a-b            (mDNS, Modbus)
                            (Danfoss + sensors)
                          ✅ Phase 13a
                            (runtime UI)
                          ✅ Phase 14+14b
                            (DataLogger 6-ch)
                          ✅ Phase 7b-c
                            (theme, i18n, anim)
                            M3 ✅
                          ✅ Phase 12a
                            (KC868-A6)
─────────────────────────────────────────────────────────
```

---

## Milestones

| # | Назва | Фази | Статус |
|---|-------|------|--------|
| M1 | Лабораторний прототип | 1-5c | ✅ 2026-02-17 |
| M2 | Connected Controller | 6 | ✅ 2026-02-17 |
| M3 | Production Ready | 9 + 9.5 + 10.5 | ✅ 2026-02-20 |
| M4 | Industrial Grade | 12a + 8 + 13 | 🔄 Частково (12a ✅, 17Ph1 ✅) |

---

## Принципи розвитку

1. **Manifest-first** — нова функціональність починається з manifest schema
2. **Zero heap в hot path** — ETL замість STL, constexpr де можливо
3. **Генератор як bridge** — Python зв'язує manifest з C++ та Web
4. **Документація = частина продукту** — оновлюється з кодом
5. **Тести перед фічами** — тести пишуться разом з кодом

---

## Changelog
- v26.0 (2026-03-07) — WebUI Premium Redesign R1 DONE + ревізія документації. 63KB JS gz, responsive accordions, card icons, System/Network pages.
- v25.0 (2026-03-02) — Phase 17 Phase 1 DONE: Compressor Safety. 5 alarms, CompressorTracker, RateTracker, motor hours. 122 state keys, 61 META.
- v24.0 (2026-03-01) — Рефакторинг документації: замінено Danfoss абревіатури на людські назви, виправлено одиниці параметрів, додано відсутні HTTP endpoints
- v23.0 (2026-03-01) — Phase 12a DONE: KC868-A6, defrost_relay merger, heap optimization. 6 drivers, 53 STATE_META, 52 MQTT sub.
- v22.0 (2026-02-24) — Phase 14b DONE: 6-channel DataLogger, TempRecord 16 bytes, ChannelDef table, JSON v3. 97 state keys, 264 tests.
- v21.0 (2026-02-24) — Phase 7b-c DONE: Light/dark theme, i18n UA/EN, Svelte transitions. Bundle 44KB gz.
- v20.0 (2026-02-24) — Phase 14+14a DONE: DataLogger + multi-channel. 95 state keys, 207 tests.
- v18.0 (2026-02-23) — Phase 13a DONE: Runtime UI visibility. 84 state keys, 178 tests.
- v17.0 (2026-02-22) — Phase 11b DONE: Multi DS18B20 (MATCH_ROM), NTC/ADC driver, DigitalInput C++ driver. 5 drivers, 81 state keys, 206 tests.
- v16.0 (2026-02-21) — Phase 11a DONE: Night Setback, Post-defrost suppression, Display during defrost.
- v15.0 (2026-02-21) — Повне перезаписування: стиснуто завершені фази, видалено дублювання з CLAUDE.md.
- v14.0 (2026-02-20) — Phase 10.5 + 9.5 DONE. M3 ДОСЯГНУТО. 209 tests, 5 artifacts.
- v12.0 (2026-02-18) — Phase 9 COMPLETE: 4 industrial modules.
- v8.0 (2026-02-17) — Phase 6+6.5+7a DONE. M1+M2 ДОСЯГНУТО.
- v3.0 (2026-02-16) — Створено.
