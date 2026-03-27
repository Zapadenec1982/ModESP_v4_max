# ModESP v4 — Дорожня карта розвитку (Firmware Phases F-9+)

> Останнє оновлення: 2026-03-10
> На основі конкурентного аналізу SaaS ринку холодильного обладнання
> та аудиту невикористаних апаратних ресурсів ESP32

---

## Поточний стан прошивки

**Production ✅** — 1 пристрій (F27FCD) підключений до ModESP Cloud через MQTT TLS.

### Реалізовано
| Компонент | Деталі |
|-----------|--------|
| Температура | DS18B20 (OneWire, MATCH_ROM) + NTC (ADC, B-parameter) |
| Реле | 6× PCF8574 relay (compressor, defrost, evap_fan, cond_fan, spare×2) |
| Входи | 6× PCF8574 digital input (door, night_mode, spare×4) |
| Термостат | Асиметричний диференціал, 3 режими вентилятора, нічний setback, safety run |
| Відтайка | 7-фазна FSM, 3 типи (stop/electric/hot gas), 4 ініціації, demand temp |
| Захист | 10 алармів (HAL/LAL/sensor/door/short_cycle/rapid/continuous/pulldown/rate) |
| CompressorTracker | Ring buffer 30 starts, duty cycle, motor hours (persist NVS) |
| DataLogger | 6-channel LittleFS, SVG chart, CSV export, event recording |
| MQTT | TLS, v1 protocol (48 pub + 60 sub keys), heartbeat 30s |
| OTA | MQTT-triggered, HTTP download, SHA256, board validation, rollback |
| WebUI | Svelte 4, dark/light theme, i18n UA/EN, 63KB gzipped |

### Невикористані апаратні ресурси
| Ресурс | Деталі | Потенціал |
|--------|--------|-----------|
| ADC × 4 | GPIO 36, 39, 34, 35 (12-bit, 0-3.3V) | Датчик струму, тиску, рівня |
| OneWire bus 2 | GPIO 33 | Додаткові DS18B20 |
| I2C expander pins | 2 free relay + 2 free input на PCF8574 | Доп. реле/входи |
| SPI × 3 | Не використано | Дисплей, SD карта |
| UART × 2 | UART1, UART2 (UART0 = console) | Modbus RS-485, GPS |
| LEDC PWM × 8 | Не використано | Плавне регулювання, buzzer |
| CAN/TWAI | 1 контролер | Industrial fieldbus |
| Free DRAM | ~170KB | Нові алгоритми, буфери |

---

## Чого не вистачає (vs конкуренти)

Аналіз 10 SaaS платформ виявив ключові функції, які конкуренти реалізують зовнішніми сенсорами та cloud ML, але які ModESP_v4 може реалізувати **на edge** з існуючим hardware:

| Функція | Конкуренти | ModESP v4 потенціал | Пріоритет |
|---------|-----------|-------------------|-----------|
| Енергомоніторинг (kWh) | Axiom, KLATU, SmartSense (зовнішні лічильники) | ADC + SCT-013 ($3) | 🔴 Високий |
| Compressor health scoring | KLATU (8 патентів, cloud ML) | Edge: duty deviation + COP indicator | 🔴 Високий |
| Adaptive defrost | Danfoss AK-CC 550 (T_evap analysis) | Edge: ефективність відтайок → авто-інтервал | 🔴 Високий |
| Condenser monitoring | Axiom (Digital Twin, cloud) | Edge: ΔT trend → alarm "конденсатор брудний" | 🟡 Середній |
| Predictive alerts | KLATU (TRAXX EKG), Axiom (Virtual Tech) | Edge: statistical SPC, не ML | 🟡 Середній |
| Дисплей | Danfoss/Dixell (вбудований) | SPI + ST7789/ILI9341 + LVGL | 🟡 Середній |
| Modbus | Danfoss (native), Carel (native) | UART + RS-485 transceiver | 🟡 Середній |
| Вологість камери | SmartSense, Monnit (compliance logging) | I2C SHT31/AHT20 ($2-4 чіп, $15-30 probe) | ⚪ Низький |

