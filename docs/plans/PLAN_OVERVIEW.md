# ModESP v4 → ModESP PRO: 2-Zone MPXPRO Alternative

> **Статус:** Це МЕТА-ПЛАН (roadmap). Кожен блок потребує окремого детального плану
> на основі глибокого дослідження перед реалізацією.

## Context

Партнер використовує Carel MPXPRO MX30M25HR0 (~€590) з E2V клапанами та SPKT датчиками тиску. Потрібна **мультизонна платформа: 2 зони на 1 ESP32** (KC868-A6). Спільний компресор, незалежні випарники, EXV клапани та датчики на кожну зону.

---

## Об'єктивна оцінка готовності кожного блоку

| Блок | Дослідження | Що НЕ досліджено | Готовність до реалізації |
|---|---|---|---|
| **A (namespace)** | Generator, hardcoded keys count, BaseModule API | StateKey type, SharedState internals, ModuleManager multi-instance, RAM бюджет | 40% — потрібно вивчити SharedState/ModuleManager |
| **B (pressure)** | SPKT specs, ESP32 ADC accuracy, ADS1115 | 4-20mA sensors (Danfoss AKS), NTC driver internals, ADC HAL layer | 50% — потрібно абстракція для різних типів сенсорів |
| **C (refrigerant)** | Загальна ідея | Джерела даних, кількість точок для ±0.5°C, R32/R454B | 30% — потрібні реальні дані з CoolProp/NIST |
| **D (valve drivers)** | E2V, AKV, TMC2209, EVD Mini | **Інші виробники!** Emerson, Sporlan, Sanhua, Fujikoki (unipolar!), generic 0-10V/4-20mA | 35% — потрібна абстракція, не 3 hardcoded drivers |
| **E (EEV PI)** | Velocity-form PI, state machine | MPXPRO algorithm, PID tuning for refrigeration, anti-hunting, real-world tuning | 25% — найскладніший блок, мало досліджений |
| **F (Modbus)** | esp-modbus, MAX13487, register mapping | Стандартні register maps (Carel/Danfoss), slave address config | 60% — найготовіший блок |
| **G (protection)** | Згаданий | HP/LP ranges, alarm priorities, interaction з existing 10 monitors | 20% — потрібен окремий план |
| **H (watchdog)** | Згаданий | Implementation, safe state definition, power loss detection | 15% — критично важливий, мало досліджений |
| **I (zone coord)** | Згаданий | Defrost coordination algorithm, compressor+EEV timing | 15% — залежить від відповідей партнера |
| **J (WebUI)** | Wish list | Все | 5% — просто список бажань |

### Ключові пропуски в поточному плані

**1. Valve driver abstraction (Блок D) — надто вузький**
План покриває тільки E2V (Carel stepper) та AKV (Danfoss PWM). Але ринок набагато ширший:
- **Bipolar steppers:** Carel E2V, Danfoss ETS, Emerson EX, Sporlan SEI
- **Unipolar steppers:** Fujikoki (3-wire, інший драйвер!)
- **PWM solenoids:** Danfoss AKV, Sporlan EB
- **0-10V analog:** через зовнішній EVD/EKD driver
- **4-20mA analog:** промисловий стандарт
- **Modbus-controlled:** EVD Evolution, деякі VFD
Потрібна generic абстракція `IValveDriver` з `set_position(float 0..1)`, а не 3 конкретні класи.

**2. Pressure sensor types (Блок B) — тільки ratiometric**
- **Ratiometric 0.5-4.5V:** Carel SPKT (в плані)
- **4-20mA current loop:** Danfoss AKS, Emerson PT5 (НЕ в плані!)
- **0-5V або 0-10V:** generic промислові
4-20mA потребує резистор 250Ω для конвертації в 1-5V, або окремий АЦП.

**3. RAM бюджет для 2-zone (Блок A) — не порахований**
- Поточний SharedState: ~137 keys × ? bytes = ?
- 2-zone: ~250 keys → потрібно перевірити чи влізе в RAM ESP32
- Resolved keys per module: 38+30+40+30 = ~138 × 32 = ~4.4KB додатково per zone
- ModuleManager pools: 2× більше module instances

