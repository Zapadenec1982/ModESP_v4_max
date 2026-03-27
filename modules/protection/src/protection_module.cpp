/**
 * @file protection_module.cpp
 * @brief Protection module — alarm monitoring, compressor safety, rate diagnostics
 *
 * Monitors:
 *   1. High Temp (HAL) — delayed, blocked during defrost
 *   2. Low Temp (LAL)  — delayed, always active
 *   3. Sensor1 (ERR1)  — instant, air sensor failure
 *   4. Sensor2 (ERR2)  — instant, evap sensor failure (info only)
 *   5. Door open       — delayed, info only
 *   6. Short Cycling   — 3 consecutive runs < min_compressor_run
 *   7. Rapid Cycling   — starts > max_starts_hour in rolling 60-min window
 *   8. Continuous Run   — compressor ON > max_continuous_run
 *   9. Pulldown Failure — compressor ON > pulldown_timeout, temp not dropped
 *  10. Rate-of-Change   — temp rises > max_rise_rate while compressor ON
 *
 * Continuous run escalation:
 *   Level 1: compressor_blocked = true (forced off, fans still run)
 *   Level 2: lockout = true (permanent, all OFF, manual reset required)
 */

#include "protection_module.h"
#include "esp_log.h"

#include <cmath>   // fabsf, fmaxf

static const char* TAG = "Protection";

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

ProtectionModule::ProtectionModule()
    : BaseModule("protection", modesp::ModulePriority::HIGH)
{}