**Ключовий інсайт:** ModESP_v4 вже збирає більше даних (CompressorTracker, RateTracker, defrost phases) ніж більшість конкурентів. Потрібно лише **витягнути інсайти з цих даних на edge**.

> **Примітка щодо вологості:** Більшість промислових контролерів (Danfoss, Carel, Dixell, Eliwell)
> **не використовують** датчик вологості для керування відтайкою чи термостатом. Конкуренти
> (SmartSense, Monnit) використовують humidity виключно для **compliance logging** (HACCP/FSMA),
> а не як input для алгоритмів. Adaptive defrost industry standard — аналіз T_evap, не RH.
> Детально: секція F-9.2.

---

## Фаза F-9: Sensors Expansion + Edge Analytics

**Ціль:** Нові датчики для energy monitoring, door analytics, edge intelligence.
**Тривалість:** 2-3 тижні (F-9.1, F-9.3, F-9.4 — основні; F-9.2 — опціональний, низький пріоритет)

### F-9.1. Датчик струму компресора (ADC + SCT-013)

**Hardware:**
```
SCT-013-030 (split-core CT, 30A → 1V output)
  → voltage divider (bias 1.65V для АЦП ESP32)
  → ADC1 GPIO 36 (вже сконфігурований в board.json)
Вартість: ~$3-5 за датчик
Монтаж: надівається на провід компресора, без розриву кола
```

**Firmware:**
- [ ] Новий HAL driver: `current_sensor` (тип: `adc_current`)
  - Sampling: 1kHz протягом 20ms (1 повний цикл 50Hz) кожні 5 секунд
  - RMS calculation: `I_rms = sqrt(sum(samples²) / N) × calibration_factor`
  - Калібрувальний коефіцієнт: configurable через manifest (default: 30.0 для SCT-013-030)
  - Anti-alias: пропуск перших 2ms після початку sampling
- [ ] Board config: `adc_channels` entry з типом `current` та calibration параметрами
- [ ] Bindings: `role: compressor_current` → module: `equipment`
- [ ] Equipment module: нові keys:
  ```
  equipment.compressor_current    (float, A, read-only)
  equipment.power_watts           (float, W = I × V_rated)
  equipment.energy_kwh            (float, cumulative, persist NVS)
  equipment.energy_today_kwh      (float, reset щодня о 00:00)
  ```
- [ ] Config params (manifest):
  ```
  energy.rated_voltage     (int, V, default: 220, persist)
  energy.ct_ratio          (float, default: 30.0, persist)
  energy.power_factor      (float, default: 0.85, persist)
  ```
- [ ] MQTT publish: 4 нових state keys (delta, як існуючі)
- [ ] Feature: `energy_monitoring` → `equipment.has_energy_monitoring`

**RAM:** ~2KB (ADC DMA buffer + RMS accumulator)
**Flash:** ~3KB code
**Складність:** Низька — ADC driver вже є для NTC, потрібен лише RMS калькулятор

---

### F-9.2. Датчик вологості (I2C SHT31/AHT20) — ОПЦІОНАЛЬНИЙ, НИЗЬКИЙ ПРІОРИТЕТ

> **⚠️ Чесна оцінка:** Більшість промислових контролерів (Danfoss EKC/AK, Carel MasterCella,
> Dixell XR, Eliwell EWRC) **не мають** входу для датчика вологості. Це не тому що вони
> "відстали" — а тому що T_evap analysis дає достатньо інформації для adaptive defrost,
> а humidity sensor має серйозні проблеми надійності в холодильних камерах.
>
> Конкуренти (SmartSense, Monnit) використовують humidity виключно для **HACCP compliance
> logging**, а не як input для алгоритмів керування обладнанням.

**Доцільні сценарії:**

