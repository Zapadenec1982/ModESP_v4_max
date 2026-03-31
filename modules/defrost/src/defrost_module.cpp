/**
 * @file defrost_module.cpp
 * @brief Defrost cycle — 7-phase state machine (spec_v3 §3)
 *
 * State machine:
 *   IDLE → [dFT=2: STABILIZE → VALVE_OPEN →] ACTIVE
 *        → [dFT=2: EQUALIZE →] DRIP → FAD → IDLE
 *
 * Relay states per phase — see enter_phase() comments.
 * Equipment Manager applies arbitration and interlocks.
 */

#include "defrost_module.h"
#include "esp_log.h"

static const char TAG[] = "Defrost";

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

DefrostModule::DefrostModule()
    : BaseModule("defrost", modesp::ModulePriority::NORMAL)
{}

DefrostModule::DefrostModule(const char* ns, etl::span<const modesp::InputBinding> inputs)
    : BaseModule("defrost", ns, modesp::ModulePriority::NORMAL, inputs)
{}

// ═══════════════════════════════════════════════════════════════
// Sync settings з SharedState
// ═══════════════════════════════════════════════════════════════

void DefrostModule::sync_settings() {
    defrost_type_  = read_int(ns_key("type"), 0);
    counter_mode_  = read_int(ns_key("counter_mode"), 1);
    initiation_    = read_int(ns_key("initiation"), 0);
    termination_   = read_int(ns_key("termination"), 0);
    end_temp_      = read_float(ns_key("end_temp"), end_temp_);
    demand_temp_   = read_float(ns_key("demand_temp"), demand_temp_);
    fad_temp_      = read_float(ns_key("fad_temp"), fad_temp_);

    // Години → мілісекунди
    interval_ms_ = static_cast<uint32_t>(read_int(ns_key("interval"), 8)) * 3600000;

    // Хвилини → мілісекунди
    max_duration_ms_ = static_cast<uint32_t>(read_int(ns_key("max_duration"), 30)) * 60000;

    // Хвилини → мілісекунди
    drip_time_ms_   = static_cast<uint32_t>(read_int(ns_key("drip_time"), 2)) * 60000;
    fan_delay_ms_   = static_cast<uint32_t>(read_int(ns_key("fan_delay"), 2)) * 60000;
    stabilize_ms_   = static_cast<uint32_t>(read_int(ns_key("stabilize_time"), 1)) * 60000;
    equalize_ms_    = static_cast<uint32_t>(read_int(ns_key("equalize_time"), 2)) * 60000;

    // Секунди → мілісекунди
    valve_delay_ms_ = static_cast<uint32_t>(read_int(ns_key("valve_delay"), 3)) * 1000;

    // Staggered Defrost (MPXPRO d1S): auto N defrosts/day → override interval
    int32_t defrosts_per_day = read_int(ns_key("defrosts_per_day"), 0);
    if (defrosts_per_day > 0 && defrosts_per_day <= 14) {
        // 24h / N = interval in ms
        interval_ms_ = 86400000 / static_cast<uint32_t>(defrosts_per_day);
    }

    // Pump Down (MPXPRO dH1) — секунди → мілісекунди
    pump_down_time_ms_ = static_cast<uint32_t>(read_int(ns_key("pump_down_time"), 0)) * 1000;

    // Running Time defrost (MPXPRO d10/d11) — хвилини → мілісекунди
    running_time_ms_   = static_cast<uint32_t>(read_int(ns_key("running_time"), 0)) * 60000;
    running_time_temp_ = read_float(ns_key("running_time_temp"), -30.0f);

    // Defrost by ΔT (MPXPRO dd1/dd2/dTd/tdd)
    delta_t_threshold_ = read_float(ns_key("delta_t_threshold"), 10.0f);
    delta_t_time_ms_   = static_cast<uint32_t>(read_int(ns_key("delta_t_time"), 60)) * 60000;

    // Skip Defrost (MPXPRO d7/dn)
    skip_enabled_       = read_bool(ns_key("skip_enabled"), false);
    skip_threshold_pct_ = read_int(ns_key("skip_threshold"), 75);

    // Power Defrost (MPXPRO ddt/ddP)
    power_end_temp_delta_  = read_float(ns_key("power_end_temp_delta"), 0.0f);
    power_duration_delta_  = read_int(ns_key("power_duration_delta"), 0);

    // Early termination (MPXPRO dEP/dET)
    early_term_enabled_ = read_bool(ns_key("early_term_enabled"), false);
    early_term_temp_    = read_float(ns_key("early_term_temp"), 12.0f);
}