// ═══════════════════════════════════════════════════════════════
// Sync settings з SharedState (WebUI/API може їх змінити)
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::sync_settings() {
    high_limit_ = read_float("protection.high_limit", high_limit_);
    low_limit_  = read_float("protection.low_limit", low_limit_);

    // Хвилини → мілісекунди (окремі затримки для HAL і LAL)
    high_alarm_delay_ms_ = static_cast<uint32_t>(read_int("protection.high_alarm_delay", 30)) * 60000;
    low_alarm_delay_ms_  = static_cast<uint32_t>(read_int("protection.low_alarm_delay", 30)) * 60000;
    door_delay_ms_  = static_cast<uint32_t>(read_int("protection.door_delay", 5)) * 60000;

    manual_reset_ = read_bool("protection.manual_reset", manual_reset_);

    // Хвилини → мілісекунди (затримка аварії високої T після відтайки)
    post_defrost_delay_ms_ = static_cast<uint32_t>(
        read_int("protection.post_defrost_delay", 30)) * 60000;

    // Компресорний захист
    min_compressor_run_ms_ = static_cast<uint32_t>(
        read_int("protection.min_compressor_run", 120)) * 1000;
    max_starts_hour_ = read_int("protection.max_starts_hour", 12);
    max_continuous_run_ms_ = static_cast<uint32_t>(
        read_int("protection.max_continuous_run", 360)) * 60000;
    pulldown_timeout_ms_ = static_cast<uint32_t>(
        read_int("protection.pulldown_timeout", 60)) * 60000;
    pulldown_min_drop_ = read_float("protection.pulldown_min_drop", 2.0f);
    max_rise_rate_ = read_float("protection.max_rise_rate", 0.5f);
    rate_duration_ms_ = static_cast<uint32_t>(
        read_int("protection.rate_duration", 5)) * 60000;
    // Ескалація continuous run
    forced_off_period_ms_ = static_cast<uint32_t>(
        read_int("protection.forced_off_period", 20)) * 60000;
    max_retries_ = read_int("protection.max_retries", 3);

    // Condenser protection (like Danfoss A37/A54)
    condenser_alarm_limit_ = read_float("protection.condenser_alarm_limit", 80.0f);
    condenser_block_limit_ = read_float("protection.condenser_block_limit", 85.0f);

    // Door → compressor delay (like Danfoss C04, seconds)
    door_comp_delay_ms_ = static_cast<uint32_t>(
        read_int("protection.door_comp_delay", 900)) * 1000;

    // compressor_hours_ читається ТІЛЬКИ в on_init() —
    // тут не перечитуємо, бо модуль акумулює значення між записами в state
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_init
// ═══════════════════════════════════════════════════════════════

bool ProtectionModule::on_init() {
    // PersistService вже відновив збережені значення з NVS → SharedState (Phase 1)
    sync_settings();
    // compressor_hours — акумулятивний наробіток, читаємо тільки раз при init
    compressor_hours_ = read_float("protection.compressor_hours", 0.0f);

    // Початковий стан — існуючі
    state_set("protection.lockout", false);
    state_set("protection.alarm_active", false);
    state_set("protection.alarm_code", "none");
    state_set("protection.high_temp_alarm", false);
    state_set("protection.low_temp_alarm", false);
    state_set("protection.sensor1_alarm", false);
    state_set("protection.sensor2_alarm", false);
    state_set("protection.door_alarm", false);
    state_set("protection.reset_alarms", false);

    // Ескалація continuous run
    state_set("protection.compressor_blocked", false);
    state_set("protection.continuous_run_count", static_cast<int32_t>(0));

    // Початковий стан — компресорний захист
    state_set("protection.short_cycle_alarm", false);
    state_set("protection.rapid_cycle_alarm", false);
    state_set("protection.continuous_run_alarm", false);
    state_set("protection.pulldown_alarm", false);
    state_set("protection.rate_alarm", false);
    state_set("protection.compressor_starts_1h", (int32_t)0);
    state_set("protection.compressor_duty", 0.0f);
    state_set("protection.compressor_run_time", (int32_t)0);
    state_set("protection.last_cycle_run", (int32_t)0);
    state_set("protection.last_cycle_off", (int32_t)0);
    // compressor_hours вже в SharedState від PersistService

    ESP_LOGI(TAG, "Initialized (HAL=%.1f°C/%lumin, LAL=%.1f°C/%lumin)",
             high_limit_, high_alarm_delay_ms_ / 60000,
             low_limit_, low_alarm_delay_ms_ / 60000);
    ESP_LOGI(TAG, "Features: door=%d, compressor=%d, rate=%d",
             has_feature("door_protection"),
             has_feature("compressor_protection"),
             has_feature("rate_protection"));
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_update — головний цикл
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::on_update(uint32_t dt_ms) {
    // 1. Sync settings
    sync_settings();

    // 2. Читаємо inputs з SharedState
    float air_temp   = read_float("equipment.air_temp");
    bool  sensor1_ok = read_bool("equipment.sensor1_ok");
    bool  sensor2_ok = read_bool("equipment.sensor2_ok");
    bool  door_open  = read_bool("equipment.door_open");
    bool  defrost    = read_bool("defrost.active");

    // Визначаємо чи defrost у фазі нагріву (BUG-007 fix)
    // HAL alarm блокується тільки в heating-фазах: stabilize, valve_open, active, equalize
    bool defrost_heating = false;
    if (defrost) {
        auto phase_val = state_get("defrost.phase");
        if (phase_val.has_value()) {
            const auto* sp = etl::get_if<etl::string<32>>(&phase_val.value());
            if (sp && (*sp == "active" || *sp == "stabilize" ||
                       *sp == "valve_open" || *sp == "equalize")) {
                defrost_heating = true;
            }
        }
    }

    // 3. Post-defrost suppression — блокуємо HAL + Rate alarm після відтайки
    if (defrost && !was_defrost_active_) {
        // Початок відтайки
        post_defrost_suppression_ = false;
        post_defrost_timer_ms_ = 0;
    }
    if (!defrost && was_defrost_active_) {
        // Кінець відтайки → починаємо suppress timer
        post_defrost_suppression_ = true;
        post_defrost_timer_ms_ = 0;
        ESP_LOGI(TAG, "Post-defrost suppression started (%lu min)",
                 post_defrost_delay_ms_ / 60000);
    }
    was_defrost_active_ = defrost;

    if (post_defrost_suppression_) {
        post_defrost_timer_ms_ += dt_ms;
        if (post_defrost_timer_ms_ >= post_defrost_delay_ms_) {
            post_defrost_suppression_ = false;
            ESP_LOGI(TAG, "Post-defrost suppression ended");
        }
    }

    // Сумарний suppress для HAL + Rate alarm: heating-фази відтайки АБО post-defrost
    bool suppress_high = defrost_heating || post_defrost_suppression_;

    // 3a. Перевіряємо команду скидання аварій
    check_reset_command();

    // 4. Оновлюємо існуючі монітори
    update_high_temp(air_temp, sensor1_ok, suppress_high, dt_ms);
    update_low_temp(air_temp, sensor1_ok, dt_ms);
    update_sensor_alarm(sensor1_, sensor1_ok, "SENSOR1 (ERR1)");
    // sensor2 (evap_temp) — тільки якщо датчик підключений в bindings
    if (read_bool("equipment.has_evap_temp")) {
        update_sensor_alarm(sensor2_, sensor2_ok, "SENSOR2 (ERR2)");
    } else if (sensor2_.active) {
        // Якщо датчик відключили — скинути хибний алярм
        sensor2_.active = false;
    }
    if (has_feature("door_protection")) {
        update_door_alarm(door_open, dt_ms);

        // Door → compressor delay: block compressor if door open too long
        if (door_open) {
            door_comp_timer_ms_ += dt_ms;
            if (door_comp_timer_ms_ >= door_comp_delay_ms_ && !door_comp_blocked_) {
                door_comp_blocked_ = true;
                ESP_LOGW(TAG, "DOOR COMPRESSOR BLOCK — open > %lu s",
                         door_comp_delay_ms_ / 1000);
            }
        } else {
            door_comp_timer_ms_ = 0;
            if (door_comp_blocked_) {
                door_comp_blocked_ = false;
                ESP_LOGI(TAG, "Door compressor block cleared");
            }
        }
        state_set("protection.door_comp_blocked", door_comp_blocked_);
    }

    // 6. Condenser temperature protection
    if (read_bool("equipment.has_cond_temp")) {
        float cond_temp = read_float("equipment.cond_temp");
        update_condenser_alarm(cond_temp, true, dt_ms);
    }

    // 7. Компресорний захист
    if (has_feature("compressor_protection")) {
        bool compressor_on = read_bool("equipment.compressor");
        update_compressor_tracker(compressor_on, air_temp, defrost, dt_ms);

        // Rate-of-change монітор
        if (has_feature("rate_protection")) {
            if (compressor_on && !suppress_high) {
                update_rate_tracker(air_temp, dt_ms);
            } else {
                rate_.rising_duration_ms = 0;
                rate_.ewma_rate = 0.0f;
                if (!compressor_on) rate_.initialized = false;
                // Auto-clear rate alarm коли компресор вимкнено або defrost
                if (rate_rise_.active && !manual_reset_) {
                    rate_rise_.active = false;
                    ESP_LOGI(TAG, "Rate alarm cleared (compressor off/defrost)");
                }
            }
        }

        // Діагностика кожні 5 секунд
        diag_timer_ += dt_ms;
        if (diag_timer_ >= 5000) {
            diag_timer_ = 0;
            publish_compressor_diagnostics();
        }
    }

    // 6. Публікуємо стан аварій
    publish_alarms();
}

// ═══════════════════════════════════════════════════════════════
// High Temp alarm з dAd затримкою і defrost blocking
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::update_high_temp(float temp, bool sensor_ok,
                                         bool defrost_active, uint32_t dt_ms) {
    // Блокується під час defrost (spec_v3 §1.3)
    if (defrost_active) {
        high_temp_.pending = false;
        high_temp_.pending_ms = 0;
        // НЕ скидаємо active якщо вже в аварії — тільки pending
        return;
    }

    // Не можемо перевірити без датчика
    if (!sensor_ok) {
        high_temp_.pending = false;
        high_temp_.pending_ms = 0;
        return;
    }

    if (temp > high_limit_) {
        // Вище межі
        if (!high_temp_.active) {
            high_temp_.pending = true;
            high_temp_.pending_ms += dt_ms;
            if (high_temp_.pending_ms >= high_alarm_delay_ms_) {
                high_temp_.active = true;
                high_temp_.pending = false;
                ESP_LOGW(TAG, "HIGH TEMP ALARM: %.1f > %.1f (delay %lu min)",
                         temp, high_limit_, high_alarm_delay_ms_ / 60000);
            }
        }
    } else {
        // Повернувся в норму
        high_temp_.pending = false;
        high_temp_.pending_ms = 0;
        if (high_temp_.active && !manual_reset_) {
            high_temp_.active = false;
            ESP_LOGI(TAG, "High temp alarm cleared (auto)");
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Low Temp alarm з dAd затримкою (завжди активний)
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::update_low_temp(float temp, bool sensor_ok, uint32_t dt_ms) {
    // Не можемо перевірити без датчика
    if (!sensor_ok) {
        low_temp_.pending = false;
        low_temp_.pending_ms = 0;
        return;
    }

    if (temp < low_limit_) {
        // Нижче межі
        if (!low_temp_.active) {
            low_temp_.pending = true;
            low_temp_.pending_ms += dt_ms;
            if (low_temp_.pending_ms >= low_alarm_delay_ms_) {
                low_temp_.active = true;
                low_temp_.pending = false;
                ESP_LOGW(TAG, "LOW TEMP ALARM: %.1f < %.1f (delay %lu min)",
                         temp, low_limit_, low_alarm_delay_ms_ / 60000);
            }
        }
    } else {
        // Повернувся в норму
        low_temp_.pending = false;
        low_temp_.pending_ms = 0;
        if (low_temp_.active && !manual_reset_) {
            low_temp_.active = false;
            ESP_LOGI(TAG, "Low temp alarm cleared (auto)");
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Sensor alarm — instant (без затримки)
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::update_sensor_alarm(AlarmMonitor& m, bool sensor_ok,
                                            const char* label) {
    if (!sensor_ok) {
        if (!m.active) {
            m.active = true;
            ESP_LOGW(TAG, "%s ALARM — sensor failure", label);
        }
    } else {
        if (m.active && !manual_reset_) {
            m.active = false;
            ESP_LOGI(TAG, "%s alarm cleared (auto)", label);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Door alarm з затримкою
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::update_door_alarm(bool door_open, uint32_t dt_ms) {
    if (door_open) {
        if (!door_.active) {
            door_.pending = true;
            door_.pending_ms += dt_ms;
            if (door_.pending_ms >= door_delay_ms_) {
                door_.active = true;
                door_.pending = false;
                ESP_LOGW(TAG, "DOOR ALARM — open > %lu min", door_delay_ms_ / 60000);
            }
        }
    } else {
        // Двері закриті — скидаємо
        door_.pending = false;
        door_.pending_ms = 0;
        if (door_.active && !manual_reset_) {
            door_.active = false;
            ESP_LOGI(TAG, "Door alarm cleared (auto)");
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Компресорний трекер — відстеження циклів, duty, starts
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::update_compressor_tracker(bool compressor_on, float temp,
                                                   bool defrost_active, uint32_t dt_ms) {
    // --- Ковзне вікно 1 годину ---
    comp_.window_ms += dt_ms;
    if (comp_.window_ms > 3600000) {
        // Обрізаємо total_on пропорційно до вікна
        uint32_t overflow = comp_.window_ms - 3600000;
        if (comp_.total_on_1h_ms > overflow) {
            comp_.total_on_1h_ms -= overflow;
        } else {
            comp_.total_on_1h_ms = 0;
        }
        comp_.window_ms = 3600000;
    }

    // --- Переходи стану ---
    if (compressor_on && !comp_.prev_state) {
        // OFF → ON: новий запуск
        comp_.current_run_ms = 0;
        comp_.last_off_ms = comp_.current_off_ms;
        comp_.temp_at_start = temp;

        // Записуємо evap baseline для pulldown (matched comparison)
        auto evap_start = state_get("equipment.evap_temp");
        if (evap_start.has_value()) {
            const auto* fp = etl::get_if<float>(&evap_start.value());
            comp_.evap_at_start = fp ? *fp : temp;
        } else {
            comp_.evap_at_start = temp;  // fallback to air
        }

        // Записуємо timestamp в ring buffer
        comp_.start_timestamps[comp_.start_head] = comp_.window_ms;
        comp_.start_head = (comp_.start_head + 1) % CompressorTracker::MAX_STARTS;
        if (comp_.start_count < CompressorTracker::MAX_STARTS) {
            comp_.start_count++;
        }
    }
    else if (!compressor_on && comp_.prev_state) {
        // ON → OFF: кінець циклу
        comp_.last_run_ms = comp_.current_run_ms;
        comp_.current_off_ms = 0;

        // Перевірка короткого циклу
        if (comp_.current_run_ms < min_compressor_run_ms_) {
            comp_.short_cycle_count++;
            ESP_LOGW(TAG, "Short cycle detected: %lu sec (min %lu), count=%u",
                     comp_.current_run_ms / 1000, min_compressor_run_ms_ / 1000,
                     comp_.short_cycle_count);
        } else {
            // Нормальний цикл — скидаємо лічильник
            comp_.short_cycle_count = 0;
        }

    }
    comp_.prev_state = compressor_on;

    // --- Акумуляція часу ---
    if (compressor_on) {
        comp_.current_run_ms += dt_ms;
        comp_.total_on_1h_ms += dt_ms;
    } else {
        comp_.current_off_ms += dt_ms;
    }

    // Скидання short cycle counter після тривалого простою (10× min_compressor_run)
    if (!compressor_on && comp_.short_cycle_count > 0) {
        if (comp_.current_off_ms > min_compressor_run_ms_ * 10) {
            comp_.short_cycle_count = 0;
            ESP_LOGD(TAG, "Short cycle counter reset (idle > %lu sec)",
                     (min_compressor_run_ms_ * 10) / 1000);
        }
    }

    // --- Аварії ---

    // 6. Short Cycling: 3 послідовних коротких цикли
    if (comp_.short_cycle_count >= 3) {
        if (!short_cycle_.active) {
            short_cycle_.active = true;
            ESP_LOGW(TAG, "SHORT CYCLE ALARM: %u consecutive short cycles", comp_.short_cycle_count);
        }
    } else {
        if (short_cycle_.active && !manual_reset_) {
            short_cycle_.active = false;
            ESP_LOGI(TAG, "Short cycle alarm cleared (auto)");
        }
    }

    // 7. Rapid Cycling: забагато запусків за годину
    int starts_1h = count_starts_in_window(3600000);
    if (starts_1h > max_starts_hour_) {
        if (!rapid_cycle_.active) {
            rapid_cycle_.active = true;
            ESP_LOGW(TAG, "RAPID CYCLE ALARM: %d starts/hr > %d", starts_1h, max_starts_hour_);
        }
    } else {
        if (rapid_cycle_.active && !manual_reset_) {
            rapid_cycle_.active = false;
            ESP_LOGI(TAG, "Rapid cycle alarm cleared (auto)");
        }
    }

    // 8. Continuous Run з ескалацією: forced off → defrost → retry → lockout

    // Guard: не рахуємо continuous run під час defrost (hot gas = нормальна робота)
    if (!defrost_active) {

    // Forced off таймер: компресор заблоковано, вентилятори працюють
    if (forced_off_active_) {
        forced_off_timer_ms_ += dt_ms;
        if (forced_off_timer_ms_ >= forced_off_period_ms_) {
            // Forced off завершився
            forced_off_active_ = false;
            continuous_run_count_++;

            if (continuous_run_count_ >= static_cast<uint8_t>(max_retries_)) {
                // Рівень 2: перманентна блокіровка
                permanent_lockout_ = true;
                ESP_LOGW(TAG, "CONTINUOUS RUN LOCKOUT: %u retries — permanent lockout",
                         continuous_run_count_);
            } else {
                ESP_LOGI(TAG, "Forced off ended (count=%u/%ld) — releasing",
                         continuous_run_count_, max_retries_);

                // Перевірка обмерзання: якщо T_evap < demand_temp → тригер відтайки
                if (read_bool("equipment.has_evap_temp")) {
                    int defrost_type = read_int("defrost.type", 0);
                    if (defrost_type != 0) {
                        float evap_temp = read_float("equipment.evap_temp");
                        float demand_temp = read_float("defrost.demand_temp", -15.0f);
                        if (evap_temp < demand_temp) {
                            state_set("defrost.manual_start", true);
                            ESP_LOGI(TAG, "Icing suspected (evap=%.1f < demand=%.1f) — defrost triggered",
                                     evap_temp, demand_temp);
                        }
                    }
                }
            }

            // Скидаємо для свіжого відліку
            comp_.current_run_ms = 0;
            continuous_run_.active = false;  // Дозволяємо повторний тригер

            state_set("protection.continuous_run_count",
                       static_cast<int32_t>(continuous_run_count_));
        }
    } else if (permanent_lockout_) {
        // Перманентна блокіровка — нічого не робимо (lockout тримається в publish_alarms)
    } else if (compressor_on && comp_.current_run_ms > max_continuous_run_ms_) {
        // Рівень 1: тригер forced off
        if (!continuous_run_.active) {
            continuous_run_.active = true;
            forced_off_active_ = true;
            forced_off_timer_ms_ = 0;
            ESP_LOGW(TAG, "CONTINUOUS RUN ALARM: %lu min > %lu min — forced off (%lu min)",
                     comp_.current_run_ms / 60000, max_continuous_run_ms_ / 60000,
                     forced_off_period_ms_ / 60000);
        }
    } else if (!compressor_on && !forced_off_active_) {
        // Нормальне вимкнення компресора
        if (continuous_run_.active && !manual_reset_) {
            continuous_run_.active = false;
            ESP_LOGI(TAG, "Continuous run alarm cleared (auto)");
        }
        // Нормальний цикл завершився — скидаємо лічильник
        if (comp_.last_run_ms > 0 && comp_.last_run_ms < max_continuous_run_ms_) {
            if (continuous_run_count_ > 0) {
                continuous_run_count_ = 0;
                state_set("protection.continuous_run_count", static_cast<int32_t>(0));
                ESP_LOGI(TAG, "Continuous run counter reset (normal cycle)");
            }
        }
    }

    } // end !defrost_active guard

    // 9. Pulldown Failure: компресор працює, а температура не падає
    if (compressor_on && !defrost_active &&
        comp_.current_run_ms > pulldown_timeout_ms_) {
        // Matched baseline: evap vs evap, або air vs air
        float start_temp = comp_.temp_at_start;   // air_temp при старті
        float current_temp = temp;                 // air_temp зараз
        auto evap_val = state_get("equipment.evap_temp");
        if (evap_val.has_value()) {
            const auto* fp = etl::get_if<float>(&evap_val.value());
            if (fp) {
                start_temp = comp_.evap_at_start;  // evap при старті
                current_temp = *fp;                 // evap зараз
            }
        }

        float temp_drop = start_temp - current_temp;
        if (temp_drop < pulldown_min_drop_) {
            if (!pulldown_.active) {
                pulldown_.active = true;
                ESP_LOGW(TAG, "PULLDOWN ALARM: running %lu min, drop=%.1f < %.1f",
                         comp_.current_run_ms / 60000, temp_drop, pulldown_min_drop_);
            }
        } else {
            // Температура впала достатньо
            if (pulldown_.active && !manual_reset_) {
                pulldown_.active = false;
                ESP_LOGI(TAG, "Pulldown alarm cleared (temp dropped %.1f)", temp_drop);
            }
        }
    } else if (!compressor_on) {
        // Компресор вимкнений — скидаємо pulldown
        if (pulldown_.active && !manual_reset_) {
            pulldown_.active = false;
            ESP_LOGI(TAG, "Pulldown alarm cleared (compressor off)");
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Підрахунок запусків у вікні
// ═══════════════════════════════════════════════════════════════

int ProtectionModule::count_starts_in_window(uint32_t window_ms) const {
    if (comp_.start_count == 0) return 0;

    uint32_t cutoff = 0;
    if (comp_.window_ms > window_ms) {
        cutoff = comp_.window_ms - window_ms;
    }

    int count = 0;
    // Ітеруємо ring buffer від найстарішого до найновішого
    uint8_t idx = (comp_.start_head + CompressorTracker::MAX_STARTS - comp_.start_count)
                  % CompressorTracker::MAX_STARTS;
    for (uint8_t i = 0; i < comp_.start_count; i++) {
        if (comp_.start_timestamps[idx] >= cutoff) {
            count++;
        }
        idx = (idx + 1) % CompressorTracker::MAX_STARTS;
    }
    return count;
}

// ═══════════════════════════════════════════════════════════════
// Rate-of-Change трекер (EWMA, lambda=0.3)
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::update_rate_tracker(float temp, uint32_t dt_ms) {
    if (!rate_.initialized) {
        rate_.prev_temp = temp;
        rate_.initialized = true;
        return;
    }

    float dt_min = static_cast<float>(dt_ms) / 60000.0f;
    if (dt_min < 0.001f) return;  // захист від ділення на нуль

    float instant_rate = (temp - rate_.prev_temp) / dt_min;
    rate_.prev_temp = temp;

    // EWMA згладжування
    constexpr float LAMBDA = 0.3f;
    rate_.ewma_rate = LAMBDA * instant_rate + (1.0f - LAMBDA) * rate_.ewma_rate;

    if (rate_.ewma_rate > max_rise_rate_) {
        rate_.rising_duration_ms += dt_ms;
        if (rate_.rising_duration_ms >= rate_duration_ms_) {
            if (!rate_rise_.active) {
                rate_rise_.active = true;
                ESP_LOGW(TAG, "RATE ALARM: %.2f C/min > %.2f for %lu min",
                         rate_.ewma_rate, max_rise_rate_,
                         rate_.rising_duration_ms / 60000);
            }
        }
    } else {
        rate_.rising_duration_ms = 0;
        if (rate_rise_.active && !manual_reset_) {
            rate_rise_.active = false;
            ESP_LOGI(TAG, "Rate alarm cleared (auto)");
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Публікація діагностики компресора (кожні 5 сек)
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::publish_compressor_diagnostics() {
    int32_t starts = count_starts_in_window(3600000);
    float duty = (comp_.window_ms > 0)
                 ? (static_cast<float>(comp_.total_on_1h_ms) * 100.0f
                    / static_cast<float>(comp_.window_ms))
                 : 0.0f;
    int32_t run_sec  = static_cast<int32_t>(comp_.current_run_ms / 1000);
    int32_t last_run = static_cast<int32_t>(comp_.last_run_ms / 1000);
    int32_t last_off = static_cast<int32_t>(comp_.last_off_ms / 1000);

    // Мотогодини: інкремент за 5 секунд (якщо компресор ON)
    if (read_bool("equipment.compressor")) {
        compressor_hours_ += 5.0f / 3600.0f;
    }

    // track_change=false — діагностика не тригерить WS broadcasts
    state_set("protection.compressor_starts_1h", starts, false);
    state_set("protection.compressor_duty", duty, false);
    state_set("protection.compressor_run_time", run_sec, false);
    state_set("protection.last_cycle_run", last_run, false);
    state_set("protection.last_cycle_off", last_off, false);

    // compressor_hours — persist раз на 1 год (720 × 5с = 3600с)
    // state_set() тригерить persist callback → NVS write, тому викликаємо РІДКО
    hours_persist_counter_++;
    if (hours_persist_counter_ >= 720) {
        hours_persist_counter_ = 0;
        state_set("protection.compressor_hours", compressor_hours_);
    }
}

// ═══════════════════════════════════════════════════════════════
// Команда скидання аварій (manual reset через WebUI/API)
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::check_reset_command() {
    if (read_bool("protection.reset_alarms")) {
        // Скидаємо всі активні аварії
        bool any = high_temp_.active || low_temp_.active || sensor1_.active ||
                   sensor2_.active || door_.active ||
                   condenser_.active || condenser_block_.active ||
                   short_cycle_.active || rapid_cycle_.active ||
                   continuous_run_.active || pulldown_.active || rate_rise_.active ||
                   permanent_lockout_ || forced_off_active_;

        if (any) {
            high_temp_.active = false;
            high_temp_.pending = false;
            high_temp_.pending_ms = 0;
            low_temp_.active = false;
            low_temp_.pending = false;
            low_temp_.pending_ms = 0;
            sensor1_.active = false;
            sensor2_.active = false;
            door_.active = false;
            door_.pending = false;
            door_.pending_ms = 0;
            condenser_.active = false;
            condenser_block_.active = false;

            // Скидаємо alarm flags
            short_cycle_.active = false;
            rapid_cycle_.active = false;
            continuous_run_.active = false;
            pulldown_.active = false;
            rate_rise_.active = false;

            // Скидаємо tracker state що спричинив аварії
            // (без цього аварія спрацює повторно в наступному циклі)
            comp_.current_run_ms = 0;
            comp_.short_cycle_count = 0;
            memset(comp_.start_timestamps, 0, sizeof(comp_.start_timestamps));
            comp_.start_head = 0;
            comp_.start_count = 0;
            comp_.total_on_1h_ms = 0;
            comp_.window_ms = 0;
            rate_.rising_duration_ms = 0;
            rate_.ewma_rate = 0.0f;
            rate_.initialized = false;

            // Скидаємо ескалацію continuous run
            continuous_run_count_ = 0;
            forced_off_active_ = false;
            forced_off_timer_ms_ = 0;
            permanent_lockout_ = false;
            state_set("protection.continuous_run_count", static_cast<int32_t>(0));

            // Негайно публікуємо зняття блокіровки
            state_set("protection.compressor_blocked", false);
            state_set("protection.lockout", false);

            ESP_LOGI(TAG, "All alarms reset — tracker state cleared");
        }
        // Скидаємо trigger
        state_set("protection.reset_alarms", false);
    }
}

// ═══════════════════════════════════════════════════════════════
// Публікація стану аварій
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::publish_alarms() {
    // Ескалація: compressor block (рівень 1) + lockout (рівень 2)
    state_set("protection.compressor_blocked", forced_off_active_);
    state_set("protection.lockout", permanent_lockout_);

    // Окремі алерти — існуючі
    state_set("protection.high_temp_alarm", high_temp_.active);
    state_set("protection.low_temp_alarm", low_temp_.active);
    state_set("protection.sensor1_alarm", sensor1_.active);
    state_set("protection.sensor2_alarm", sensor2_.active);
    state_set("protection.door_alarm", door_.active);

    // Окремі алерти — нові
    state_set("protection.short_cycle_alarm", short_cycle_.active);
    state_set("protection.rapid_cycle_alarm", rapid_cycle_.active);
    state_set("protection.continuous_run_alarm", continuous_run_.active);
    state_set("protection.pulldown_alarm", pulldown_.active);
    state_set("protection.rate_alarm", rate_rise_.active);
    state_set("protection.condenser_alarm", condenser_.active);
    state_set("protection.condenser_block", condenser_block_.active);

    // Зведений статус (включає ескалацію)
    bool any_alarm = high_temp_.active || low_temp_.active ||
                     sensor1_.active || sensor2_.active || door_.active ||
                     short_cycle_.active || rapid_cycle_.active ||
                     continuous_run_.active || pulldown_.active || rate_rise_.active ||
                     condenser_.active || condenser_block_.active ||
                     permanent_lockout_ || forced_off_active_;
    state_set("protection.alarm_active", any_alarm);

    // Код найвищої за пріоритетом аварії
    // lockout > comp_blocked > err1 > rate_rise > high_temp > pulldown >
    // short_cycle > rapid_cycle > low_temp > continuous_run > err2 > door > none
    if (permanent_lockout_) {
        alarm_code_ = "lockout";
    } else if (forced_off_active_) {
        alarm_code_ = "comp_blocked";
    } else if (condenser_block_.active) {
        alarm_code_ = "cond_block";
    } else if (sensor1_.active) {
        alarm_code_ = "err1";
    } else if (condenser_.active) {
        alarm_code_ = "condenser";
    } else if (rate_rise_.active) {
        alarm_code_ = "rate_rise";
    } else if (high_temp_.active) {
        alarm_code_ = "high_temp";
    } else if (pulldown_.active) {
        alarm_code_ = "pulldown";
    } else if (short_cycle_.active) {
        alarm_code_ = "short_cycle";
    } else if (rapid_cycle_.active) {
        alarm_code_ = "rapid_cycle";
    } else if (low_temp_.active) {
        alarm_code_ = "low_temp";
    } else if (continuous_run_.active) {
        alarm_code_ = "continuous_run";
    } else if (sensor2_.active) {
        alarm_code_ = "err2";
    } else if (door_.active) {
        alarm_code_ = "door";
    } else {
        alarm_code_ = "none";
    }
    state_set("protection.alarm_code", alarm_code_);
}

// ═══════════════════════════════════════════════════════════════
// Condenser temperature protection (like Danfoss A61/A80)
// Two levels: alarm (warning) + block (compressor OFF, manual reset)
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::update_condenser_alarm(float cond_temp, bool has_cond,
                                               uint32_t dt_ms) {
    if (!has_cond || std::isnan(cond_temp)) {
        condenser_.active = false;
        condenser_block_.active = false;
        return;
    }

    // Level 1: alarm — condenser temp too high (check airflow)
    if (cond_temp > condenser_alarm_limit_) {
        if (!condenser_.active) {
            condenser_.active = true;
            ESP_LOGW(TAG, "CONDENSER ALARM — temp %.1f°C > %.1f°C limit",
                     cond_temp, condenser_alarm_limit_);
        }
    } else {
        if (condenser_.active && !manual_reset_) {
            condenser_.active = false;
            ESP_LOGI(TAG, "Condenser alarm cleared (temp %.1f°C)", cond_temp);
        }
    }

    // Level 2: block — condenser temp critical (manual reset required)
    if (cond_temp > condenser_block_limit_) {
        if (!condenser_block_.active) {
            condenser_block_.active = true;
            ESP_LOGW(TAG, "CONDENSER BLOCK — temp %.1f°C > %.1f°C, compressor OFF!",
                     cond_temp, condenser_block_limit_);
        }
    }
    // Block requires manual reset — never auto-clears
    // (like Danfoss A80: reset by r12 OFF→ON or power cycle)
}

// ═══════════════════════════════════════════════════════════════
// Stop
// ═══════════════════════════════════════════════════════════════

void ProtectionModule::on_stop() {
    state_set("protection.lockout", false);
    state_set("protection.compressor_blocked", false);
    state_set("protection.alarm_active", false);
    state_set("protection.alarm_code", "none");
    ESP_LOGI(TAG, "Protection stopped");
}