| Сценарій | Потрібен humidity? | Пояснення |
|----------|:------------------:|-----------|
| Морозильні камери (-18°C і нижче) | ❌ | Абсолютна вологість мізерна, sensor unreliable |
| Середньотемпературні (+2..+8°C) | 🟡 | Корисно для product quality monitoring |
| Торгові вітрини з підігрівом скла | ✅ | Anti-condensation control = реальна економія 30-50% |
| HACCP compliance (food service) | ✅ | Regulatory requirement, просто logging |
| Adaptive defrost input | ❌ | T_evap analysis — industry standard (Danfoss AK-CC 550) |

**Проблеми надійності в холодильних камерах:**

| Проблема | Деталі |
|----------|--------|
| Конденсат на сенсорі | Під час відтайки T різко зростає → волога конденсується → хибні 99-100% RH на 15-30 хв |
| Обмерзання | При T < 0°C вода на сенсорі замерзає → фізичне пошкодження мембрани |
| Деградація точності | SHT31: ±2% RH при 25°C, але ±4-5% при -20°C. Drift 1-2% RH/рік в агресивному середовищі |
| Реальна ціна | Чіп $2-4, але промисловий probe в IP67 з кабелем — $15-30. Заміна кожні 2-3 роки |

**Hardware:**
```
SHT31-DIS (Sensirion, I2C addr 0x44) — ±2% RH при 25°C
  або AHT20 (ASAIR, I2C addr 0x38) — бюджетний, ±3% RH
  → I2C bus 0 (GPIO 4 SDA, GPIO 15 SCL, 100kHz)
Вартість: $2-4 чіп, $15-30 промисловий probe в IP67
Монтаж: всередині камери, якнайдалі від випарника
Термін служби: 2-3 роки (vs 5-10+ років для DS18B20)
```

**Firmware:**
- [ ] Новий HAL driver: `i2c_humidity_sensor`
  - Підтримка SHT31 (0x44, CRC-8) та AHT20 (0x38, CRC-8)
  - Auto-detect: спробувати SHT31, потім AHT20 при boot
  - Measurement: single-shot mode, 1 read per 10 seconds
  - Post-defrost filter: ігнорувати readings 15 хв після завершення відтайки (конденсат)
  - Health: error counter, unhealthy after 5 consecutive failures
- [ ] Board config: I2C device entry з типом `humidity`
- [ ] Bindings: `role: humidity_sensor` → module: `equipment`
- [ ] Equipment module: нові keys:
  ```
  equipment.humidity           (float, %RH, read-only)
  equipment.humidity_temp      (float, °C, cross-reference)
  equipment.has_humidity       (bool, feature flag)
  ```
- [ ] Protection module (якщо enabled):
  ```
  product.humidity_low_limit   (float, %RH, default: 75, persist)
  product.humidity_high_limit  (float, %RH, default: 95, persist)
  protection.low_humidity_alarm  (bool, product drying out)
  protection.high_humidity_alarm (bool, mold/condensation risk)
  ```
- [ ] DataLogger: humidity як 6-й канал (вже зарезервований у ChannelDef)
- [ ] MQTT publish: 2-4 нових state keys (залежно від alarm config)

**RAM:** ~1KB (I2C buffer + last reading + post-defrost filter)
**Flash:** ~4KB code
**Складність:** Низька технічно, але потребує field testing для порогів та post-defrost filter timing

---

### F-9.3. Door sensor (binding + metrics)

**Hardware:** Вже підтримано — магнітний контакт на PCF8574 input.

**Firmware:**
- [ ] Нові state keys для door analytics:
  ```
  equipment.door_open_count      (int, daily counter, reset 00:00)
  equipment.door_open_duration_s (int, cumulative seconds today)
  equipment.door_last_open       (int, timestamp останнього відкриття)
  ```
- [ ] Equipment module: track door open events (timestamp, duration)
- [ ] Protection module: door_alarm з configurable delay (вже є, перевірити binding)
- [ ] MQTT publish: 3 нових state keys
- [ ] DataLogger: door events (open/close з timestamp)

**RAM:** ~200B
**Складність:** Мінімальна — DigitalInput driver вже готовий

---

### F-9.4. Edge Analytics (Statistical Process Control)

