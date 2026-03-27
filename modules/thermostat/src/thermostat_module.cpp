/**
 * @file thermostat_module.cpp
 * @brief Thermostat v2 — full spec_v3 logic
 *
 * State machine: STARTUP → IDLE ↔ COOLING, SAFETY_RUN (sensor failure)
 *
 * Asymmetric differential:
 *   ON:  T_air >= setpoint + differential
 *   OFF: T_air <= setpoint (after min_on_time elapsed)
 *
 * Defrost pause: при defrost.active термостат повністю зупиняється.
 *   Всі requests скидаються, таймери заморожені.
 *   Після відтайки comp_off_time скидається → min_off рахується заново.
 *   При газовій відтайці компресор працює від defrost, термостат не втручається.
 *
 * Equipment Layer: all requests via SharedState (thermostat.req.*)
 */

#include "thermostat_module.h"
#include "modesp/driver_messages.h"
#include "esp_log.h"
#include <ctime>

static const char* TAG = "Thermostat";

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

ThermostatModule::ThermostatModule()
    : BaseModule("thermostat", modesp::ModulePriority::NORMAL)
{}

// ═══════════════════════════════════════════════════════════════
// Sync settings з SharedState (WebUI/API може їх змінити)
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::sync_settings() {
    setpoint_     = read_float("thermostat.setpoint", setpoint_);
    differential_ = read_float("thermostat.differential", differential_);

    // Цілочисельні параметри (хвилини → мілісекунди)
    min_off_ms_       = static_cast<uint32_t>(read_int("thermostat.min_off_time", 3)) * 60000;
    min_on_ms_        = static_cast<uint32_t>(read_int("thermostat.min_on_time", 2)) * 60000;
    startup_delay_ms_ = static_cast<uint32_t>(read_int("thermostat.startup_delay", 1)) * 60000;
    evap_fan_mode_    = read_int("thermostat.evap_fan_mode", 1);
    fan_stop_temp_    = read_float("thermostat.fan_stop_temp", fan_stop_temp_);
    fan_stop_hyst_    = read_float("thermostat.fan_stop_hyst", fan_stop_hyst_);
    cond_fan_delay_ms_ = static_cast<uint32_t>(read_int("thermostat.cond_fan_delay", 30)) * 1000;

    // Хвилини → мілісекунди
    safety_on_ms_  = static_cast<uint32_t>(read_int("thermostat.safety_run_on", 20)) * 60000;
    safety_off_ms_ = static_cast<uint32_t>(read_int("thermostat.safety_run_off", 10)) * 60000;

    // Нічний режим
    night_setback_   = read_float("thermostat.night_setback", night_setback_);
    night_mode_      = read_int("thermostat.night_mode", 0);
    night_start_     = read_int("thermostat.night_start", 22);
    night_end_       = read_int("thermostat.night_end", 6);

    // Дисплей під час відтайки
    display_defrost_ = read_int("thermostat.display_defrost", 1);
}

// ═══════════════════════════════════════════════════════════════
// Night setback — визначення активності нічного режиму
// ═══════════════════════════════════════════════════════════════

bool ThermostatModule::is_night_active() {
    switch (night_mode_) {
        case 0: return false;  // Вимкнено
        case 1: {
            // За розкладом
            time_t now = time(nullptr);
            struct tm t;
            localtime_r(&now, &t);
            int hour = t.tm_hour;
            if (night_start_ > night_end_) {
                // Перетин через опівніч: наприклад 22..6
                return hour >= night_start_ || hour < night_end_;
            } else {
                // Без перетину: наприклад 2..8
                return hour >= night_start_ && hour < night_end_;
            }
        }
        case 2:
            // Через дискретний вхід
            if (!has_feature("night_di")) return false;
            return read_bool("equipment.night_input");
        case 3:
            // Вручну — через SharedState
            return read_bool("thermostat.night_active");
        default:
            return false;
    }
}