// ═══════════════════════════════════════════════════════════════
// Phase name
// ═══════════════════════════════════════════════════════════════

const char* DefrostModule::phase_name() const {
    switch (phase_) {
        case Phase::IDLE:       return "idle";
        case Phase::PUMP_DOWN:  return "pump_down";
        case Phase::STABILIZE:  return "stabilize";
        case Phase::VALVE_OPEN: return "valve_open";
        case Phase::ACTIVE:     return "active";
        case Phase::EQUALIZE:   return "equalize";
        case Phase::DRIP:       return "drip";
        case Phase::FAD:        return "fad";
    }
    return "unknown";
}

// ═══════════════════════════════════════════════════════════════
// Requests до Equipment Manager
// ═══════════════════════════════════════════════════════════════

void DefrostModule::set_requests(bool comp, bool relay, bool evap_fan,
                                  bool cond_fan) {
    if (req_comp_ != comp) {
        req_comp_ = comp;
        state_set(ns_key("req.compressor"), comp);
    }
    if (req_relay_ != relay) {
        req_relay_ = relay;
        state_set(ns_key("req.defrost_relay"), relay);
    }
    if (req_evap_ != evap_fan) {
        req_evap_ = evap_fan;
        state_set(ns_key("req.evap_fan"), evap_fan);
    }
    if (req_cond_ != cond_fan) {
        req_cond_ = cond_fan;
        state_set(ns_key("req.cond_fan"), cond_fan);
    }
}

void DefrostModule::clear_requests() {
    set_requests(false, false, false, false);
}

// ═══════════════════════════════════════════════════════════════
// Enter phase — встановлення relay requests для кожної фази
// ═══════════════════════════════════════════════════════════════