**Ціль:** Базові аналітичні метрики НА КОНТРОЛЕРІ, без cloud ML.

**Firmware:**
- [ ] Новий модуль: `AnalyticsModule` (реєструється в ModuleManager)
- [ ] Ковзне середнє duty cycle (7-денне вікно):
  ```cpp
  // Circular buffer: 168 слотів (24h × 7d, 1 значення на годину)
  float duty_hourly[168];
  float duty_mean_7d;    // = avg(duty_hourly)
  float duty_stddev_7d;  // = stddev(duty_hourly)
  ```
- [ ] Duty deviation alert:
  ```
  analytics.duty_deviation   (float, %, поточний vs 7d mean)
  analytics.duty_alert       (bool, true якщо |deviation| > 20%)
  ```
- [ ] COP indicator (ефективність охолодження):
  ```
  COP_indicator = (T_air - T_evap) / duty_cycle
  Якщо COP_indicator знижується → конденсатор брудний або витік хладагенту
  analytics.cop_indicator    (float, °C/%, read-only)
  analytics.cop_trend        (int, -1/0/+1 = worse/stable/better vs 7d)
  ```
- [ ] Defrost efficiency:
  ```
  defrost_efficiency = (T_evap_end - T_evap_start) / defrost_duration_min
  Якщо знижується → нагнітання снігу, проблеми з нагрівачем
  analytics.defrost_efficiency   (float, °C/min, last defrost)
  analytics.defrost_eff_trend    (int, -1/0/+1 vs running avg)
  ```
- [ ] Нові state keys (всього 6):
  ```
  analytics.duty_deviation       (float, %)
  analytics.duty_alert           (bool)
  analytics.cop_indicator        (float)
  analytics.cop_trend            (int)
  analytics.defrost_efficiency   (float)
  analytics.defrost_eff_trend    (int)
  ```
- [ ] MQTT publish: 6 нових state keys
- [ ] Feature: `edge_analytics` → `equipment.has_edge_analytics`

**RAM:** ~2KB (circular buffers для duty hourly + defrost history)
**Flash:** ~5KB code
**Складність:** Середня — алгоритми прості, але потрібне ретельне тестування порогів

---

## Фаза F-10: Advanced Refrigeration Algorithms

**Ціль:** Алгоритми рівня Danfoss AK-CC 550 / Carel MasterCella.
**Тривалість:** 3-4 тижні

### F-10.1. Adaptive Defrost

**Проблема:** Фіксований інтервал відтайки → зайва витрата енергії (до 30% overhead).

**Алгоритм:**
```
Після кожної відтайки:
  1. Записати: {duration, T_evap_start, T_evap_end, termination_type}
  2. Розрахувати efficiency = ΔT / duration
  3. Якщо efficiency > threshold І termination = by_temp (не timeout):
     → збільшити interval на 1 годину (до max_interval)
  4. Якщо termination = timeout (відтайка не завершилась по температурі):
     → зменшити interval на 2 години (до min_interval)
  5. Якщо 3 поспіль timeout → alarm + reset до default interval
```

**Firmware:**
- [ ] Circular buffer останніх 10 відтаєк з метриками
- [ ] Adaptive interval calculator (викликається після кожної відтайки)
- [ ] Config params:
  ```
  defrost.adaptive_enabled    (bool, default: false, persist)
  defrost.adaptive_min        (int, hours, default: 4, persist)
  defrost.adaptive_max        (int, hours, default: 24, persist)
  defrost.efficiency_threshold (float, °C/min, default: 0.5, persist)
  ```
- [ ] State keys:
  ```
  defrost.adaptive_interval   (int, поточний адаптивний інтервал, hours)
  defrost.last_efficiency     (float, °C/min, остання відтайка)
  defrost.adaptive_adjustments (int, кількість корекцій)
  ```
- [ ] MQTT publish: 3 нових state keys

**RAM:** ~500B (10-entry defrost history buffer)
**Економія:** 10-30% енергії на відтайках у типових інсталяціях.

---

### F-10.2. Condenser Health Monitor