// ═══════════════════════════════════════════════════════════════
// Request helpers
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::request_compressor(bool on) {
    if (compressor_on_ != on) {
        compressor_on_ = on;
        // Скидаємо обидва таймери при зміні стану
        comp_on_time_ms_ = 0;
        comp_off_time_ms_ = 0;
        state_set("thermostat.req.compressor", on);
        ESP_LOGI(TAG, "Request compressor %s", on ? "ON" : "OFF");
    }
}

void ThermostatModule::request_evap_fan(bool on) {
    if (evap_fan_on_ != on) {
        evap_fan_on_ = on;
        state_set("thermostat.req.evap_fan", on);
    }
}

void ThermostatModule::request_cond_fan(bool on) {
    if (cond_fan_on_ != on) {
        cond_fan_on_ = on;
        state_set("thermostat.req.cond_fan", on);
    }
}

// ═══════════════════════════════════════════════════════════════
// State machine
// ═══════════════════════════════════════════════════════════════

const char* ThermostatModule::state_name() const {
    switch (state_) {
        case State::STARTUP:    return "startup";
        case State::IDLE:       return "idle";
        case State::COOLING:    return "cooling";
        case State::SAFETY_RUN: return "safety_run";
    }
    return "unknown";
}

void ThermostatModule::enter_state(State new_state) {
    if (state_ == new_state) return;

    ESP_LOGI(TAG, "%s → %s", state_name(),
             new_state == State::STARTUP    ? "startup" :
             new_state == State::IDLE       ? "idle" :
             new_state == State::COOLING    ? "cooling" :
             new_state == State::SAFETY_RUN ? "safety_run" : "?");

    state_ = new_state;
    state_timer_ms_ = 0;

    // Дії при вході в стан
    switch (new_state) {
        case State::STARTUP:
            request_compressor(false);
            break;
        case State::IDLE:
            request_compressor(false);
            break;
        case State::COOLING:
            request_compressor(true);
            break;
        case State::SAFETY_RUN:
            // Починаємо з ON фази
            safety_phase_on_ = true;
            safety_timer_ms_ = 0;
            request_compressor(true);
            ESP_LOGW(TAG, "SAFETY RUN — sensor failure, cyclic compressor");
            break;
    }

    state_set("thermostat.state", state_name());
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_init
// ═══════════════════════════════════════════════════════════════

bool ThermostatModule::on_init() {
    // PersistService вже відновив збережені значення з NVS → SharedState (Phase 1)
    sync_settings();

    // Публікуємо початковий стан
    state_set("thermostat.temperature", 0.0f);
    state_set("thermostat.setpoint", setpoint_);
    state_set("thermostat.differential", differential_);
    state_set("thermostat.req.compressor", false);
    state_set("thermostat.req.evap_fan", false);
    state_set("thermostat.req.cond_fan", false);
    state_set("thermostat.state", "startup");
    state_set("thermostat.comp_on_time", static_cast<int32_t>(0));
    state_set("thermostat.comp_off_time", static_cast<int32_t>(0));
    state_set("thermostat.night_active", false);
    state_set("thermostat.effective_setpoint", setpoint_);
    state_set("thermostat.display_temp", 0.0f);

    ESP_LOGI(TAG, "Initialized (setpoint=%.1f°C, differential=%.1f°C, state=startup)",
             setpoint_, differential_);
    ESP_LOGI(TAG, "  min_off=%lumin, min_on=%lumin, startup_delay=%lumin",
             min_off_ms_ / 60000, min_on_ms_ / 60000, startup_delay_ms_ / 60000);
    ESP_LOGI(TAG, "  evap_fan_mode=%ld, cond_fan_delay=%lus",
             evap_fan_mode_, cond_fan_delay_ms_ / 1000);
    ESP_LOGI(TAG, "Features: fan=%d, fan_temp=%d, cond_fan=%d",
             has_feature("fan_control"),
             has_feature("fan_temp_control"),
             has_feature("condenser_fan"));
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_update — головний цикл
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::on_update(uint32_t dt_ms) {
    // 1. Sync settings (WebUI/API може їх змінити)
    sync_settings();

    // 2. Читаємо inputs з SharedState
    current_temp_ = read_float("equipment.air_temp");
    evap_temp_    = read_float("equipment.evap_temp");
    sensor1_ok_   = read_bool("equipment.sensor1_ok");
    sensor2_ok_   = read_bool("equipment.sensor2_ok");
    defrost_active_ = read_bool("defrost.active");
    protection_lockout_ = read_bool("protection.lockout");

    // 3. Дзеркало температури для UI/MQTT
    if (sensor1_ok_) {
        state_set("thermostat.temperature", current_temp_);
    } else {
        state_set("thermostat.temperature", NAN);
        state_set("thermostat.display_temp", NAN);
    }

    // 3a. Нічний режим — обчислюємо effective_setpoint
    bool was_night = night_active_;
    night_active_ = is_night_active();
    effective_sp_ = setpoint_ + (night_active_ ? night_setback_ : 0.0f);
    if (night_active_ != was_night) {
        state_set("thermostat.night_active", night_active_);
        ESP_LOGI(TAG, "Night mode %s (effective SP=%.1f°C)",
                 night_active_ ? "ON" : "OFF", effective_sp_);
    }
    // Публікуємо тільки при зміні — уникаємо зайвих version bumps
    if (effective_sp_ != last_effective_sp_) {
        state_set("thermostat.effective_setpoint", effective_sp_);
        last_effective_sp_ = effective_sp_;
    }

    // ═══ Defrost pause ═══
    // Під час відтайки термостат повністю зупиняється:
    // - EM арбітрує: defrost.req.* має пріоритет
    // - При газовій відтайці компресор працює від defrost, термостат не втручається
    // - Таймери заморожені — після відтайки min_off_time рахується заново
    if (defrost_active_) {
        if (!was_defrost_active_) {
            request_compressor(false);
            request_evap_fan(false);
            request_cond_fan(false);
            // Фіксуємо T для display_defrost mode=1 (заморожена T)
            frozen_temp_ = current_temp_;
            state_set("thermostat.state", "paused");
            ESP_LOGI(TAG, "Defrost active — paused (frozen_temp=%.1f°C)", frozen_temp_);
        }
        // Дисплей під час відтайки
        switch (display_defrost_) {
            case 0:  state_set("thermostat.display_temp", current_temp_); break;
            case 1:  state_set("thermostat.display_temp", frozen_temp_);  break;
            default: state_set("thermostat.display_temp", -999.0f);       break;
        }
        was_defrost_active_ = true;
        publish_outputs();
        return;
    }

    // Defrost щойно завершився — скидаємо таймери і стан
    if (was_defrost_active_) {
        was_defrost_active_ = false;
        comp_off_time_ms_ = 0;
        comp_on_time_ms_ = 0;
        cond_fan_delay_active_ = false;
        cond_fan_off_timer_ms_ = 0;
        // Примусово в IDLE — щоб state machine заново оцінив і увійшов
        // в COOLING через enter_state() з request_compressor(true)
        state_ = State::IDLE;
        state_timer_ms_ = 0;
        state_set("thermostat.state", "idle");
        ESP_LOGI(TAG, "Defrost ended — state→idle, timers reset");
    }

    // Нормальна робота — display_temp = поточна температура (тільки при зміні)
    if (current_temp_ != last_display_temp_) {
        state_set("thermostat.display_temp", current_temp_);
        last_display_temp_ = current_temp_;
    }

    // 4. Оновлюємо таймери по ФАКТИЧНОМУ стану компресора (BUG-009 fix)
    state_timer_ms_ += dt_ms;
    bool comp_actual = read_bool("equipment.compressor");
    if (comp_actual) {
        comp_on_time_ms_ += dt_ms;
    } else {
        comp_off_time_ms_ += dt_ms;
    }

    // 5. Protection lockout — все вимкнути, не змінюємо state
    if (protection_lockout_) {
        request_compressor(false);
        request_evap_fan(false);
        request_cond_fan(false);
        was_lockout_active_ = true;
        return;
    }

    // 5a. Lockout щойно знявся — переввійти в поточний стан щоб reassert requests
    if (was_lockout_active_) {
        was_lockout_active_ = false;
        State saved = state_;
        state_ = State::STARTUP;    // force різний стан щоб enter_state не був no-op
        enter_state(saved);
        ESP_LOGI(TAG, "Lockout cleared — re-entered %s", state_name());
    }

    // 6. State machine
    switch (state_) {
        case State::STARTUP:
            // Чекаємо startup_delay, потім → IDLE
            if (state_timer_ms_ >= startup_delay_ms_) {
                ESP_LOGI(TAG, "Startup complete (%lu s)", startup_delay_ms_ / 1000);
                enter_state(State::IDLE);
            }
            break;

        case State::IDLE:
        case State::COOLING:
            // Перевіряємо sensor failure → SAFETY_RUN
            if (!sensor1_ok_) {
                enter_state(State::SAFETY_RUN);
                break;
            }
            update_regulation(dt_ms);
            break;

        case State::SAFETY_RUN:
            // Вихід при відновленні датчика
            if (sensor1_ok_) {
                ESP_LOGI(TAG, "Sensor restored — exiting Safety Run");
                enter_state(State::IDLE);
                break;
            }
            update_safety_run(dt_ms);
            break;
    }

    // 8. Управління вентиляторами (незалежно від state)
    update_evap_fan();
    update_cond_fan(dt_ms);

    // 9. Публікація лічильників
    publish_outputs();
}

// ═══════════════════════════════════════════════════════════════
// Регулювання: IDLE ↔ COOLING
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::update_regulation(uint32_t dt_ms) {
    (void)dt_ms;

    float upper = effective_sp_ + differential_;  // Поріг увімкнення
    float lower = effective_sp_;                  // Поріг вимкнення

    if (state_ == State::IDLE) {
        // Умови запуску (ВСІ одночасно)
        if (current_temp_ >= upper && comp_off_time_ms_ >= min_off_ms_) {
            ESP_LOGI(TAG, "IDLE → COOLING (temp=%.1f >= %.1f, off=%lu/%lu ms)",
                     current_temp_, upper, comp_off_time_ms_, min_off_ms_);
            enter_state(State::COOLING);
        } else if (current_temp_ >= upper && comp_off_time_ms_ < min_off_ms_) {
            ESP_LOGD(TAG, "Start delayed: min_off not elapsed (%lu/%lu ms)",
                     comp_off_time_ms_, min_off_ms_);
        }
    } else if (state_ == State::COOLING) {
        // Умови зупинки: T <= setpoint І мін. час роботи закінчився
        if (current_temp_ <= lower) {
            if (comp_on_time_ms_ >= min_on_ms_) {
                ESP_LOGI(TAG, "COOLING → IDLE (temp=%.1f <= %.1f, on=%lu/%lu ms)",
                         current_temp_, lower, comp_on_time_ms_, min_on_ms_);
                enter_state(State::IDLE);
            } else {
                ESP_LOGD(TAG, "Stop delayed: min_on not elapsed (%lu/%lu ms)",
                         comp_on_time_ms_, min_on_ms_);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Safety Run: циклічна робота компресора при відмові датчика
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::update_safety_run(uint32_t dt_ms) {
    safety_timer_ms_ += dt_ms;

    if (safety_phase_on_) {
        // ON фаза
        if (safety_timer_ms_ >= safety_on_ms_) {
            safety_phase_on_ = false;
            safety_timer_ms_ = 0;
            request_compressor(false);
            ESP_LOGD(TAG, "Safety Run: ON→OFF (%lu min elapsed)", safety_on_ms_ / 60000);
        }
    } else {
        // OFF фаза
        if (safety_timer_ms_ >= safety_off_ms_) {
            safety_phase_on_ = true;
            safety_timer_ms_ = 0;
            request_compressor(true);
            ESP_LOGD(TAG, "Safety Run: OFF→ON (%lu min elapsed)", safety_off_ms_ / 60000);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Вентилятор випарника (3 режими)
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::update_evap_fan() {
    // Примітка: під час defrost on_update() робить return раніше,
    // тому цей метод викликається тільки при нормальній роботі.

    if (!has_feature("fan_control")) {
        return;  // evap_fan не призначений → не керуємо
    }

    switch (evap_fan_mode_) {
        case 0:
            // Постійно ON
            request_evap_fan(true);
            break;

        case 1:
            // Синхронно з компресором
            request_evap_fan(compressor_on_);
            break;

        case 2: {
            // За T_evap: вимикається якщо T_evap > fan_stop_temp
            // Потрібен датчик випарника — fallback на mode 1
            if (!has_feature("fan_temp_control") || !sensor2_ok_) {
                request_evap_fan(compressor_on_);
                break;
            }
            // Гістерезис: ON при T_evap <= FST - hyst, OFF при T_evap > FST
            if (evap_fan_on_) {
                // Зараз ON — вимикаємо якщо T_evap > FST
                if (evap_temp_ > fan_stop_temp_) {
                    request_evap_fan(false);
                }
            } else {
                // Зараз OFF — вмикаємо якщо T_evap <= FST - hyst
                if (evap_temp_ <= (fan_stop_temp_ - fan_stop_hyst_)) {
                    request_evap_fan(true);
                }
            }
            break;
        }

        default:
            // Невідомий режим — fallback на mode 1
            request_evap_fan(compressor_on_);
            break;
    }
}

// ═══════════════════════════════════════════════════════════════
// Вентилятор конденсатора (з затримкою OFF)
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::update_cond_fan(uint32_t dt_ms) {
    if (!has_feature("condenser_fan")) {
        return;  // cond_fan не призначений → не керуємо
    }

    if (compressor_on_) {
        // Компресор ON → конд. вент. ON
        request_cond_fan(true);
        cond_fan_delay_active_ = false;
        cond_fan_off_timer_ms_ = 0;
    } else {
        // Компресор OFF → затримка перед вимкненням конд. вент.
        if (cond_fan_on_ && !cond_fan_delay_active_) {
            // Починаємо відлік затримки
            cond_fan_delay_active_ = true;
            cond_fan_off_timer_ms_ = 0;
        }

        if (cond_fan_delay_active_) {
            cond_fan_off_timer_ms_ += dt_ms;
            if (cond_fan_off_timer_ms_ >= cond_fan_delay_ms_) {
                request_cond_fan(false);
                cond_fan_delay_active_ = false;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Публікація стану
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::publish_outputs() {
    // track_change=false: таймери не тригерять WS delta broadcast
    state_set("thermostat.comp_on_time", static_cast<int32_t>(comp_on_time_ms_ / 1000), false);
    state_set("thermostat.comp_off_time", static_cast<int32_t>(comp_off_time_ms_ / 1000), false);
}

// ═══════════════════════════════════════════════════════════════
// Messages
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::on_message(const etl::imessage& msg) {
    // Setpoint change (від WebSocket, config тощо)
    if (msg.get_message_id() == modesp::msg_id::SETPOINT_CHANGED) {
        const auto& sp_msg = static_cast<const modesp::MsgSetpointChanged&>(msg);
        if (sp_msg.target == "thermostat") {
            setpoint_ = sp_msg.value;
            state_set("thermostat.setpoint", setpoint_);
            ESP_LOGI(TAG, "Setpoint changed: %.1f°C", setpoint_);
        }
        return;
    }

    // Safe Mode: аварійна зупинка через message bus
    if (msg.get_message_id() == modesp::msg_id::SYSTEM_SAFE_MODE) {
        request_compressor(false);
        request_evap_fan(false);
        request_cond_fan(false);
        ESP_LOGW(TAG, "SAFE MODE — all requests OFF");
        return;
    }
}

// ═══════════════════════════════════════════════════════════════
// Stop
// ═══════════════════════════════════════════════════════════════

void ThermostatModule::on_stop() {
    request_compressor(false);
    request_evap_fan(false);
    request_cond_fan(false);
    ESP_LOGI(TAG, "Thermostat stopped — all requests OFF");
}