**4. EEV PI tuning (Блок E) — теоретичний, не практичний**
- Velocity-form PI описаний, але Kp/Ki defaults не обґрунтовані
- Немає порівняння з MPXPRO/Dixell алгоритмами
- Немає аналізу стабільності (step response, oscillation risk)
- MOP (Maximum Operating Pressure) логіка не деталізована

**5. Safety (Блок H) — найкритичніший, найменш досліджений**
- Power loss → valve state невизначений
- ESP32 brownout detection → чи встигне закрити клапан?
- Watchdog timeout values не обґрунтовані
- Sequence of operations при аварійній зупинці не описаний

---

## Рекомендований підхід: окремі плани

Кожен блок повинен мати **окремий детальний план** з:
1. Глибоке дослідження предметної області
2. Аналіз існуючого коду (конкретні файли, функції, типи)
3. Порівняння з конкурентами (MPXPRO, Dixell, Emerson)
4. Деталі імплементації з прив'язкою до конкретних рядків коду
5. Тести і критерії прийняття
6. RAM/Flash бюджет

### Пріоритет окремих планів

| # | План | Чому першим | Залежності |
|---|---|---|---|
| 1 | **Блок F: Modbus** | Найготовіший (60%), незалежний, швидкий результат для партнера | Немає |
| 2 | **Блок C: Refrigerant tables** | Незалежний, чисто обчислювальний, легко тестувати | Немає |
| 3 | **Блок B: Pressure sensors** | Незалежний, потрібен для E та G | Немає |
| 4 | **Блок A: Namespace** | Блокує D, E, I. Але потребує найглибшого дослідження коду | Немає |
| 5 | **Блок D: Valve drivers** | Після A. Потрібне дослідження ринку клапанів | A1-A2 |
| 6 | **Блок E: EEV PI** | Найскладніший. Потребує B, C, D | B, C, D |
| 7 | **Блок H: Safety** | Критичний для production. Після D, E | D, E |
| 8 | **Блок G: Protection** | Розширення існуючого модуля | B |
| 9 | **Блок I: Zone coord** | Після уточнення з партнером | A6 |
| 10 | **Блок J: WebUI** | Останній | Все інше |

### KC868-A6 ↔ 2-zone mapping (ідеальний збіг)

| Ресурс | GPIO | Zone 1 | Zone 2 | Shared |
|---|---|---|---|---|
| Relay 1 | PCF8574:0 | | | Compressor |
| Relay 2 | PCF8574:1 | | | Cond fan |
| Relay 3 | PCF8574:2 | Evap fan Z1 | | |
| Relay 4 | PCF8574:3 | | Evap fan Z2 | |
| Relay 5 | PCF8574:4 | Defrost Z1 | | |
| Relay 6 | PCF8574:5 | | Defrost Z2 | |
| DAC 1 | GPIO 25 | EXV Z1 (0-10V→EVD Mini) | | |
| DAC 2 | GPIO 26 | | EXV Z2 (0-10V→EVD Mini) | |
| OneWire 1 | GPIO 32 | air_temp + evap_temp Z1 | | |
| OneWire 2 | GPIO 33 | | air_temp + evap_temp Z2 | |
| ADC 1 | GPIO 36 | Suction P Z1 | | |
| ADC 2 | GPIO 39 | | Suction P Z2 | |
| ADC 3 | GPIO 34 | | | Discharge P |
| ADC 4 | GPIO 35 | | | Spare |
| DIN 1 | PCF8574:0 | Door Z1 | | |
| DIN 2 | PCF8574:1 | | Door Z2 | |
| RS485 | TX=GPIO27, RX=GPIO14 | | | Modbus (MAX13487EESA auto-dir) |

### ADC accuracy: dual-path strategy

ESP32 12-bit ADC: ±1 bar після калібрації (±30-100mV). Для 0-60 bar SPKT це:
- **Достатньо** для pressure alarms (HP/LP threshold)
- **НЕ достатньо** для superheat PID (потрібно ±0.5 bar)
- **Рішення:** ADS1115 (16-bit I2C, ~$2) для PID каналів. I2C addr 0x48 не конфліктує з PCF8574.
- АБО: oversampling 256x + polynomial correction → ±10-15mV → ±0.3 bar (потрібна перевірка)