**Проблема:** Забруднений конденсатор → вищий тиск → більше споживання → коротший ресурс компресора.

**Алгоритм:**
```
Потребує: T_cond (DS18B20 або NTC на конденсаторі)

ΔT_cond = T_cond - T_air (коли компресор працює > 10 хв)
baseline = rolling avg ΔT_cond за перші 7 днів після чистки
current = rolling avg ΔT_cond за останні 24h

degradation_pct = (current - baseline) / baseline × 100

IF degradation_pct > 30% → soft alarm "Очистіть конденсатор"
IF degradation_pct > 50% → alarm + compressor protection (знижене duty)
```

**Firmware:**
- [ ] Потребує binding: `cond_temp` role (DS18B20 або NTC) — вже підтримано в HAL
- [ ] CondenserMonitor (частина Protection або Analytics module):
  ```
  analytics.cond_delta_t         (float, °C, T_cond - T_air)
  analytics.cond_degradation     (float, %, vs baseline)
  analytics.cond_baseline        (float, °C, persist NVS)
  protection.condenser_alarm     (bool, soft alarm)
  ```
- [ ] Config:
  ```
  protection.cond_warn_threshold   (int, %, default: 30, persist)
  protection.cond_alarm_threshold  (int, %, default: 50, persist)
  analytics.cond_baseline_reset    (bool, trigger: скидає baseline)
  ```
- [ ] MQTT publish: 4 нових state keys
- [ ] Feature: `condenser_monitor` → `equipment.has_condenser_monitor`

**RAM:** ~500B (rolling average buffers)
**Потреба:** Cond temp sensor — вже підтримується, потрібен лише binding

---

### F-10.3. Smart Night Mode

**Проблема:** Фіксований розклад не враховує реальне використання.

**Алгоритм:**
```
IF night_mode == 3 (auto):
  Track door_open_count per hour (24-слотний circular buffer)
  Визначити "тихі години": послідовність годин з door_opens < threshold
  Плавний setback: +0.5°C кожні 15 хв (замість різкого +3°C)
  Плавний повернення: -0.5°C кожні 15 хв
  Learning: 7-денне ковзне середнє для паттерну door opens → auto schedule
```

**Firmware:**
- [ ] Night mode = 3 (auto) — новий режим
- [ ] Door activity tracker (24-slot hourly buffer)
- [ ] Gradual setback ramp (0.5°C step, 15min interval)
- [ ] Config:
  ```
  thermostat.night_mode = 3        (auto, додати до enum)
  thermostat.night_door_threshold  (int, opens/hour для "тихої" години, default: 2)
  thermostat.night_ramp_step       (float, °C per step, default: 0.5)
  thermostat.night_ramp_interval   (int, minutes, default: 15)
  ```
- [ ] State keys:
  ```
  thermostat.night_auto_start    (int, auto-detected start hour)
  thermostat.night_auto_end      (int, auto-detected end hour)
  thermostat.night_ramp_progress (float, current setback applied)
  ```

**RAM:** ~200B (24-slot buffer + ramp state)
**Економія:** 5-15% нічної енергії, плюс м'якший температурний режим

---

### F-10.4. Predictive Compressor Health

**Ціль:** Розширити CompressorTracker до predictive indicator.

**Алгоритм:**
```
Використати existing CompressorTracker (ring buffer 30 starts) +
optional SCT-013 (current sensor):

1. Tracking cycle efficiency:
   efficiency_per_cycle = ΔT_achieved / run_time
   IF trend знижується → "компресор деградує"

2. Start current analysis (якщо є SCT-013):
   locked_rotor_ratio = I_start_peak / I_running_avg
   IF ratio > 6× → "hard start" → recommend start capacitor/relay

3. Composite health score:
   compressor_health = 100
     - short_cycle_events × 5
     - (duty - baseline_duty) × 0.5
     - efficiency_degradation × 10
     - hard_start_events × 10
```