void DefrostModule::enter_phase(Phase p) {
    if (phase_ == p) return;  // BUG-006: re-entry guard
    phase_ = p;
    phase_timer_ms_ = 0;

    state_set(ns_key("active"), p != Phase::IDLE);
    state_set(ns_key("phase"), phase_name());

    switch (p) {
        case Phase::IDLE:
            clear_requests();
            state_set(ns_key("state"), "idle");
            break;

        case Phase::PUMP_DOWN:
            // Pump Down: comp ON, EEV/solenoid closed, fan OFF — спорожнення випарника
            set_requests(true, false, false, false);
            // Signal EEV to close (0%)
            state_set(ns_key("req.eev_close"), true);
            state_set(ns_key("state"), "pump_down");
            ESP_LOGI(TAG, "Pump down started (%lu s)", pump_down_time_ms_ / 1000);
            break;

        case Phase::STABILIZE:
            // dFT=2, Фаза 1: компресор ON + конд.вент ON, реле OFF
            // Тиск стабілізується перед відкриттям клапана ГГ
            set_requests(true, false, false, true);
            // Pump down завершено — дозволяємо EEV
            state_set(ns_key("req.eev_close"), false);
            state_set(ns_key("state"), "stabilize");
            break;

        case Phase::VALVE_OPEN:
            // dFT=2, Фаза 2: компресор ON + реле ON (клапан ГГ) + конд.вент ON
            set_requests(true, true, false, true);
            state_set(ns_key("state"), "valve_open");
            break;

        case Phase::ACTIVE:
            // Використовуємо active_defrost_type_ — кешований при старті циклу (BUG-002 fix)
            if (active_defrost_type_ == 0) {
                // Природна: все OFF (компресор зупиняється)
                set_requests(false, false, false, false);
                state_set(ns_key("state"), "defrost_natural");
            } else if (active_defrost_type_ == 1) {
                // Електричний тен: реле ON, компресор OFF
                // EM інтерлок заблокує компресор при type=1
                set_requests(false, true, false, false);
                state_set(ns_key("state"), "defrost_heater");
            } else {
                // ГГ: компресор ON + реле ON (клапан ГГ)
                set_requests(true, true, false, false);
                state_set(ns_key("state"), "defrost_hotgas");
            }
            break;

        case Phase::EQUALIZE:
            // dFT=2, Фаза 4: все OFF, тиск падає
            set_requests(false, false, false, false);
            state_set(ns_key("state"), "equalize");
            break;

        case Phase::DRIP:
            // Все OFF, вода стікає
            set_requests(false, false, false, false);
            state_set(ns_key("state"), "drip");
            break;

        case Phase::FAD:
            // Компресор ON, конд. вент. ON, реле OFF
            set_requests(true, false, false, true);
            state_set(ns_key("state"), "fad");
            break;
    }

    ESP_LOGI(TAG, "Phase → %s", phase_name());
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_init
// ═══════════════════════════════════════════════════════════════

bool DefrostModule::on_init() {
    sync_settings();

    // Відновлюємо persist лічильники з SharedState (заповнені PersistService)
    interval_timer_ms_ = static_cast<uint32_t>(read_int(ns_key("interval_timer"), 0)) * 1000;
    defrost_count_ = read_int(ns_key("defrost_count"), 0);

    // Початковий стан
    state_set(ns_key("active"), false);
    state_set(ns_key("phase"), "idle");
    state_set(ns_key("state"), "idle");
    state_set(ns_key("phase_timer"), static_cast<int32_t>(0));
    state_set(ns_key("interval_timer"), static_cast<int32_t>(interval_timer_ms_ / 1000));
    state_set(ns_key("defrost_count"), defrost_count_);
    state_set(ns_key("last_termination"), "none");
    state_set(ns_key("consecutive_timeouts"), static_cast<int32_t>(0));
    state_set(ns_key("heater_alarm"), false);
    state_set(ns_key("manual_start"), false);
    state_set(ns_key("manual_stop"), false);
    state_set(ns_key("req.compressor"), false);
    state_set(ns_key("req.defrost_relay"), false);
    state_set(ns_key("req.evap_fan"), false);
    state_set(ns_key("req.cond_fan"), false);

    const char* type_name = defrost_type_ == 0 ? "natural" :
                            defrost_type_ == 1 ? "heater" : "hotgas";
    const char* init_name = initiation_ == 0 ? "timer" :
                            initiation_ == 1 ? "demand" :
                            initiation_ == 2 ? "combo" : "disabled";
    const char* term_name = termination_ == 0 ? "temp" : "timer";
    ESP_LOGI(TAG, "Initialized (type=%s, interval=%ldh, initiation=%s, termination=%s, counter=%s)",
             type_name,
             static_cast<long>(interval_ms_ / 3600000),
             init_name, term_name,
             counter_mode_ == 1 ? "realtime" : "compressor");
    ESP_LOGI(TAG, "  end_temp=%.1f, max_duration=%ldmin, defrost_count=%ld",
             end_temp_,
             static_cast<long>(max_duration_ms_ / 60000),
             static_cast<long>(defrost_count_));
    ESP_LOGI(TAG, "Features: by_sensor=%d, electric=%d, hot_gas=%d",
             has_feature("defrost_by_sensor"),
             has_feature("defrost_electric"),
             has_feature("defrost_hot_gas"));
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_update — головний цикл
// ═══════════════════════════════════════════════════════════════

void DefrostModule::on_update(uint32_t dt_ms) {
    sync_settings();

    // Читаємо inputs
    evap_temp_     = read_input_float("equipment.evap_temp");
    sensor2_ok_    = read_input_bool("equipment.sensor2_ok");
    compressor_on_ = read_input_bool("equipment.compressor");
    bool lockout   = read_input_bool("protection.lockout");

    // Protection lockout → abort defrost
    if (lockout && phase_ != Phase::IDLE) {
        ESP_LOGW(TAG, "Protection lockout — aborting defrost");
        finish_defrost();
        return;
    }

    // Compressor blocked → abort hot gas defrost (потрібен потік хладагенту)
    bool comp_blocked = read_input_bool("protection.compressor_blocked");
    if (comp_blocked && phase_ != Phase::IDLE && active_defrost_type_ == 2) {
        if (phase_ == Phase::STABILIZE || phase_ == Phase::VALVE_OPEN || phase_ == Phase::ACTIVE) {
            ESP_LOGW(TAG, "Compressor blocked — aborting hot gas defrost");
            finish_defrost();
            return;
        }
    }

    // Ручна зупинка розморозки
    if (read_bool(ns_key("manual_stop"))) {
        state_set(ns_key("manual_stop"), false);
        if (phase_ != Phase::IDLE) {
            ESP_LOGI(TAG, "Manual stop — aborting defrost");
            finish_defrost();
            return;
        }
    }

    // Очищуємо manual_start якщо defrost вже активний —
    // запобігає подвійному циклу від одного натиску (flag залишався
    // в SharedState до наступного IDLE → тригерив ще один defrost).
    if (phase_ != Phase::IDLE && read_bool(ns_key("manual_start"))) {
        state_set(ns_key("manual_start"), false);
    }

    // Phase dispatch
    switch (phase_) {
        case Phase::IDLE:
            update_idle(dt_ms);
            break;
        case Phase::PUMP_DOWN:
            update_pump_down(dt_ms);
            break;
        case Phase::STABILIZE:
            update_stabilize(dt_ms);
            break;
        case Phase::VALVE_OPEN:
            update_valve_open(dt_ms);
            break;
        case Phase::ACTIVE:
            update_active_phase(dt_ms);
            break;
        case Phase::EQUALIZE:
            update_equalize(dt_ms);
            break;
        case Phase::DRIP:
            update_drip(dt_ms);
            break;
        case Phase::FAD:
            update_fad(dt_ms);
            break;
    }

    publish_state();
}

// ═══════════════════════════════════════════════════════════════
// IDLE — перевірка ініціації
// ═══════════════════════════════════════════════════════════════

void DefrostModule::update_idle(uint32_t dt_ms) {
    // Ручний запуск
    if (read_bool(ns_key("manual_start"))) {
        state_set(ns_key("manual_start"), false);
        start_defrost("manual");
        return;
    }

    // Ініціація вимкнена
    if (initiation_ == 3) return;

    // Оновлюємо interval timer
    if (counter_mode_ == 1) {
        // Реальний час — завжди тікає
        interval_timer_ms_ += dt_ms;
    } else if (counter_mode_ == 2) {
        // Час компресора — тільки коли компресор ON
        if (compressor_on_) {
            interval_timer_ms_ += dt_ms;
        }
    }

    // Running Time trigger (mode 4): comp ON + T_evap < threshold → counter++
    bool running_trigger = false;
    if (initiation_ == 4 && running_time_ms_ > 0) {
        if (compressor_on_ && sensor2_ok_ && evap_temp_ < running_time_temp_) {
            running_time_counter_ms_ += dt_ms;
            if (running_time_counter_ms_ >= running_time_ms_) {
                running_trigger = true;
                running_time_counter_ms_ = 0;
            }
        } else {
            // Reset counter коли T піднімається або comp OFF
            running_time_counter_ms_ = 0;
        }
    }

    // Defrost by ΔT trigger (mode 5): air_temp - evap_temp > threshold → counter++
    bool delta_t_trigger = false;
    if (initiation_ == 5 && delta_t_time_ms_ > 0) {
        float air_temp = read_input_float("equipment.air_temp");
        bool sensor1_ok = read_input_bool("equipment.sensor1_ok");
        if (sensor1_ok && sensor2_ok_) {
            float delta = air_temp - evap_temp_;
            if (delta > delta_t_threshold_) {
                delta_t_counter_ms_ += dt_ms;
                if (delta_t_counter_ms_ >= delta_t_time_ms_) {
                    delta_t_trigger = true;
                    delta_t_counter_ms_ = 0;
                }
            } else {
                delta_t_counter_ms_ = 0;
            }
        }
    }

    // Перевіряємо triggers
    bool timer_trigger  = (initiation_ == 0 || initiation_ == 2) && check_timer_trigger();
    bool demand_trigger = (initiation_ == 1 || initiation_ == 2) && check_demand_trigger();

    if (timer_trigger || demand_trigger || running_trigger || delta_t_trigger) {
        // Оптимізація: випарник чистий → скасовуємо (тільки в temp-mode)
        if (termination_ == 0 && sensor2_ok_ && evap_temp_ > end_temp_) {
            ESP_LOGI(TAG, "Defrost skipped — evap clean (%.1f > %.1f)",
                     evap_temp_, end_temp_);
            interval_timer_ms_ = 0;
            return;
        }

        // Skip Defrost (MPXPRO algorithm): counter-based skipping
        if (skip_enabled_ && skip_remaining_ > 0) {
            skip_remaining_--;
            ESP_LOGI(TAG, "Defrost skipped by counter (%ld remaining)", static_cast<long>(skip_remaining_));
            interval_timer_ms_ = 0;
            return;
        }

        const char* reason = timer_trigger ? "timer" :
                             demand_trigger ? "demand" :
                             running_trigger ? "running_time" : "delta_t";
        start_defrost(reason);
    }
}

bool DefrostModule::check_timer_trigger() {
    return interval_timer_ms_ >= interval_ms_;
}

bool DefrostModule::check_demand_trigger() {
    // Потрібен датчик випарника
    if (!sensor2_ok_) return false;
    // Мінімальний інтервал — не запускати defrost одразу після попереднього (BUG-008 fix)
    if (interval_timer_ms_ < interval_ms_ / 4) return false;
    return evap_temp_ < demand_temp_;
}

// ═══════════════════════════════════════════════════════════════
// Start defrost
// ═══════════════════════════════════════════════════════════════

void DefrostModule::start_defrost(const char* reason) {
    // Кешуємо тип defrost на весь цикл — зміна через WebUI не впливає mid-cycle (BUG-002 fix)
    active_defrost_type_ = defrost_type_;

    // BUG-011: Валідація наявності реле відтайки для обраного типу дефросту.
    // Якщо обрано тен або ГГ але defrost_relay не сконфігурований — fallback на natural.
    if ((active_defrost_type_ == 1 || active_defrost_type_ == 2)
        && !read_input_bool("equipment.has_defrost_relay")) {
        ESP_LOGW(TAG, "Defrost type %ld selected but defrost_relay NOT configured — fallback to NATURAL",
                 static_cast<long>(active_defrost_type_));
        active_defrost_type_ = 0;
    }

    // Power Defrost: підсилений дефрост вночі (MPXPRO ddt/ddP)
    bool night_active = read_input_bool("thermostat.night_active");
    float effective_end_temp = end_temp_;
    uint32_t effective_max_duration = max_duration_ms_;
    if (night_active && (power_end_temp_delta_ != 0.0f || power_duration_delta_ != 0)) {
        effective_end_temp += power_end_temp_delta_;
        effective_max_duration += static_cast<uint32_t>(power_duration_delta_) * 60000;
        ESP_LOGI(TAG, "Power Defrost (night): end_temp %.1f→%.1f°C, max_dur %lu→%lu min",
                 end_temp_, effective_end_temp,
                 max_duration_ms_ / 60000, effective_max_duration / 60000);
    }

    // Зберігаємо effective values для цього циклу
    effective_end_temp_   = effective_end_temp;
    effective_max_dur_ms_ = effective_max_duration;

    ESP_LOGI(TAG, "Starting defrost (%s), type=%ld", reason, static_cast<long>(active_defrost_type_));
    interval_timer_ms_ = 0;

    // Pump Down перед defrost (якщо enabled)
    if (pump_down_time_ms_ > 0) {
        enter_phase(Phase::PUMP_DOWN);
    } else if (active_defrost_type_ == 2) {
        // Гарячий газ → починаємо зі стабілізації
        enter_phase(Phase::STABILIZE);
    } else {
        // Природна або тен → одразу active
        enter_phase(Phase::ACTIVE);
    }
}

// ═══════════════════════════════════════════════════════════════
// Pump Down — спорожнення випарника (MPXPRO dH1)
// ═══════════════════════════════════════════════════════════════

void DefrostModule::update_pump_down(uint32_t dt_ms) {
    phase_timer_ms_ += dt_ms;
    if (phase_timer_ms_ >= pump_down_time_ms_) {
        ESP_LOGI(TAG, "Pump down complete (%lu s)", pump_down_time_ms_ / 1000);
        if (active_defrost_type_ == 2) {
            enter_phase(Phase::STABILIZE);
        } else {
            enter_phase(Phase::ACTIVE);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Phase handlers: dFT=2 специфічні фази
// ═══════════════════════════════════════════════════════════════

void DefrostModule::update_stabilize(uint32_t dt_ms) {
    phase_timer_ms_ += dt_ms;
    if (phase_timer_ms_ >= stabilize_ms_) {
        enter_phase(Phase::VALVE_OPEN);
    }
}

void DefrostModule::update_valve_open(uint32_t dt_ms) {
    phase_timer_ms_ += dt_ms;
    if (phase_timer_ms_ >= valve_delay_ms_) {
        enter_phase(Phase::ACTIVE);
    }
}

void DefrostModule::update_equalize(uint32_t dt_ms) {
    phase_timer_ms_ += dt_ms;
    if (phase_timer_ms_ >= equalize_ms_) {
        enter_phase(Phase::DRIP);
    }
}

// ═══════════════════════════════════════════════════════════════
// ACTIVE — завершення по T_evap або таймеру безпеки
// ═══════════════════════════════════════════════════════════════

void DefrostModule::update_active_phase(uint32_t dt_ms) {
    phase_timer_ms_ += dt_ms;

    // Завершення по T_evap (тільки в temp-mode, після мінімального часу)
    // MIN_ACTIVE_CHECK_MS запобігає миттєвому завершенню при високій T_evap (тест/сплеск)
    if (termination_ == 0 && phase_timer_ms_ >= MIN_ACTIVE_CHECK_MS) {
        if (sensor2_ok_ && evap_temp_ >= effective_end_temp_) {
            consecutive_timeouts_ = 0;
            state_set(ns_key("heater_alarm"), false);
            state_set(ns_key("last_termination"), "temp");
            finish_active_phase("temp reached");
            return;
        }
    }

    // Early termination: дострокове завершення якщо T камери > порогу (MPXPRO dEP/dET)
    if (early_term_enabled_ && phase_timer_ms_ >= MIN_ACTIVE_CHECK_MS) {
        float cabinet_temp = read_input_float("equipment.air_temp");
        bool  sensor1_ok   = read_input_bool("equipment.sensor1_ok");
        if (sensor1_ok && cabinet_temp > early_term_temp_) {
            early_term_count_++;
            state_set(ns_key("early_term_count"), early_term_count_);
            ESP_LOGW(TAG, "Early defrost termination — cabinet %.1f°C > limit %.1f°C",
                     cabinet_temp, early_term_temp_);
            state_set(ns_key("last_termination"), "early_cabinet");
            finish_active_phase("early cabinet temp");
            return;
        }
    }

    // Завершення по таймеру (dEt) — завжди працює в обох modes
    if (phase_timer_ms_ >= effective_max_dur_ms_) {
        if (termination_ == 0) {
            // temp-mode: таймер = safety backup → лічильник таймаутів
            consecutive_timeouts_++;
            state_set(ns_key("consecutive_timeouts"), consecutive_timeouts_);
            if (consecutive_timeouts_ >= 3) {
                ESP_LOGW(TAG, "3 consecutive timeouts — possible heater/sensor failure!");
                state_set(ns_key("heater_alarm"), true);
            }
        } else {
            // timer-mode: нормальне завершення
            consecutive_timeouts_ = 0;
        }
        state_set(ns_key("last_termination"), "timeout");
        finish_active_phase(termination_ == 0 ? "timeout" : "timer");
        return;
    }
}

void DefrostModule::finish_active_phase(const char* reason) {
    defrost_count_++;
    state_set(ns_key("defrost_count"), defrost_count_);
    ESP_LOGI(TAG, "Active defrost finished (%s), count=%ld, duration=%lu s",
             reason, static_cast<long>(defrost_count_), phase_timer_ms_ / 1000);

    // Skip Defrost algorithm (MPXPRO d7/dn):
    // Перші 7 defrosts — learning (не skip). Далі:
    // Short defrost (< threshold% of max) → skip_counter++ (max 3)
    // Long defrost → skip_counter = 0 (reset)
    if (skip_enabled_ && termination_ == 0) {
        warmup_count_++;
        if (warmup_count_ > 7) {
            uint32_t threshold_ms = (max_duration_ms_ * static_cast<uint32_t>(skip_threshold_pct_)) / 100;
            if (phase_timer_ms_ < threshold_ms) {
                // Short defrost → increment counter (max 3)
                if (skip_counter_ < 3) skip_counter_++;
                skip_remaining_ = skip_counter_;
                ESP_LOGI(TAG, "Skip defrost: short (%.0f%% of max), counter=%ld, will skip %ld",
                         100.0f * phase_timer_ms_ / max_duration_ms_,
                         static_cast<long>(skip_counter_), static_cast<long>(skip_remaining_));
            } else {
                // Long defrost → reset
                skip_counter_ = 0;
                skip_remaining_ = 0;
            }
        }
    }

    // Використовуємо active_defrost_type_ — кешований при старті циклу (BUG-002 fix)
    if (active_defrost_type_ == 2) {
        // ГГ → потрібно вирівнювання тиску
        enter_phase(Phase::EQUALIZE);
    } else {
        // Природна/тен → одразу drip
        enter_phase(Phase::DRIP);
    }
}

// ═══════════════════════════════════════════════════════════════
// DRIP — дренаж
// ═══════════════════════════════════════════════════════════════

void DefrostModule::update_drip(uint32_t dt_ms) {
    phase_timer_ms_ += dt_ms;
    if (phase_timer_ms_ >= drip_time_ms_) {
        // Якщо fan_delay = 0 → пропускаємо FAD
        if (fan_delay_ms_ == 0) {
            finish_defrost();
        } else {
            enter_phase(Phase::FAD);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// FAD — Fan After Defrost
// ═══════════════════════════════════════════════════════════════

void DefrostModule::update_fad(uint32_t dt_ms) {
    phase_timer_ms_ += dt_ms;

    // Завершення по T_evap < FAT (якщо є датчик)
    if (sensor2_ok_ && evap_temp_ < fad_temp_) {
        ESP_LOGI(TAG, "FAD complete (T_evap=%.1f < FAT=%.1f)", evap_temp_, fad_temp_);
        finish_defrost();
        return;
    }

    // Завершення по таймеру FAd
    if (phase_timer_ms_ >= fan_delay_ms_) {
        ESP_LOGI(TAG, "FAD complete (timer %lu s)", fan_delay_ms_ / 1000);
        finish_defrost();
        return;
    }
}

// ═══════════════════════════════════════════════════════════════
// Finish defrost — повернення в IDLE
// ═══════════════════════════════════════════════════════════════

void DefrostModule::finish_defrost() {
    ESP_LOGI(TAG, "Defrost cycle complete");
    enter_phase(Phase::IDLE);
}

// ═══════════════════════════════════════════════════════════════
// Публікація стану
// ═══════════════════════════════════════════════════════════════

void DefrostModule::publish_state() {
    // track_change=false: таймери не тригерять WS delta broadcast
    state_set(ns_key("phase_timer"), static_cast<int32_t>(phase_timer_ms_ / 1000), false);
    // Зворотній відлік: скільки залишилось до наступної розморозки
    int32_t remaining = (interval_ms_ > interval_timer_ms_)
        ? static_cast<int32_t>((interval_ms_ - interval_timer_ms_) / 1000)
        : 0;
    state_set(ns_key("interval_timer"), remaining, false);
}

// ═══════════════════════════════════════════════════════════════
// Stop
// ═══════════════════════════════════════════════════════════════

void DefrostModule::on_stop() {
    if (phase_ != Phase::IDLE) {
        ESP_LOGW(TAG, "Stopping during active defrost — aborting");
    }
    clear_requests();
    state_set(ns_key("active"), false);
    state_set(ns_key("phase"), "idle");
    state_set(ns_key("state"), "stopped");
    ESP_LOGI(TAG, "Defrost stopped");
}