### EXV варіанти (E2V: bipolar stepper, 450mA, 36Ω, 480 steps, 24V min)

| Варіант | Вартість/zone | Для чого |
|---|---|---|
| **EVD Mini + DAC 0-10V** | ~€140 | Production (certified, power-loss safe) |
| **TMC2209 direct** | ~€20 | Бюджет/розробка (StallGuard homing, UART) |
| **EVD Evolution + Modbus** | ~€200 | Повна інтеграція |

Danfoss AKV 10P-2: PWM соленоїд, 24V DC (BE024DS, 0.75A), MOSFET + flyback, 6s cycle.

---

## Блоки D, E, G-J (короткі описи)

### Блок D: Драйвери клапанів (потребує A1-A2 для namespace)

**D1: EevAnalogDriver (DAC → EVD Mini)**
- Нові файли: `drivers/eev_analog/`
- IActuatorDriver з `set_value(float 0..1)` → ESP32 DAC (8-bit, 0-10V)
- Для EVD Mini в positioner mode (parameter S4=5)
- **Найпростіший варіант, ~100 рядків**

**D2: EevStepperDriver (TMC2209 direct)**
- Нові файли: `drivers/eev_stepper/`
- STEP/DIR/ENABLE GPIO, 24V supply
- Non-blocking step generation, 50Hz drive frequency
- Homing: overdrive 550 steps closed, StallGuard detection
- NVS position save/restore
- **~400 рядків, потребує TMC2209 UART lib**

**D3: AkvPulseDriver (PWM solenoid)**
- Нові файли: `drivers/akv_pulse/`
- LEDC PWM, 6s cycle, duty 10-100%
- MOSFET + flyback diode на 24V DC coil
- **~120 рядків**

**D4: HAL extensions**
- `hal_types.h`: + StepperOutputConfig, PwmChannelConfig, DacChannelConfig
- `hal.cpp`: + init_stepper_outputs(), init_dac()
- `generate_ui.py`: + VALID_HARDWARE_TYPES

### Блок E: EevModule — superheat PI controller (потребує B, C, D)

**E1: 7-state machine**
- Нові файли: `modules/eev/`
- States: INIT → IDLE → STARTUP → RUNNING → LOW_SH_PROTECT → DEFROST → SENSOR_FAULT
- **Velocity-form PI** (індустріальний стандарт):
  ```
  delta_steps = Kp * (error - prev_error) + Ki * Ts * error
  new_position = current_position + delta_steps
  ```
- Природний anti-windup (velocity form)
- Виводить кроки/позицію, не абсолютне значення

**E2: Timing (best practices з дослідження)**
- Sensor read: 500ms
- Superheat calculation: 1s
- PI loop: 2-5s (configurable)
- Max steps per cycle: обмежити для anti-hunting

**E3: Startup procedure**
- Compressor OFF → valve closed, PI reset
- Compressor START → feed-forward opening (50% default або last good position з NVS)
- Wait 2-5 min (configurable) → enable PI
- Low-SH protection активна навіть під час wait