**Firmware:**
- [ ] Extend CompressorTracker з efficiency tracking:
  ```
  analytics.compressor_health      (int, 0-100)
  analytics.cycle_efficiency       (float, °C/min, last cycle)
  analytics.efficiency_trend       (int, -1/0/+1)
  analytics.hard_start_count       (int, за 7 днів, якщо є SCT-013)
  ```
- [ ] Feature: `compressor_health` → `equipment.has_compressor_health`
- [ ] MQTT publish: 4 нових state keys

**RAM:** ~1KB (extended CompressorTracker)
**Складність:** Середня

---

## Фаза F-11: Display + Industrial Interfaces

**Ціль:** Standalone operation та інтеграція в промислові системи.
**Тривалість:** 4-6 тижнів

### F-11.1. SPI Display (ST7789 / ILI9341) + LVGL

**Hardware:**
```
ST7789 240×320 IPS (2.4" або 2.8"), SPI interface
  → SPI2: MOSI GPIO 23, CLK GPIO 18, CS GPIO 5, DC GPIO 16, RST GPIO 17
  → Backlight: LEDC PWM GPIO 2
Вартість: $3-8 за дисплей
```

**Firmware:**
- [ ] SPI display driver (esp_lcd API від ESP-IDF)
- [ ] LVGL v9 integration (tick timer, flush callback)
- [ ] Screens (auto-generated від manifest де можливо):
  ```
  1. Dashboard: T_air (великий), T_evap, setpoint, compressor status, alarm icon
  2. Alarms: active alarms list, alarm code, duration
  3. Parameters: grouped settings (thermostat, defrost, protection)
  4. System: WiFi status, MQTT status, firmware version, uptime
  5. Chart: mini temperature trend (останні 4h, downsampled)
  ```
- [ ] Navigation: 3 кнопки (UP/DOWN/SELECT) на free GPIO inputs або encoder
- [ ] Display sleep: backlight off після 5 хв без input
- [ ] Config:
  ```
  display.enabled     (bool, default: false, persist)
  display.brightness  (int, 0-100, default: 80, persist)
  display.sleep_min   (int, minutes, default: 5, persist)
  display.rotation    (int, 0/90/180/270, persist)
  ```

**RAM:** ~40KB (LVGL draw buffers, 2× partial)
**Flash:** ~100KB (LVGL core + font + screens)
**Складність:** Висока — LVGL інтеграція потребує значної роботи

---

### F-11.2. Modbus RTU Slave (RS-485)

**Hardware:**
```
MAX485 або SP3485 (RS-485 transceiver)
  → UART1: TX GPIO 17, RX GPIO 16, DE/RE GPIO 2
Вартість: $1-2 за transceiver
```

**Firmware:**
- [ ] Modbus RTU slave implementation (FreeRTOS task, 9600-115200 baud)
- [ ] Register map (auto-generated від state_meta.json):
  ```
  Holding Registers (read/write): writable config params
    40001: thermostat.setpoint (×10, int16)
    40002: thermostat.differential (×10)
    40003: defrost.interval
    ... (60 registers від subscribeKeys)

  Input Registers (read-only): state values
    30001: equipment.air_temp (×10, int16)
    30002: equipment.evap_temp (×10)
    30003: equipment.compressor (0/1)
    ... (48 registers від publishKeys)

  Coils (read-only): alarms
    00001: protection.high_temp_alarm
    00002: protection.low_temp_alarm
    ... (10 coils)

  Discrete Inputs:
    10001: equipment.compressor (on/off)
    10002: defrost.active
  ```
- [ ] Configurable Modbus address (1-247, persist NVS)
- [ ] Auto-generated register map documentation
- [ ] Feature: `modbus_rtu` → controlled by board.json (UART pin availability)

**RAM:** ~4KB (UART buffer + register shadow)
**Flash:** ~10KB code
**Складність:** Середня — ESP-IDF має Modbus component, потрібен mapping

---

### F-11.3. Modbus RTU Master (для зовнішніх лічильників)

**Hardware:** Той самий RS-485, але Master mode.

