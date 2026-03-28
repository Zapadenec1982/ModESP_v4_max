# ModESP PRO — Hardware Guide for Partner (KC868-A6)

## Огляд платформи

KC868-A6 (Kincony) — базова плата для ModESP PRO контролера холодильного обладнання.
Підтримує 2 зони охолодження з незалежним керуванням EEV, defrost, thermostat.

## Ресурси KC868-A6

| Ресурс | Кількість | GPIO/Bus | Призначення |
|---|---|---|---|
| Relay outputs | 6 | PCF8574 @ 0x24 | Compressor, fans, defrost |
| Digital inputs | 6 | PCF8574 @ 0x22 (EL357 opto) | Door, night, **або EEV outputs** |
| ADC 0-5V | 4 | GPIO 36/39/34/35 | Pressure sensors (SPKT) |
| DAC 0-10V | 2 | GPIO 25/26 | EEV analog (EVD Mini) |
| OneWire | 2 | GPIO 32/33 | DS18B20 temperature sensors |
| RS485 | 1 | GPIO 27/14 (MAX13487) | Modbus RTU |
| I2C | 1 | GPIO 4/15 | PCF8574, OLED, ADS1115 |

## Розподіл relay по зонах (2-zone)

| Relay | GPIO | Призначення |
|---|---|---|
| 1 | PCF8574:0 | Compressor (shared) |
| 2 | PCF8574:1 | Condenser fan (shared) |
| 3 | PCF8574:2 | Evaporator fan Zone 1 |
| 4 | PCF8574:3 | Evaporator fan Zone 2 |
| 5 | PCF8574:4 | Defrost relay Zone 1 |
| 6 | PCF8574:5 | Defrost relay Zone 2 |

## Керування EEV клапанами

### Варіант A: DAC 0-10V → EVD Mini (без модифікацій)

Найпростіший варіант — використовує вбудовані DAC виходи KC868-A6.

```
KC868-A6 DAC1 (GPIO25) ──[0-10V]──→ EVD Mini Z1 (param S4=5) ──→ E2V Z1
KC868-A6 DAC2 (GPIO26) ──[0-10V]──→ EVD Mini Z2 (param S4=5) ──→ E2V Z2
```

| Параметр | Значення |
|---|---|
| Роздільність | 8-bit (256 кроків з 480) |
| Додаткові компоненти | EVD Mini EVDM001N00 (~€100/zone) |
| Ultracap | EVD0000UC0 (~€40/zone) — рекомендовано |
| Модифікація KC868-A6 | Не потрібна |
| **Вартість на zone** | **~€140** |

### Варіант B: PCF8574 → H-bridge → E2V direct (MVP модифікація)

Використовує digital inputs як stepper outputs. Потребує модифікації плати.

**Модифікація:** Видалити 4× EL357 оптопари з позицій DIN1-DIN4.
PCF8574 pin виходить напряму на клемник через існуючий pull-up 4.7KΩ.

```
PCF8574 P0 (DIN1) ──→ STEP Z1 ──→ H-bridge ──→ E2V Z1 Coil A/B
PCF8574 P1 (DIN2) ──→ DIR  Z1 ──→ H-bridge
PCF8574 P2 (DIN3) ──→ STEP Z2 ──→ H-bridge ──→ E2V Z2 Coil A/B
PCF8574 P3 (DIN4) ──→ DIR  Z2 ──→ H-bridge
PCF8574 P4 (DIN5) ──→ Door contact (input, залишається)
PCF8574 P5 (DIN6) ──→ Spare
```

| Параметр | Значення |
|---|---|
| Роздільність | Full-step (480 кроків) |
| Drive frequency | 50 Hz (I2C @ 100kHz) |
| Додаткові компоненти | H-bridge L298N (~€2/zone) або 4× MOSFET |
| Живлення stepper | 24V DC (окреме від ESP32) |
| Модифікація KC868-A6 | Видалити 4 оптопари |
| Ізоляція | ❌ Відсутня (прийнятно для MVP) |
| **Вартість на zone** | **~€2** |

### Варіант B+ (серійний): CPC1019N замість EL357

Для серійного пристрою — замінити EL357 на CPC1019N PhotoMOS **розвернутий 180°**.
Зберігає ізоляцію 1.5kV. Потребує перерізання доріжки 12VIN на 4 позиціях.

| Параметр | Значення |
|---|---|
| Trigger current | 0.2mA (працює з існуючим pull-up 4.7KΩ) |
| Ізоляція | ✅ 1500V |
| Додатково | Перерізати trace 12VIN→Pad1 (4 позиції) |
| **Вартість на zone** | **~€3** |

### Варіант C: Danfoss AKV (PWM соленоїд)

Для AKV не потрібен stepper — тільки PWM через MOSFET.
Можна використати PCF8574 output для повільного PWM (6-секундний цикл).

