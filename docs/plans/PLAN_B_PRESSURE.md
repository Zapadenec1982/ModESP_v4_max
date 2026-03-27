# Блок B: Датчики тиску — ДЕТАЛЬНИЙ ПЛАН

> **Статус: DESIGNING (80%)**
> Досліджено: NTC driver pattern, ISensorDriver interface, ADC oneshot API, DriverManager pools.
> Pattern: клон NTC driver з іншою формулою конверсії.

---

## Контекст

Датчики тиску потрібні для: superheat PID (EevModule), HP/LP alarms (Protection), monitoring (WebUI/Modbus). SPKT00G1S0 (Carel) — ratiometric 0.5-4.5V на 0-60 bar через ADC KC868-A6.

## ISensorDriver interface (з codebase, driver_interfaces.h)

```cpp
class ISensorDriver {
    virtual bool init() = 0;
    virtual void update(uint32_t dt_ms) = 0;
    virtual bool read(float& value) = 0;  // cached value
    virtual bool is_healthy() const = 0;   // false after 5 consecutive errors
    virtual const char* role() const = 0;
    virtual const char* type() const = 0;
};
```

## Покрокова реалізація

**B1: PressureAdcDriver (клон NTC з іншою математикою)**

Нові файли:
```
drivers/pressure_adc/
├── manifest.json
├── include/pressure_adc_driver.h
├── src/pressure_adc_driver.cpp    (~180 рядків)
└── CMakeLists.txt
```

**manifest.json:**
```json
{
  "manifest_version": 1,
  "driver": "pressure_adc",
  "hardware_type": "adc_channel",
  "provides": {"type": "float", "unit": "bar", "range": [-1, 60]},
  "settings": [
    {"key": "p_range", "type": "float", "default": 60.0, "description": "Sensor full scale (bar)"},
    {"key": "v_min", "type": "float", "default": 0.5, "description": "Min output voltage"},
    {"key": "v_max", "type": "float", "default": 4.5, "description": "Max output voltage"},
    {"key": "offset", "type": "float", "default": 0.0, "description": "Calibration offset (bar)"},
    {"key": "filter_alpha", "type": "float", "default": 0.2, "description": "IIR filter coefficient"}
  ]
}
```

**Конверсія (замість NTC B-parameter):**
```cpp
// KC868-A6: LM224 scales 0-5V → 0-3.3V for ESP32 ADC
// ADC raw (0-4095) → voltage (0-5V через reverse scaling)
float voltage = (raw / 4095.0f) * 5.0f;  // reverse LM224 scaling

// SPKT ratiometric: 0.5V = 0 bar, 4.5V = P_range bar
float pressure = (voltage - v_min_) / (v_max_ - v_min_) * p_range_ + offset_;
```

**IIR фільтр (відсутній в NTC — додаємо):**
```cpp
// In update():
float raw_pressure = voltage_to_pressure(voltage);
filtered_ = filter_alpha_ * raw_pressure + (1.0f - filter_alpha_) * filtered_;
```

**Діагностика (за NTC pattern):**
- `voltage < 0.3V` → обрив (sensor disconnected)
- `voltage > 4.7V` → КЗ (short circuit)
- `consecutive_errors_ >= 5` → `is_healthy() = false`

**ADC API:** `adc_oneshot_read()` — shared `s_adc1_handle` з NTC driver (static в ntc_driver.cpp). **Проблема:** handle static в NTC файлі. Потрібно перенести в HAL або зробити shared init.

**B2: DriverManager integration**

Файл: `components/modesp_hal/src/driver_manager.cpp`
- Додати `static PressureAdcDriver pressure_pool[MAX_SENSORS];` + counter
- Додати `create_pressure_adc_sensor()` за NTC pattern
- Додати dispatcher: `else if (binding.driver_type == "pressure_adc")`
- Додати reset: `pressure_adc_count = 0;`

**B3: Bindings + Equipment integration**

`boards/kc868a6/bindings.json` додати:
```json
{"hardware": "adc_1", "driver": "pressure_adc", "role": "suction_pressure", "module": "equipment"},
{"hardware": "adc_2", "driver": "pressure_adc", "role": "discharge_pressure", "module": "equipment"}
```

`modules/equipment/manifest.json` додати roles + state keys:
```json
"state": {
    "equipment.suction_bar": {"type": "float", "access": "read"},
    "equipment.discharge_bar": {"type": "float", "access": "read"},
    "equipment.has_suction_p": {"type": "bool", "access": "read"}
}
```

`modules/equipment/src/equipment_module.cpp`:
- `bind_drivers()`: + `find_sensor("suction_pressure")`
- `update_sensors()`: + read pressure → `state_set("equipment.suction_bar", val)`

**B4: ADC handle sharing (архітектурний fix)**

Проблема: `s_adc1_handle` — static в `ntc_driver.cpp`. PressureAdcDriver теж потребує його.
Рішення: перенести ADC1 init в HAL:
```cpp
// hal.cpp:
adc_oneshot_unit_handle_t HAL::get_adc1_handle() {
    if (!adc1_handle_) {
        adc_oneshot_unit_init_cfg_t cfg = {.unit_id = ADC_UNIT_1};
        adc_oneshot_new_unit(&cfg, &adc1_handle_);
    }
    return adc1_handle_;
}
```
NTC і PressureAdc обидва використовують `hal.get_adc1_handle()`.

**B5: ADS1115 driver (опціонально, для superheat PID)**

Якщо ESP32 ADC недостатньо точний (±1 bar) для superheat:
- I2C 16-bit ADC, addr 0x48-0x4B
- Implements ISensorDriver (same interface)
- `drivers/ads1115/` — ~250 рядків
- bindings.json: `{"hardware": "i2c_0", "driver": "ads1115", "role": "suction_pressure_hires"}`
- **Відкладити до стендових випробувань** — спочатку перевірити чи ESP32 ADC + IIR фільтр + oversampling дає ±0.5 bar

## Verification

| Крок | Тест |
|---|---|
| B1 | Host: voltage→pressure conversion, IIR filter, diagnostics |
| B2 | Build: driver compiles, pool allocation works |
| B3 | Hardware: SPKT на ADC, порівняти з manometer |
| B4 | Build: NTC + Pressure обидва працюють одночасно |
| B5 | Hardware: ADS1115 vs ESP32 ADC accuracy comparison |