**Firmware:**
- [ ] Modbus master на UART2 (окремий від slave)
- [ ] Configurable device polling:
  ```json
  "modbus_master": {
    "uart": 2,
    "baud": 9600,
    "devices": [
      {
        "address": 1,
        "type": "energy_meter",
        "model": "SDM120",
        "registers": {
          "voltage": {"addr": 0, "type": "float32", "unit": "V"},
          "current": {"addr": 6, "type": "float32", "unit": "A"},
          "power": {"addr": 12, "type": "float32", "unit": "W"},
          "energy": {"addr": 342, "type": "float32", "unit": "kWh"}
        }
      }
    ]
  }
  ```
- [ ] Mapping зовнішніх registers → state keys → MQTT
- [ ] Перевага над SCT-013: точний kWh без калібрування

**RAM:** ~3KB
**Складність:** Середня

---

## Фаза F-12: Multi-Board + Production Hardening

**Ціль:** Підтримка різних плат, підготовка до серійного виробництва.
**Тривалість:** 3-4 тижні

### F-12.1. Custom PCB Board Support

- [ ] Мінімальна плата: ESP32-S3 + 4 relay (GPIO direct) + 2 DS18B20 + 1 NTC
- [ ] `board_custom_mini.json` — pin mapping для custom PCB
- [ ] Тестування: compile + flash + verify з іншим board.json (без зміни коду)

### F-12.2. ESP32-C3 Budget Board

- [ ] ESP32-C3 (RISC-V, 400KB SRAM, 4MB flash, WiFi+BLE5)
- [ ] Обмежений функціонал: 2 relay, 2 sensor, no display, no Modbus
- [ ] Ціль: найдешевший IoT-монітор для малого обладнання ($5 BOM)

### F-12.3. Production Hardening

- [ ] Secure Boot V2 (RSA-PSS, hardware root of trust)
- [ ] Flash Encryption (AES-256-XTS)
- [ ] NVS Encryption (separate key partition)
- [ ] Factory reset button (GPIO long-press 10s → clear NVS, reboot to AP mode)
- [ ] Manufacturing test mode (self-test: ADC, I2C, OneWire, relay → report pass/fail)
- [ ] Serial number в eFuse (unique device identity)

---

## Візуальна дорожня карта

```
2026 Q2 (квітень-травень)          Q3 (червень-липень)           Q4 (серпень+)
──────────────────────────────────────────────────────────────────────────────
 F-9.1: SCT-013 current ⚡        F-10.1: Adaptive Defrost      F-11.1: SPI Display
 ├── ADC RMS driver               ├── Efficiency tracking        ├── LVGL integration
 ├── Power/energy calc            ├── Auto interval adjust       ├── 5 screens
 └── 1 тиждень                   └── 2 тижні                   └── 3-4 тижні

 F-9.3: Door metrics 🚪           F-10.2: Condenser Monitor     F-11.2: Modbus Slave
 └── 2 дні                       ├── ΔT tracking               ├── Register map
                                  ├── Degradation alert          ├── Auto-generated
 F-9.4: Edge Analytics 📊         └── 1 тиждень                └── 2 тижні
 ├── Duty deviation
 ├── COP indicator                F-10.3: Smart Night Mode      F-11.3: Modbus Master
 ├── Defrost efficiency           └── 1 тиждень                └── 2 тижні
 └── 2 тижні
                                  F-10.4: Compressor Health     F-12: Multi-board
                                  └── 1 тиждень                ├── Custom PCB
                                                                ├── ESP32-C3
                                  F-9.2: SHT31 humidity 💧      ├── Secure Boot
                                  (⚪ optional, low priority)    └── 3-4 тижні
                                  └── 1 тиждень (якщо потрібен
                                      для конкретного клієнта)
──────────────────────────────────────────────────────────────────────────────
```

---

## Залежності Firmware → Cloud

