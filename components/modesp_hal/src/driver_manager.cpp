/**
 * @file driver_manager.cpp
 * @brief DriverManager implementation — driver creation from bindings
 *
 * Includes concrete driver headers and manages static driver pools.
 * Each binding maps a hardware resource to a driver type and role.
 *
 * Driver types:
 *   "ds18b20"       → DS18B20Driver       (sensor, OneWire)
 *   "digital_input" → DigitalInputDriver   (sensor, GPIO)
 *   "ntc"           → NtcDriver            (sensor, ADC)
 *   "relay"         → RelayDriver          (actuator, GPIO)
 *   "pcf8574_relay" → PCF8574RelayDriver   (actuator, I2C expander)
 *   "pcf8574_input" → PCF8574InputDriver   (sensor, I2C expander)
 */

#include "modesp/hal/driver_manager.h"
#include "ds18b20_driver.h"
#include "relay_driver.h"
#include "digital_input_driver.h"
#include "ntc_driver.h"
#include "pressure_adc_driver.h"
#include "eev_analog_driver.h"
#include "eev_stepper_driver.h"
#include "eev_pcf8574_stepper_driver.h"
#include "akv_pulse_driver.h"
#include "pcf8574_relay_driver.h"
#include "pcf8574_input_driver.h"
#include "esp_log.h"

static const char TAG[] = "DriverMgr";