**E4: Safety interlocks (обов'язкові)**
- SH < 2K → aggressive close (2-3x normal rate)
- SH < 0K (subcooled) → full close immediately
- MOP: close valve when suction P > limit
- Sensor fail → safe position (50% default)
- Compressor OFF → valve closed
- Deadband ±1K around setpoint (anti-hunting)

**E5: Parameters (through SharedState, WebUI editable)**
- superheat_target (8K default), Kp, Ki, startup_wait_s
- min_position, max_position, safe_position
- low_sh_limit, mop_pressure, deadband

### Блок G: Protection expansion (після B)

**G1: Pressure-based alarms**
- `modules/protection/manifest.json`: + HP, LP, low-SH monitors
- `protection_module.cpp`: + 3 monitors за існуючим паттерном
- Guarded by `equipment.has_suction_p` — backward compatible

### Блок H: Watchdog + emergency (після D, E)

**H1: Valve safety logic**
- Valve closes BEFORE or WITH compressor stop
- ModuleManager watchdog: module stuck > N sec → safe state
- Safe state: all valves close, compressor stop

**H2: Power loss handling**
- EVD Mini варіант: Ultracap (~€40) вирішує проблему
- TMC2209 варіант: NVS save position + homing on restart
- Рекомендація партнеру: Ultracap або зовнішній supercap circuit

### Блок I: Zone coordination (після A6)

**I1: ZoneCoordinator в EquipmentModule**
- Арбітраж compressor: OR(zone1.req, zone2.req)
- Coordinated defrost: одна зона за раз (або обидві з затримкою)
- При defrost Z1: EevModule Z2 підвищує superheat target
- Condenser fan: підвищений поріг при hot-gas defrost

### Блок J: WebUI + маніфест (останній)

**J1: Нові UI pages/widgets**
- Superheat graph (realtime)
- Valve position indicator
- PI state display
- Pressure readings per zone
- Zone status overview

---

## Порядок реалізації (з паралелізацією)

```
     Тиждень:  1    2    3    4    5    6    7    8    9   10

Блок A (namespace):
  A1-A2       ██
  A3-A5           ████
  A6                    ████
  A7                    ██

Блок B (pressure, паралельно з A):
  B1-B2       ████
  B3              ██

Блок C (refrigerant, паралельно):
  C1          ██

Блок F (Modbus, паралельно):
  F1-F4           ████

Блок D (valve drivers, після A1-A2):
  D1              ██
  D2-D3               ████
  D4              ██

Блок E (EEV module, після B+C+D):
  E1-E5                     ████████

Блок G (protection, після B):
  G1                  ██

Блок H (watchdog, після D+E):
  H1-H2                             ████

Блок I (zone coord, після A6):
  I1                        ████

Блок J (WebUI, останній):
  J1                                     ████
```

**Critical path:** A1→A2→A6→I1 (namespace → multi-zone Equipment)
**Quick wins (паралельно):** B1-B2 (pressure sensors), C1 (refrigerant tables), F1-F4 (Modbus)

---

## Критичні файли

| Файл | Блоки |
|---|---|
| `components/modesp_core/include/modesp/base_module.h` | A1, A2 |
| `components/modesp_core/src/base_module.cpp` | A1, A2 |
| `modules/thermostat/src/thermostat_module.cpp` | A3 (38 replacements) |
| `modules/defrost/src/defrost_module.cpp` | A4 (30 replacements) |
| `modules/protection/src/protection_module.cpp` | A5, G1 |
| `modules/equipment/src/equipment_module.cpp` | A6, B2, I1 |
| `modules/equipment/manifest.json` | A6, B2 |
| `components/modesp_hal/include/modesp/hal/hal_types.h` | D4 |
| `components/modesp_hal/src/driver_manager.cpp` | B2, D1-D3 |
| `tools/generate_ui.py` | A7, D4 |
| `boards/kc868a6/board.json` | F4 |
| `boards/kc868a6/bindings.json` | B2, D1-D3 |

## Reusable patterns

| Шаблон | Де використовувати |
|---|---|
| NTC driver (`drivers/ntc/`) | PressureAdcDriver (B1), ADS1115 (B3) |
| Relay driver (`drivers/relay/`) | EevStepperDriver (D2), AkvPulseDriver (D3) |
| Protection monitors pattern | Pressure alarms (G1) |
| Defrost FSM | EevModule state machine (E1) |
| esp-modbus official component | ModbusSlaveModule (F2) |

## Verification

| Блок | Тест |
|---|---|
| A | Існуючі 108 host tests + 254 pytest повинні пройти без змін |
| B | Host: pressure math. Hardware: SPKT на ADC |
| C | Host: interpolation accuracy ≤0.5°C для всіх фреонів |
| D | Hardware: stepper movement, DAC output voltage |
| E | Host: PI step response, state machine transitions. Hardware: стенд |
| F | Hardware: Modbus master reader (any Modbus tool) |
| G | Host: alarm triggering. Pytest: state keys |
| H | Hardware: power cycle test, watchdog trigger |