| Firmware Phase | Нові State Keys | Cloud Phase | Примітки |
|---------------|-----------------|-------------|----------|
| F-9.1 (SCT-013) | +4 keys (current, power, energy, energy_today) | Cloud Phase 9 (Energy Dashboard) | Cloud може estimated kWh без firmware |
| F-9.2 (SHT31) | +2-4 keys (humidity, temp, alarms) | Cloud telemetry (автоматично) | ⚪ Опціональний. Тільки для HACCP або display cases |
| F-9.3 (Door) | +3 keys (count, duration, last_open) | Cloud analytics | Корисно для fleet benchmarking |
| F-9.4 (Edge Analytics) | +6 keys (duty_dev, cop, defrost_eff + trends) | Cloud Phase 13 (Recommendations) | Cloud використовує як input для rules |
| F-10.1 (Adaptive Defrost) | +3 keys (adaptive_interval, efficiency, adjustments) | Cloud telemetry (автоматично) | Cloud показує економію |
| F-10.2 (Condenser) | +4 keys (delta_t, degradation, baseline, alarm) | Cloud Phase 13 (Recommendations) | "Очистіть конденсатор" |
| F-10.4 (Compressor Health) | +4 keys (health, efficiency, trend, hard_starts) | Cloud Phase 9 (Health Score) | Input для cloud health score |
| F-11.2 (Modbus Slave) | 0 (existing keys exposed via Modbus) | Немає | BMS/SCADA інтеграція |

---

## Оцінка ресурсів

### Після всіх фаз F-9 + F-10:

| Ресурс | Зараз | Після F-9+F-10 | Ліміт | Запас |
|--------|-------|----------------|-------|-------|
| State keys | 122 | ~148 (+26) | 256 (ETL) | 108 |
| MQTT pub keys | 48 | ~74 (+26) | ~128 | 54 |
| MQTT sub keys | 60 | ~70 (+10) | ~128 | 58 |
| DRAM used | ~150KB | ~160KB (+10KB) | 320KB | 160KB |
| Flash (firmware) | ~1.3MB | ~1.35MB (+50KB) | 1.5MB (ota_0) | 150KB |
| NVS entries | ~60 | ~75 (+15) | 256 | 181 |

### Після F-11 (Display + Modbus):

| Ресурс | Після F-11 | Ліміт | Запас |
|--------|-----------|-------|-------|
| DRAM used | ~210KB (+50KB LVGL) | 320KB | 110KB |
| Flash | ~1.5MB (+150KB LVGL+Modbus) | 1.5MB | ⚠️ На межі |

> **Увага:** Display (LVGL) займе значну частину flash. Можливо потрібна окрема firmware build з display support (без WebUI static files) або перехід на 8MB flash.

---

## Milestones

| # | Назва | Фази | Ціль |
|---|-------|------|------|
| M4 | **Smart Controller** | F-9.1 + F-9.3 + F-9.4 (energy, door, edge analytics) | Q2 2026 |
| M5 | **Danfoss Killer** | F-10 (adaptive defrost, condenser, compressor health) | Q3 2026 |
| M6 | **Industrial Grade** | F-11 (display + Modbus) + F-12 (multi-board, secure boot) | Q4 2026 |

---

## Принципи розвитку

1. **Edge-first** — якщо алгоритм може працювати на контролері, він НЕ йде в cloud
2. **Manifest-driven** — нові параметри починаються з manifest → auto-generated code
3. **Zero heap в hot path** — ETL containers, stack buffers, no malloc в ISR/timers
4. **Backward compatible** — нові features за feature flags, старі bindings працюють
5. **Board-agnostic** — бізнес-логіка не залежить від конкретної плати (board.json абстракція)
6. **Тести обов'язкові** — pytest + host C++ unit tests для кожного нового алгоритму

---

## Changelog

- 2026-03-10 — F-9.2 (humidity) знижено до optional/low priority. Додано чесну оцінку: обмеження надійності в холодильних камерах, реальна ціна probe ($15-30), post-defrost filter, порівняння з industry standard (T_evap analysis). Humidity корисний тільки для HACCP compliance logging та display case anti-condensation, НЕ для defrost або thermostat алгоритмів.
- 2026-03-10 — Створено. Phases F-9 — F-12 на основі конкурентного аналізу та аудиту hardware.
