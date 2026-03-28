/**
 * @file equipment_module.cpp
 * @brief Equipment Manager — єдиний власник HAL drivers
 *
 * Потік даних кожен цикл:
 *   1. read_sensors()      — читає сенсори → SharedState
 *   2. read_requests()     — читає req.* від бізнес-модулів
 *   3. apply_arbitration() — арбітраж + інтерлоки → визначає outputs
 *   4. apply_outputs()     — застосовує outputs ДО реле (той самий цикл)
 *   5. publish_state()     — публікує фактичний стан актуаторів
 *
 * ВАЖЛИВО: apply_outputs() після apply_arbitration() в тому самому циклі.
 * Попередній порядок (apply_outputs першим) створював осциляцію через
 * однотактову затримку між рішенням арбітражу та його застосуванням.
 */

#include "equipment_module.h"
#include "modesp/hal/driver_manager.h"
#include "esp_log.h"
#include <cstring>
#include <cmath>

static const char TAG[] = "Equipment";

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

EquipmentModule::EquipmentModule()
    : BaseModule("equipment", modesp::ModulePriority::CRITICAL)
{}

// ═══════════════════════════════════════════════════════════════
// Driver binding — єдиний модуль з доступом до HAL
// ═══════════════════════════════════════════════════════════════

void EquipmentModule::bind_drivers(modesp::DriverManager& dm) {
    // Обов'язкові
    sensor_air_  = dm.find_sensor("air_temp");
    compressor_  = dm.find_actuator("compressor");

    // Опціональні сенсори
    sensor_evap_ = dm.find_sensor("evap_temp");
    sensor_cond_ = dm.find_sensor("condenser_temp");

    // Актуатори
    defrost_relay_ = dm.find_actuator("defrost_relay");
    evap_fan_      = dm.find_actuator("evap_fan");
    cond_fan_      = dm.find_actuator("cond_fan");

    // Дискретні входи
    door_sensor_  = dm.find_sensor("door_contact");
    night_sensor_ = dm.find_sensor("night_input");

    if (!sensor_air_) {
        ESP_LOGE(TAG, "Sensor 'air_temp' not found — REQUIRED");
    }
    if (!compressor_) {
        ESP_LOGE(TAG, "Actuator 'compressor' not found — REQUIRED");
    }
    if (sensor_evap_)   ESP_LOGI(TAG, "Evaporator sensor bound");
    if (sensor_cond_)   ESP_LOGI(TAG, "Condenser sensor bound");
    if (defrost_relay_) ESP_LOGI(TAG, "Defrost relay bound");
    if (evap_fan_)      ESP_LOGI(TAG, "Evaporator fan bound");
    if (cond_fan_)      ESP_LOGI(TAG, "Condenser fan bound");
    if (door_sensor_)   ESP_LOGI(TAG, "Door contact bound");
    if (night_sensor_)  ESP_LOGI(TAG, "Night input bound");

    light_ = dm.find_actuator("light");
    if (light_) ESP_LOGI(TAG, "Light relay bound");

    // Block H: EEV valve driver (optional)
    eev_driver_ = dm.find_actuator("eev");
    if (eev_driver_) ESP_LOGI(TAG, "EEV valve driver bound");

    // Per-zone driver binding (when zone_count_ >= 2)
    if (zone_count_ >= 2) {
        zones_[0].evap_sensor     = dm.find_sensor("evap_temp_z1");
        zones_[0].pressure_sensor = dm.find_sensor("suction_p_z1");
        zones_[0].defrost_relay   = dm.find_actuator("defrost_relay_z1");
        zones_[0].evap_fan        = dm.find_actuator("evap_fan_z1");
        zones_[0].eev_driver      = dm.find_actuator("eev_z1");

        zones_[1].evap_sensor     = dm.find_sensor("evap_temp_z2");
        zones_[1].pressure_sensor = dm.find_sensor("suction_p_z2");
        zones_[1].defrost_relay   = dm.find_actuator("defrost_relay_z2");
        zones_[1].evap_fan        = dm.find_actuator("evap_fan_z2");
        zones_[1].eev_driver      = dm.find_actuator("eev_z2");

        ESP_LOGI(TAG, "Multi-zone: %d zones configured", (int)zone_count_);
        for (size_t z = 0; z < zone_count_; z++) {
            ESP_LOGI(TAG, "  Zone %d: evap=%s press=%s defr=%s efan=%s eev=%s",
                     (int)(z + 1),
                     zones_[z].evap_sensor     ? "OK" : "-",
                     zones_[z].pressure_sensor ? "OK" : "-",
                     zones_[z].defrost_relay   ? "OK" : "-",
                     zones_[z].evap_fan        ? "OK" : "-",
                     zones_[z].eev_driver      ? "OK" : "-");
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

bool EquipmentModule::on_init() {
    if (!sensor_air_ || !compressor_) {
        ESP_LOGE(TAG, "Required drivers not bound — call bind_drivers() first");
        return false;
    }

    // Runtime zone count from NVS (persisted via equipment.active_zones)
    zone_count_ = static_cast<size_t>(read_int(ns_key("active_zones"), 1));
    if (zone_count_ < 1) zone_count_ = 1;
    if (zone_count_ > MAX_ZONES) zone_count_ = MAX_ZONES;
    state_set(ns_key("active_zones"), static_cast<int32_t>(zone_count_));
    ESP_LOGI(TAG, "Active zones: %d", (int)zone_count_);

    // Publish zone enable flags — zone modules check this to skip on_update()
    state_set("equipment.zone2_enabled", zone_count_ >= 2);
    state_set("equipment.zone3_enabled", zone_count_ >= 3);
    state_set("equipment.zone4_enabled", zone_count_ >= 4);

    // Початковий стан в SharedState
    // Оптимістична ініціалізація: sensor_ok = true щоб Protection
    // не спрацьовувала ERR1/ERR2 до першого реального read().
    // DS18B20 потребує ~750ms на першу конверсію — перші read() фейляться.
    // Якщо датчик не сконфігурований — теж true (не помилка).
    state_set(ns_key("air_temp"), 0.0f);
    state_set(ns_key("evap_temp"), 0.0f);
    state_set(ns_key("cond_temp"), 0.0f);
    state_set(ns_key("sensor1_ok"), true);
    state_set(ns_key("sensor2_ok"), true);
    state_set(ns_key("compressor"), false);
    state_set(ns_key("defrost_relay"), false);
    state_set(ns_key("evap_fan"), false);
    state_set(ns_key("cond_fan"), false);
    state_set(ns_key("door_open"), false);
    state_set(ns_key("night_input"), false);

    // Публікуємо наявність опціонального обладнання —
    // UI (visible_when, disabled options) та бізнес-модулі перевіряють
    state_set(ns_key("has_defrost_relay"), defrost_relay_ != nullptr);
    state_set(ns_key("has_cond_fan"), cond_fan_ != nullptr);
    state_set(ns_key("has_door_contact"), door_sensor_ != nullptr);
    state_set(ns_key("has_evap_temp"), sensor_evap_ != nullptr);
    state_set(ns_key("has_cond_temp"), sensor_cond_ != nullptr);
    state_set(ns_key("has_night_input"), night_sensor_ != nullptr);
    state_set(ns_key("light"), false);
    state_set(ns_key("has_light"), light_ != nullptr);

    // Тип драйверів — для visibility карток налаштувань в UI
    // ISensorDriver::type() повертає "ds18b20", "ntc", "digital_input" тощо
    bool uses_ntc = false;
    bool uses_ds18b20 = false;
    auto check_type = [&](modesp::ISensorDriver* s) {
        if (!s) return;
        if (strcmp(s->type(), "ntc") == 0) uses_ntc = true;
        if (strcmp(s->type(), "ds18b20") == 0) uses_ds18b20 = true;
    };
    check_type(sensor_air_);
    check_type(sensor_evap_);
    check_type(sensor_cond_);
    state_set(ns_key("has_ntc_driver"), uses_ntc);
    state_set(ns_key("has_ds18b20_driver"), uses_ds18b20);

    // Per-zone initial state (when zone_count >= 2)
    if (zone_count_ >= 2) {
        for (size_t z = 0; z < zone_count_; z++) {
            char key[48];
            int zn = (int)(z + 1);

            snprintf(key, sizeof(key), "equipment.evap_temp_z%d", zn);
            state_set(key, 0.0f);

            snprintf(key, sizeof(key), "equipment.sensor2_z%d_ok", zn);
            state_set(key, true);

            snprintf(key, sizeof(key), "equipment.suction_bar_z%d", zn);
            state_set(key, 0.0f);

            snprintf(key, sizeof(key), "equipment.has_suction_p_z%d", zn);
            state_set(key, zones_[z].pressure_sensor != nullptr);

            snprintf(key, sizeof(key), "equipment.has_defrost_relay_z%d", zn);
            state_set(key, zones_[z].defrost_relay != nullptr);
        }
    }

    ESP_LOGI(TAG, "Initialized (air_sensor=%s, compressor=%s, ntc=%s, ds18b20=%s)",
             sensor_air_ ? "OK" : "MISSING",
             compressor_ ? "OK" : "MISSING",
             uses_ntc ? "yes" : "no",
             uses_ds18b20 ? "yes" : "no");
    return true;
}

void EquipmentModule::on_update(uint32_t dt_ms) {
    // Runtime zone count update (WebUI can change equipment.active_zones)
    auto new_zones = static_cast<size_t>(read_int(ns_key("active_zones"), 1));
    if (new_zones >= 1 && new_zones <= MAX_ZONES && new_zones != zone_count_) {
        zone_count_ = new_zones;
        state_set("equipment.zone2_enabled", zone_count_ >= 2);
        state_set("equipment.zone3_enabled", zone_count_ >= 3);
        state_set("equipment.zone4_enabled", zone_count_ >= 4);
        ESP_LOGI(TAG, "Zone count changed to %d", (int)zone_count_);
    }

    // AUDIT-003: оновлюємо таймер компресора
    comp_since_ms_ += dt_ms;
    if (comp_since_ms_ > modesp::TIMER_SATISFIED) comp_since_ms_ = modesp::TIMER_SATISFIED;

    // 1. Читаємо сенсори → SharedState
    read_sensors();

    // 2. Читаємо requests від бізнес-модулів
    read_requests();

    // 3. Арбітраж + інтерлоки → визначаємо outputs
    apply_arbitration();

    // 4. Застосовуємо outputs до реле (в тому самому циклі!)
    //    Раніше apply_outputs() був першим і застосовував out_ з ПОПЕРЕДНЬОГО циклу.
    //    Це створювало осциляцію: anti-short-cycle блокував ON → out_=false,
    //    але relay вже ввімкнулось з попереднього out_=true → toggle кожен цикл.
    apply_outputs();

    // 5. Публікуємо фактичний стан актуаторів
    publish_state();
}

void EquipmentModule::on_message(const etl::imessage& msg) {
    // Safe mode — все вимкнути (NEW-001 fix: було 1, а SYSTEM_SAFE_MODE = 7)
    if (msg.get_message_id() == modesp::msg_id::SYSTEM_SAFE_MODE) {
        out_ = {};
        apply_outputs();
        publish_state();
        ESP_LOGW(TAG, "SAFE MODE — all outputs OFF");
    }
}

void EquipmentModule::on_stop() {
    // Аварійна зупинка — все вимкнути
    out_ = {};
    apply_outputs();

    // Block H: Emergency close EEV valve
    if (eev_driver_) {
        eev_driver_->emergency_stop();
        ESP_LOGW(TAG, "EEV valve emergency close on stop");
    }

    ESP_LOGI(TAG, "Equipment stopped — all outputs OFF, valve closed");
}

// ═══════════════════════════════════════════════════════════════
// Читання сенсорів
// ═══════════════════════════════════════════════════════════════

void EquipmentModule::read_sensors() {
    // Коефіцієнт цифрового фільтра (0 = вимкнено, 1-10 = EMA)
    int coeff = read_int(ns_key("filter_coeff"), 4);
    float alpha = (coeff > 0) ? 1.0f / (coeff + 1) : 1.0f;

    // Датчик камери (обов'язковий)
    if (sensor_air_) {
        float temp = 0.0f;
        if (sensor_air_->read(temp)) {
            if (!ema_air_init_) { ema_air_ = temp; ema_air_init_ = true; }
            else { ema_air_ += (temp - ema_air_) * alpha; }
            air_temp_ = roundf(ema_air_ * 100.0f) / 100.0f;
            state_set(ns_key("air_temp"), air_temp_);
        } else if (!sensor_air_->is_healthy()) {
            air_temp_ = NAN;
            state_set(ns_key("air_temp"), NAN);
        }
        state_set(ns_key("sensor1_ok"), sensor_air_->is_healthy());
    }

    // Датчик випарника (опціональний)
    if (sensor_evap_) {
        float temp = 0.0f;
        if (sensor_evap_->read(temp)) {
            if (!ema_evap_init_) { ema_evap_ = temp; ema_evap_init_ = true; }
            else { ema_evap_ += (temp - ema_evap_) * alpha; }
            evap_temp_ = roundf(ema_evap_ * 100.0f) / 100.0f;
            state_set(ns_key("evap_temp"), evap_temp_);
        } else if (!sensor_evap_->is_healthy()) {
            evap_temp_ = NAN;
            state_set(ns_key("evap_temp"), NAN);
        }
        state_set(ns_key("sensor2_ok"), sensor_evap_->is_healthy());
    }

    // Датчик конденсатора (опціональний — DS18B20 або NTC)
    if (sensor_cond_) {
        float temp = 0.0f;
        if (sensor_cond_->read(temp)) {
            if (!ema_cond_init_) { ema_cond_ = temp; ema_cond_init_ = true; }
            else { ema_cond_ += (temp - ema_cond_) * alpha; }
            cond_temp_ = roundf(ema_cond_ * 100.0f) / 100.0f;
            state_set(ns_key("cond_temp"), cond_temp_);
        } else if (!sensor_cond_->is_healthy()) {
            cond_temp_ = NAN;
            state_set(ns_key("cond_temp"), NAN);
        }
    }

    // Контакт дверей (опціональний — digital_input)
    if (door_sensor_) {
        float val = 0.0f;
        door_sensor_->read(val);
        state_set(ns_key("door_open"), val > 0.5f);
    }

    // Дискретний вхід нічного режиму (опціональний)
    if (night_sensor_) {
        float val = 0.0f;
        night_sensor_->read(val);
        state_set(ns_key("night_input"), val > 0.5f);
    }

    // Per-zone sensors (when zone_count >= 2)
    if (zone_count_ >= 2) {
        for (size_t z = 0; z < zone_count_; z++) {
            char key[48];
            int zn = (int)(z + 1);
            float val = 0.0f;

            // Per-zone evaporator temperature
            if (zones_[z].evap_sensor) {
                if (zones_[z].evap_sensor->read(val)) {
                    snprintf(key, sizeof(key), "equipment.evap_temp_z%d", zn);
                    state_set(key, roundf(val * 100.0f) / 100.0f);
                }
                snprintf(key, sizeof(key), "equipment.sensor2_z%d_ok", zn);
                state_set(key, zones_[z].evap_sensor->is_healthy());
            }

            // Per-zone suction pressure
            if (zones_[z].pressure_sensor) {
                if (zones_[z].pressure_sensor->read(val)) {
                    snprintf(key, sizeof(key), "equipment.suction_bar_z%d", zn);
                    state_set(key, roundf(val * 100.0f) / 100.0f);
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Читання requests з SharedState
// ═══════════════════════════════════════════════════════════════

void EquipmentModule::read_requests() {
    // Block I: Read requests from all zones (OR aggregation)
    read_zone_requests();

    // Protection (shared across all zones)
    req_.protection_lockout  = read_input_bool("protection.lockout");
    req_.compressor_blocked  = read_input_bool("protection.compressor_blocked");
    req_.condenser_blocked   = read_input_bool("protection.condenser_block");
    req_.door_comp_blocked   = read_input_bool("protection.door_comp_blocked");

    // Lighting (independent)
    req_.light_request = read_input_bool("lighting.req.light");
}

void EquipmentModule::read_zone_requests() {
    // Reset aggregated requests
    req_.any_therm_compressor = false;
    req_.any_therm_evap_fan   = false;
    req_.any_therm_cond_fan   = false;
    req_.any_defrost_active   = false;
    req_.any_def_compressor   = false;
    req_.any_def_defrost_relay = false;
    req_.any_def_evap_fan     = false;
    req_.any_def_cond_fan     = false;

    // Read from each zone and OR together
    char key_buf[64];
    for (size_t z = 0; z < zone_count_; ++z) {
        auto& zone = zones_[z];

        // Thermostat requests (via zone namespace)
        auto make_key = [&key_buf](const char* ns, const char* suffix) -> const char* {
            char* p = key_buf;
            const char* s = ns;
            while (*s) *p++ = *s++;
            *p++ = '.';
            s = suffix;
            while (*s) *p++ = *s++;
            *p = '\0';
            return key_buf;
        };

        zone.therm_compressor = read_bool(make_key(zone.thermo_ns, "req.compressor"));
        zone.therm_evap_fan   = read_bool(make_key(zone.thermo_ns, "req.evap_fan"));
        zone.therm_cond_fan   = read_bool(make_key(zone.thermo_ns, "req.cond_fan"));

        zone.defrost_active    = read_bool(make_key(zone.defrost_ns, "active"));
        zone.def_compressor    = read_bool(make_key(zone.defrost_ns, "req.compressor"));
        zone.def_defrost_relay = read_bool(make_key(zone.defrost_ns, "req.defrost_relay"));
        zone.def_evap_fan      = read_bool(make_key(zone.defrost_ns, "req.evap_fan"));
        zone.def_cond_fan      = read_bool(make_key(zone.defrost_ns, "req.cond_fan"));

        zone.eev_valve_pos     = read_float(make_key(zone.eev_ns, "req.valve_pos"));

        // OR aggregation
        req_.any_therm_compressor |= zone.therm_compressor;
        req_.any_therm_evap_fan   |= zone.therm_evap_fan;
        req_.any_therm_cond_fan   |= zone.therm_cond_fan;
        req_.any_defrost_active   |= zone.defrost_active;
        req_.any_def_compressor   |= zone.def_compressor;
        req_.any_def_defrost_relay |= zone.def_defrost_relay;
        req_.any_def_evap_fan     |= zone.def_evap_fan;
        req_.any_def_cond_fan     |= zone.def_cond_fan;
    }
}

// ═══════════════════════════════════════════════════════════════
// Арбітраж + інтерлоки
// ═══════════════════════════════════════════════════════════════

void EquipmentModule::apply_arbitration() {
    // Protection lockout = все вимкнено (найвищий пріоритет)
    if (req_.protection_lockout) {
        out_ = {};
        return;
    }

    // Defrost active = defrost requests мають пріоритет (any zone)
    if (req_.any_defrost_active) {
        out_.compressor    = req_.any_def_compressor;
        out_.defrost_relay = req_.any_def_defrost_relay;
        out_.evap_fan      = req_.any_def_evap_fan;
        out_.cond_fan      = req_.any_def_cond_fan;
        // Логуємо тільки при вході в defrost (не кожен цикл)
        if (!prev_defrost_active_) {
            ESP_LOGI(TAG, "DEFROST arb START: comp=%d relay=%d efan=%d cfan=%d",
                     out_.compressor, out_.defrost_relay, out_.evap_fan, out_.cond_fan);
        }
    } else {
        // Логуємо при виході з defrost
        if (prev_defrost_active_) {
            ESP_LOGI(TAG, "DEFROST arb END — normal mode restored");
        }
        // Нормальний режим: thermostat requests (OR across all zones)
        out_.compressor    = req_.any_therm_compressor;
        out_.defrost_relay = false;   // Тільки defrost може ввімкнути
        out_.evap_fan      = req_.any_therm_evap_fan;
        out_.cond_fan      = req_.any_therm_cond_fan;
    }

    // === AUDIT-003: Compressor anti-short-cycle (output-level) ===
    // Захищає компресор незалежно від джерела запиту (thermostat/defrost).
    // Працює на фактичному стані реле, а не на запитах бізнес-модулів.
    // ВАЖЛИВО: виконується ДО інтерлоків, щоб інтерлоки мали фінальне слово.
    if (out_.compressor != comp_actual_) {
        if (out_.compressor) {
            // Запит на ввімкнення — перевіряємо min OFF time
            if (comp_since_ms_ < COMP_MIN_OFF_MS) {
                out_.compressor = false;
                ESP_LOGD(TAG, "Compressor ON blocked — min OFF (%lu/%lu ms)",
                         comp_since_ms_, COMP_MIN_OFF_MS);
            }
        } else {
            // Запит на вимкнення — перевіряємо min ON time
            if (comp_since_ms_ < COMP_MIN_ON_MS) {
                out_.compressor = true;  // Тримаємо ON
                ESP_LOGD(TAG, "Compressor OFF blocked — min ON (%lu/%lu ms)",
                         comp_since_ms_, COMP_MIN_ON_MS);
            }
        }
    }

    // === Protection compressor block (forced off при continuous run) ===
    // Тільки компресор OFF, вентилятори працюють за нормальним арбітражем.
    if (req_.compressor_blocked || req_.condenser_blocked || req_.door_comp_blocked) {
        out_.compressor = false;
    }

    // === Per-zone defrost relay arbitration (zone_count >= 2) ===
    // Each zone controls its own defrost relay independently.
    // The shared defrost_relay_ is used only in single-zone mode.
    // Per-zone evap fans follow zone defrost state when in defrost mode.

    // === ІНТЕРЛОКИ (hardcoded, неможливо обійти) ===
    // Виконуються ОСТАННІМИ — мають найвищий пріоритет після protection lockout.

    // Електрична відтайка (тен) і компресор НІКОЛИ одночасно.
    // Гарячий газ: компресор потрібен — інтерлок НЕ застосовується.
    // Перевіряємо defrost.type щоб визначити чи реле = тен (type=1).
    if (out_.defrost_relay && out_.compressor) {
        int defrost_type = read_input_int("defrost.type", 0);
        if (defrost_type == 1) {
            // Електричний тен — компресор OFF
            out_.compressor = false;
            ESP_LOGW(TAG, "INTERLOCK: heater+compressor → compressor OFF");
        }
        // defrost_type == 2 (ГГ): обидва ON — це нормально
    }

    // Оновлюємо tracking для delta-логування
    prev_defrost_active_ = req_.any_defrost_active;

    // === Block H: Valve safety — close valve when compressor OFF ===
    // Відкритий клапан при зупиненому компресорі → рідкий фреон →
    // гідроудар при наступному старті → поломка компресора.
    // Клапан ЗАВЖДИ закривається раніше або одночасно з компресором.
    if (!out_.compressor) {
        // Compressor OFF → valve must be closed (0%)
        out_.valve_pos = 0.0f;
    } else {
        // Compressor ON → use EEV module request
        out_.valve_pos = read_input_float("eev.req.valve_pos", 0.0f);
    }

    // === Освітлення — незалежне від refrigeration arbitration ===
    // Не залежить від lockout, defrost, protection
    out_.light = req_.light_request;
}

// ═══════════════════════════════════════════════════════════════
// Застосування outputs до relay
// ═══════════════════════════════════════════════════════════════

void EquipmentModule::apply_outputs() {
    if (compressor_)    compressor_->set(out_.compressor);
    if (cond_fan_)      cond_fan_->set(out_.cond_fan);
    if (light_)         light_->set(out_.light);

    // Per-zone outputs: defrost relay + evap fan per zone
    if (zone_count_ >= 2) {
        for (size_t z = 0; z < zone_count_; z++) {
            if (zones_[z].defrost_relay) {
                zones_[z].defrost_relay->set(zones_[z].def_defrost_relay);
            }
            if (zones_[z].evap_fan) {
                // During defrost: zone defrost controls fan; otherwise thermostat
                if (zones_[z].defrost_active) {
                    zones_[z].evap_fan->set(zones_[z].def_evap_fan);
                } else {
                    zones_[z].evap_fan->set(zones_[z].therm_evap_fan);
                }
            }
            // Per-zone EEV valve
            if (zones_[z].eev_driver && out_.compressor) {
                zones_[z].eev_driver->set_value(zones_[z].eev_valve_pos / 100.0f);
            } else if (zones_[z].eev_driver) {
                zones_[z].eev_driver->set_value(0.0f);  // Close when compressor OFF
            }
        }
    } else {
        // Single zone: existing behavior
        if (defrost_relay_) defrost_relay_->set(out_.defrost_relay);
        if (evap_fan_)      evap_fan_->set(out_.evap_fan);
    }

    // Block H: EEV valve output — apply to IValveDriver if bound (single-zone)
    if (eev_driver_) {
        // Emergency close request from EEV module (subcooled SH < 0)
        // Uses emergency_stop() → IValveDriver::emergency_close() at 150Hz
        if (read_input_bool("eev.req.emergency_close")) {
            eev_driver_->emergency_stop();
            state_set("eev.req.emergency_close", false);  // ACK — одноразовий прапорець
        } else {
            eev_driver_->set_value(out_.valve_pos / 100.0f);  // 0-100% → 0.0-1.0
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Публікація фактичного стану актуаторів
// ═══════════════════════════════════════════════════════════════

void EquipmentModule::publish_state() {
    // AUDIT-002: публікуємо ФАКТИЧНИЙ стан реле (get_state()), а не бажаний (out_)
    bool comp_now = compressor_ ? compressor_->get_state() : false;

    // AUDIT-003: відстежуємо зміну фактичного стану компресора для таймера
    if (comp_now != comp_actual_) {
        comp_since_ms_ = 0;
        comp_actual_ = comp_now;
        ESP_LOGI(TAG, "Compressor → %s", comp_now ? "ON" : "OFF");
    }

    state_set(ns_key("compressor"),    comp_now);
    state_set(ns_key("defrost_relay"), defrost_relay_ ? defrost_relay_->get_state() : false);
    state_set(ns_key("evap_fan"),      evap_fan_      ? evap_fan_->get_state()      : false);
    state_set(ns_key("cond_fan"),      cond_fan_      ? cond_fan_->get_state()      : false);
    state_set(ns_key("light"),         light_         ? light_->get_state()         : false);
}