```
PCF8574 P0 (DIN1) ──→ MOSFET gate ──→ AKV Z1 coil (24V DC, 0.75A)
PCF8574 P1 (DIN2) ──→ MOSFET gate ──→ AKV Z2 coil (24V DC, 0.75A)
PCF8574 P2-P5 ──→ Вільні для інших входів/виходів
```

| Параметр | Значення |
|---|---|
| Котушка | BE024DS (24V DC, 18W, 0.75A) |
| Цикл PWM | 6 секунд, duty 10-100% |
| Захист | Flyback діод (обов'язково!) |
| Fail-safe | ✅ Закривається при знятті живлення |
| Ultracap | ❌ Не потрібен |
| **Вартість на zone** | **~€1** (MOSFET + діод) |

## Датчики тиску

### SPKT00G1S0 (Carel, ratiometric 0.5-4.5V)

Підключається напряму до ADC входів KC868-A6.

```
SPKT sensor ──[0.5-4.5V]──→ KC868-A6 ADC1 (GPIO36) ──→ Suction P Zone 1
SPKT sensor ──[0.5-4.5V]──→ KC868-A6 ADC2 (GPIO39) ──→ Suction P Zone 2
SPKT sensor ──[0.5-4.5V]──→ KC868-A6 ADC3 (GPIO34) ──→ Discharge P (shared)
```

| Параметр | Значення |
|---|---|
| Діапазон | 0-60 bar |
| Точність (ESP32 ADC) | ±1 bar (достатньо для HP/LP alarms) |
| Точність (ADS1115) | ±0.01 bar (для superheat PID) |
| Кабель | SPKC005310 (5м, 3-wire Packard) |

**Для superheat PID:** Рекомендовано ADS1115 (I2C 16-bit ADC, ~€2) на I2C bus замість ESP32 internal ADC.

## Аварійне живлення (UPS для EEV)

**Проблема:** E2V stepper НЕ закривається при знятті живлення → ризик гідроудару.

### Рекомендоване рішення: LiPol UPS

```
24V PSU ──→ 5S BMS модуль ──→ 5S LiPol 500mAh (18.5V nominal)
    │                              │
    └── Діодна розв'язка ──────────┘
                ↓
          H-bridge ──→ E2V motor
          ESP32 (graceful shutdown)
```

| Компонент | Ціна |
|---|---|
| 5S LiPol pack 500mAh | ~€5 |
| 5S BMS модуль (захист, балансування) | ~€2 |
| 2× діоди Шотткі (розв'язка PSU/battery) | ~€0.20 |
| **Разом** | **~€7/контролер** |

**Переваги перед Ultracap (€30-50):**
- Дешевше в 4-7 разів
- Хвилини автономної роботи (не секунди)
- Graceful shutdown: NVS save, MQTT alarm, DataLogger event
- Живить і ESP32 і H-bridge

**Обмеження:**
- LiPol не любить температуру >60°C (контролер в щитку, не біля компресора)
- Строк служби 3-5 років (vs 10 для Ultracap)
- Потрібен BMS для захисту від перезаряду/переразряду

### Альтернатива: AKV замість E2V

Danfoss AKV — fail-safe by design. Закривається при знятті живлення.
**Не потребує UPS, Ultracap, або будь-якого резервного живлення.**
Менша точність регулювання, але значно простіша інсталяція.

## BOM порівняння (на 1 контролер, 2 зони)

| Компонент | Варіант A (EVD Mini) | Варіант B (Direct) | Варіант C (AKV) |
|---|---|---|---|
| KC868-A6 | €45 | €45 | €45 |
| E2V клапани × 2 | €160 | €160 | — |
| AKV клапани × 2 | — | — | €120 |
| EVD Mini × 2 | €200 | — | — |
| Ultracap / UPS | €80 (Ultracap) | €7 (LiPol) | €0 (не потрібен) |
| H-bridge × 2 | — | €4 | — |
| MOSFET + діод × 2 | — | — | €2 |
| SPKT sensors × 3 | €90 | €90 | €90 |
| NTC/DS18B20 × 4 | €12 | €12 | €12 |
| **Разом** | **~€587** | **~€318** | **~€269** |
| vs MPXPRO (€590×1) | **0% економії** | **46% економії** | **54% економії** |

> **Примітка:** MPXPRO MX30M25HR0 = €590 за ОДИН контролер на ОДНУ зону.
> ModESP PRO = €269-587 за ОДИН контролер на ДВІ зони.

## Серійний пристрій (roadmap)

Для серійного виробництва рекомендується розробка власної PCB:
- ESP32-S3 (більше RAM для multi-zone)
- Ізольовані I/O з правильною розводкою (CPC1019N/ISO7721)
- H-bridge для E2V на платі (DRV8874 або дискретні MOSFET)
- RS485 з виділеним UART
- LiPol UPS circuit інтегрований
- Роз'єми для датчиків (Packard 3-pin для SPKT, M12 для E2V)