namespace modesp {

// ═══════════════════════════════════════════════════════════════
// Static driver pools — zero heap allocation
// ═══════════════════════════════════════════════════════════════

static DS18B20Driver ds18b20_pool[MAX_SENSORS];
static size_t ds18b20_count = 0;

static DigitalInputDriver di_pool[MAX_SENSORS];
static size_t di_count = 0;

static NtcDriver ntc_pool[MAX_SENSORS];
static size_t ntc_count = 0;

static PressureAdcDriver pressure_pool[MAX_SENSORS];
static size_t pressure_count = 0;

static RelayDriver relay_pool[MAX_ACTUATORS];
static size_t relay_count = 0;

static PCF8574RelayDriver pcf_relay_pool[MAX_ACTUATORS];
static size_t pcf_relay_count = 0;

static PCF8574InputDriver pcf_input_pool[MAX_SENSORS];
static size_t pcf_input_count = 0;

// Valve driver pools (max 2 valves per zone × 2 zones = 4)
static constexpr size_t MAX_VALVES = 4;
static EevAnalogDriver  eev_analog_pool[MAX_VALVES];
static size_t eev_analog_count = 0;

static EevStepperDriver eev_stepper_pool[MAX_VALVES];
static size_t eev_stepper_count = 0;

static modesp::EevPcf8574StepperDriver eev_pcf_pool[MAX_VALVES];
static size_t eev_pcf_count = 0;

static AkvPulseDriver akv_pool[MAX_VALVES];
static size_t akv_count = 0;

// ═══════════════════════════════════════════════════════════════
// Init — create all drivers from bindings
// ═══════════════════════════════════════════════════════════════

bool DriverManager::init(const BindingTable& bindings, HAL& hal) {
    ESP_LOGI(TAG, "Creating drivers for %d bindings...",
             (int)bindings.bindings.size());

    // Reset pools
    ds18b20_count = 0;
    di_count = 0;
    ntc_count = 0;
    relay_count = 0;
    pcf_relay_count = 0;
    pcf_input_count = 0;
    sensors_.clear();
    actuators_.clear();
    sensor_count_ = 0;
    actuator_count_ = 0;

    // Лямбда для реєстрації сенсора
    auto add_sensor = [this](ISensorDriver* drv, const Binding& b) -> bool {
        if (!drv) return false;
        SensorEntry entry;
        entry.driver = drv;
        entry.role = b.role;
        entry.module = b.module_name;
        sensors_.push_back(entry);
        sensor_count_++;
        ESP_LOGI(TAG, "  Sensor '%s' [%s] -> module '%s'",
                 b.role.c_str(), b.driver_type.c_str(), b.module_name.c_str());
        return true;
    };

    // Лямбда для реєстрації актуатора
    auto add_actuator = [this](IActuatorDriver* drv, const Binding& b) -> bool {
        if (!drv) return false;
        ActuatorEntry entry;
        entry.driver = drv;
        entry.role = b.role;
        entry.module = b.module_name;
        actuators_.push_back(entry);
        actuator_count_++;
        ESP_LOGI(TAG, "  Actuator '%s' [%s] -> module '%s'",
                 b.role.c_str(), b.driver_type.c_str(), b.module_name.c_str());
        return true;
    };

    // Phase 1: Create drivers from bindings (пропускає невдалі)
    for (const auto& binding : bindings.bindings) {
        bool ok = false;
        if (binding.driver_type == "ds18b20") {
            ok = add_sensor(create_sensor(binding, hal), binding);
        } else if (binding.driver_type == "digital_input") {
            ok = add_sensor(create_di_sensor(binding, hal), binding);
        } else if (binding.driver_type == "ntc") {
            ok = add_sensor(create_ntc_sensor(binding, hal), binding);
        } else if (binding.driver_type == "pressure_adc") {
            ok = add_sensor(create_pressure_sensor(binding, hal), binding);
        } else if (binding.driver_type == "relay") {
            ok = add_actuator(create_actuator(binding, hal), binding);
        } else if (binding.driver_type == "pcf8574_relay") {
            ok = add_actuator(create_pcf_actuator(binding, hal), binding);
        } else if (binding.driver_type == "pcf8574_input") {
            ok = add_sensor(create_pcf_sensor(binding, hal), binding);
        } else if (binding.driver_type == "eev_pcf8574_stepper") {
            ok = add_actuator(create_eev_pcf_stepper(binding, hal), binding);
        } else if (binding.driver_type == "eev_analog") {
            ok = add_actuator(create_eev_analog(binding, hal), binding);
        } else if (binding.driver_type == "eev_stepper") {
            ok = add_actuator(create_eev_stepper(binding, hal), binding);
        } else if (binding.driver_type == "akv_pulse") {
            ok = add_actuator(create_akv_pulse(binding, hal), binding);
        } else {
            ESP_LOGW(TAG, "  Unknown driver type '%s' for binding '%s'",
                     binding.driver_type.c_str(),
                     binding.hardware_id.c_str());
            continue;
        }
        if (!ok) {
            ESP_LOGW(TAG, "  Skipping '%s' [%s] — create failed",
                     binding.role.c_str(), binding.driver_type.c_str());
        }
    }

    // Phase 2: Initialize all created drivers (видаляє невдалі)
    int failed = 0;
    for (size_t i = 0; i < sensors_.size(); ) {
        if (!sensors_[i].driver->init()) {
            ESP_LOGW(TAG, "Sensor '%s' init failed — disabled",
                     sensors_[i].role.c_str());
            sensors_.erase(sensors_.begin() + i);
            sensor_count_--;
            failed++;
        } else {
            i++;
        }
    }

    for (size_t i = 0; i < actuators_.size(); ) {
        if (!actuators_[i].driver->init()) {
            ESP_LOGW(TAG, "Actuator '%s' init failed — disabled",
                     actuators_[i].role.c_str());
            actuators_.erase(actuators_.begin() + i);
            actuator_count_--;
            failed++;
        } else {
            i++;
        }
    }

    if (failed > 0) {
        ESP_LOGW(TAG, "DriverManager: %d driver(s) failed, continuing with %d sensors, %d actuators",
                 failed, (int)sensor_count_, (int)actuator_count_);
    } else {
        ESP_LOGI(TAG, "DriverManager ready: %d sensors, %d actuators",
                 (int)sensor_count_, (int)actuator_count_);
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Driver creation
// ═══════════════════════════════════════════════════════════════

ISensorDriver* DriverManager::create_sensor(const Binding& binding, HAL& hal) {
    if (binding.driver_type == "ds18b20") {
        if (ds18b20_count >= MAX_SENSORS) {
            ESP_LOGE(TAG, "DS18B20 pool exhausted");
            return nullptr;
        }

        auto* ow_res = hal.find_onewire_bus(
            etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
        if (!ow_res) {
            ESP_LOGE(TAG, "OneWire bus '%s' not found in HAL",
                     binding.hardware_id.c_str());
            return nullptr;
        }

        auto& drv = ds18b20_pool[ds18b20_count++];
        drv.configure(binding.role.c_str(), ow_res->gpio, 1000,
                      binding.address.empty() ? nullptr : binding.address.c_str());
        return &drv;
    }

    return nullptr;
}

ISensorDriver* DriverManager::create_di_sensor(const Binding& binding, HAL& hal) {
    if (di_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "DigitalInput pool exhausted");
        return nullptr;
    }

    auto* gpio_res = hal.find_gpio_input(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!gpio_res) {
        ESP_LOGE(TAG, "GPIO input '%s' not found in HAL", binding.hardware_id.c_str());
        return nullptr;
    }

    auto& drv = di_pool[di_count++];
    drv.configure(binding.role.c_str(), gpio_res->gpio, gpio_res->pull_up);
    return &drv;
}

ISensorDriver* DriverManager::create_ntc_sensor(const Binding& binding, HAL& hal) {
    if (ntc_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "NTC pool exhausted");
        return nullptr;
    }

    auto* adc_res = hal.find_adc_channel(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!adc_res) {
        ESP_LOGE(TAG, "ADC channel '%s' not found in HAL", binding.hardware_id.c_str());
        return nullptr;
    }

    auto& drv = ntc_pool[ntc_count++];
    drv.configure(binding.role.c_str(), adc_res->gpio, adc_res->atten);
    return &drv;
}

ISensorDriver* DriverManager::create_pressure_sensor(const Binding& binding, HAL& hal) {
    if (pressure_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "Pressure pool exhausted");
        return nullptr;
    }

    auto* adc_res = hal.find_adc_channel(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!adc_res) {
        ESP_LOGE(TAG, "ADC channel '%s' not found in HAL", binding.hardware_id.c_str());
        return nullptr;
    }

    auto& drv = pressure_pool[pressure_count++];
    drv.configure(binding.role.c_str(), adc_res->gpio, adc_res->atten);
    return &drv;
}

IActuatorDriver* DriverManager::create_actuator(const Binding& binding, HAL& hal) {
    if (binding.driver_type == "relay") {
        if (relay_count >= MAX_ACTUATORS) {
            ESP_LOGE(TAG, "Relay pool exhausted");
            return nullptr;
        }

        auto* gpio_res = hal.find_gpio_output(
            etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
        if (!gpio_res) {
            ESP_LOGE(TAG, "GPIO output '%s' not found in HAL",
                     binding.hardware_id.c_str());
            return nullptr;
        }

        auto& drv = relay_pool[relay_count++];
        // min_switch_ms = 0 для всіх реле. Захист компресора від коротких циклів
        // реалізовано на рівні EquipmentModule (COMP_MIN_OFF_MS / COMP_MIN_ON_MS)
        // з асиметричними таймерами (180с OFF, 120с ON).
        drv.configure(binding.role.c_str(), gpio_res->gpio, gpio_res->active_high, 0);
        return &drv;
    }

    return nullptr;
}

IActuatorDriver* DriverManager::create_pcf_actuator(const Binding& binding, HAL& hal) {
    if (pcf_relay_count >= MAX_ACTUATORS) {
        ESP_LOGE(TAG, "PCF relay pool exhausted");
        return nullptr;
    }

    // Знайти expander output config по hardware_id ("relay_1")
    auto* out_cfg = hal.find_expander_output(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!out_cfg) {
        ESP_LOGE(TAG, "Expander output '%s' not found", binding.hardware_id.c_str());
        return nullptr;
    }

    // Знайти expander resource по expander_id ("relay_exp")
    auto* expander = hal.find_i2c_expander(
        etl::string_view(out_cfg->expander_id.c_str(), out_cfg->expander_id.size()));
    if (!expander) {
        ESP_LOGE(TAG, "Expander '%s' not found", out_cfg->expander_id.c_str());
        return nullptr;
    }

    auto& drv = pcf_relay_pool[pcf_relay_count++];
    drv.configure(binding.role.c_str(), expander, out_cfg->pin, out_cfg->active_high);
    return &drv;
}

IActuatorDriver* DriverManager::create_eev_pcf_stepper(const Binding& binding, HAL& hal) {
    if (eev_pcf_count >= MAX_VALVES) {
        ESP_LOGE(TAG, "EEV PCF8574 stepper pool exhausted");
        return nullptr;
    }

    // Binding hardware_id = "eev_port_1" → stepper_outputs section in board.json
    auto* stepper_cfg = hal.find_stepper_output(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!stepper_cfg) {
        ESP_LOGE(TAG, "Stepper output '%s' not found in HAL", binding.hardware_id.c_str());
        return nullptr;
    }

    // Find expander resource by expander_id
    auto* expander = hal.find_i2c_expander(
        etl::string_view(stepper_cfg->expander_id.c_str(), stepper_cfg->expander_id.size()));
    if (!expander) {
        ESP_LOGE(TAG, "Expander '%s' not found", stepper_cfg->expander_id.c_str());
        return nullptr;
    }

    uint16_t max_steps = 480;  // Default for E2V, configurable via SharedState

    auto& drv = eev_pcf_pool[eev_pcf_count++];
    drv.configure(binding.role.c_str(), expander, stepper_cfg->step_pin, stepper_cfg->dir_pin,
                  max_steps, "eev_pos");

    ESP_LOGI(TAG, "  EEV PCF8574 stepper '%s' STEP=pin%d DIR=pin%d max=%u on '%s'",
             binding.role.c_str(), stepper_cfg->step_pin, stepper_cfg->dir_pin, max_steps,
             expander->id.c_str());
    return &drv;
}

IActuatorDriver* DriverManager::create_eev_analog(const Binding& binding, HAL& hal) {
    if (eev_analog_count >= MAX_VALVES) {
        ESP_LOGE(TAG, "EEV analog pool exhausted");
        return nullptr;
    }

    auto* dac_res = hal.find_dac_channel(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!dac_res) {
        ESP_LOGE(TAG, "DAC channel '%s' not found in HAL", binding.hardware_id.c_str());
        return nullptr;
    }

    auto& drv = eev_analog_pool[eev_analog_count++];
    drv.configure(binding.role.c_str(), (int)dac_res->gpio, 255);

    ESP_LOGI(TAG, "  EEV analog '%s' on DAC GPIO %d",
             binding.role.c_str(), (int)dac_res->gpio);
    return &drv;
}

ISensorDriver* DriverManager::create_pcf_sensor(const Binding& binding, HAL& hal) {
    if (pcf_input_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "PCF input pool exhausted");
        return nullptr;
    }

    // Знайти expander input config по hardware_id ("din_1")
    auto* in_cfg = hal.find_expander_input(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!in_cfg) {
        ESP_LOGE(TAG, "Expander input '%s' not found", binding.hardware_id.c_str());
        return nullptr;
    }

    // Знайти expander resource
    auto* expander = hal.find_i2c_expander(
        etl::string_view(in_cfg->expander_id.c_str(), in_cfg->expander_id.size()));
    if (!expander) {
        ESP_LOGE(TAG, "Expander '%s' not found", in_cfg->expander_id.c_str());
        return nullptr;
    }

    auto& drv = pcf_input_pool[pcf_input_count++];
    drv.configure(binding.role.c_str(), expander, in_cfg->pin, in_cfg->invert);
    return &drv;
}

IActuatorDriver* DriverManager::create_eev_stepper(const Binding& binding, HAL& hal) {
    if (eev_stepper_count >= MAX_VALVES) {
        ESP_LOGE(TAG, "EEV stepper pool exhausted");
        return nullptr;
    }

    // GPIO-based stepper: hardware_id = GPIO output for STEP pin
    // For boards with stepper_outputs section, use find_stepper_output
    auto* stepper_cfg = hal.find_stepper_output(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!stepper_cfg) {
        // Fallback: try GPIO output (future boards with direct GPIO stepper drivers)
        auto* gpio_res = hal.find_gpio_output(
            etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
        if (!gpio_res) {
            ESP_LOGW(TAG, "Stepper output '%s' not found (no stepper_outputs or gpio_outputs)",
                     binding.hardware_id.c_str());
            return nullptr;
        }
        // GPIO stepper: step_gpio from resource, dir/enable from conventions
        EevStepperConfig cfg = {};
        cfg.step_gpio = (int)gpio_res->gpio;
        cfg.dir_gpio = -1;
        cfg.enable_gpio = -1;
        cfg.max_steps = 480;
        cfg.homing_extra_steps = 50;
        cfg.drive_freq_hz = 50;
        cfg.emergency_freq_hz = 150;
        cfg.invert_dir = false;
        cfg.nvs_namespace = "eev_pos";

        auto& drv = eev_stepper_pool[eev_stepper_count++];
        drv.configure(binding.role.c_str(), cfg);
        return &drv;
    }

    // stepper_outputs entry found — but eev_stepper expects GPIO pins, not expander
    // This driver type is for direct GPIO steppers (TMC2209), not PCF8574
    ESP_LOGW(TAG, "eev_stepper driver requires GPIO pins, not I2C expander. Use eev_pcf8574_stepper for '%s'",
             binding.hardware_id.c_str());
    return nullptr;
}

IActuatorDriver* DriverManager::create_akv_pulse(const Binding& binding, HAL& hal) {
    if (akv_count >= MAX_VALVES) {
        ESP_LOGE(TAG, "AKV pulse pool exhausted");
        return nullptr;
    }

    auto* gpio_res = hal.find_gpio_output(
        etl::string_view(binding.hardware_id.c_str(), binding.hardware_id.size()));
    if (!gpio_res) {
        ESP_LOGW(TAG, "GPIO output '%s' not found for AKV pulse driver",
                 binding.hardware_id.c_str());
        return nullptr;
    }

    auto& drv = akv_pool[akv_count++];
    drv.configure(binding.role.c_str(), (int)gpio_res->gpio, 6000, 10);

    ESP_LOGI(TAG, "  AKV pulse '%s' on GPIO %d (6s cycle)",
             binding.role.c_str(), (int)gpio_res->gpio);
    return &drv;
}

// ═══════════════════════════════════════════════════════════════
// Lookup
// ═══════════════════════════════════════════════════════════════

ISensorDriver* DriverManager::find_sensor(etl::string_view role) {
    for (auto& entry : sensors_) {
        if (entry.role.size() == role.size() &&
            etl::string_view(entry.role.c_str(), entry.role.size()) == role) {
            return entry.driver;
        }
    }
    return nullptr;
}

IActuatorDriver* DriverManager::find_actuator(etl::string_view role) {
    for (auto& entry : actuators_) {
        if (entry.role.size() == role.size() &&
            etl::string_view(entry.role.c_str(), entry.role.size()) == role) {
            return entry.driver;
        }
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════
// Update all drivers
// ═══════════════════════════════════════════════════════════════

void DriverManager::update_all(uint32_t dt_ms) {
    for (auto& entry : sensors_) {
        entry.driver->update(dt_ms);
    }
    for (auto& entry : actuators_) {
        entry.driver->update(dt_ms);
    }
}

} // namespace modesp
