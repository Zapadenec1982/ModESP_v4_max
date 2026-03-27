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

static const char* TAG = "Defrost";

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

DefrostModule::DefrostModule()
    : BaseModule("defrost", modesp::ModulePriority::NORMAL)
{}

// ═══════════════════════════════════════════════════════════════
// Sync settings з SharedState
// ═══════════════════════════════════════════════════════════════

void DefrostModule::sync_settings() {
    defrost_type_  = read_int("defrost.type", 0);
    counter_mode_  = read_int("defrost.counter_mode", 1);
    initiation_    = read_int("defrost.initiation", 0);
    termination_   = read_int("defrost.termination", 0);
    end_temp_      = read_float("defrost.end_temp", end_temp_);
    demand_temp_   = read_float("defrost.demand_temp", demand_temp_);
    fad_temp_      = read_float("defrost.fad_temp", fad_temp_);

    // Години → мілісекунди
    interval_ms_ = static_cast<uint32_t>(read_int("defrost.interval", 8)) * 3600000;

    // Хвилини → мілісекунди
    max_duration_ms_ = static_cast<uint32_t>(read_int("defrost.max_duration", 30)) * 60000;

    // Хвилини → мілісекунди
    drip_time_ms_   = static_cast<uint32_t>(read_int("defrost.drip_time", 2)) * 60000;
    fan_delay_ms_   = static_cast<uint32_t>(read_int("defrost.fan_delay", 2)) * 60000;
    stabilize_ms_   = static_cast<uint32_t>(read_int("defrost.stabilize_time", 1)) * 60000;
    equalize_ms_    = static_cast<uint32_t>(read_int("defrost.equalize_time", 2)) * 60000;

    // Секунди → мілісекунди
    valve_delay_ms_ = static_cast<uint32_t>(read_int("defrost.valve_delay", 3)) * 1000;
}

// ═══════════════════════════════════════════════════════════════
// Phase name
// ═══════════════════════════════════════════════════════════════

const char* DefrostModule::phase_name() const {
    switch (phase_) {
        case Phase::IDLE:       return "idle";
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
        state_set("defrost.req.compressor", comp);
    }
    if (req_relay_ != relay) {
        req_relay_ = relay;
        state_set("defrost.req.defrost_relay", relay);
    }
    if (req_evap_ != evap_fan) {
        req_evap_ = evap_fan;
        state_set("defrost.req.evap_fan", evap_fan);
    }
    if (req_cond_ != cond_fan) {
        req_cond_ = cond_fan;
        state_set("defrost.req.cond_fan", cond_fan);
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

    state_set("defrost.active", p != Phase::IDLE);
    state_set("defrost.phase", phase_name());

    switch (p) {
        case Phase::IDLE:
            clear_requests();
            state_set("defrost.state", "idle");
            break;

        case Phase::STABILIZE:
            // dFT=2, Фаза 1: компресор ON + конд.вент ON, реле OFF
            // Тиск стабілізується перед відкриттям клапана ГГ
            set_requests(true, false, false, true);
            state_set("defrost.state", "stabilize");
            break;

        case Phase::VALVE_OPEN:
            // dFT=2, Фаза 2: компресор ON + реле ON (клапан ГГ) + конд.вент ON
            set_requests(true, true, false, true);
            state_set("defrost.state", "valve_open");
            break;

        case Phase::ACTIVE:
            // Використовуємо active_defrost_type_ — кешований при старті циклу (BUG-002 fix)
            if (active_defrost_type_ == 0) {
                // Природна: все OFF (компресор зупиняється)
                set_requests(false, false, false, false);
                state_set("defrost.state", "defrost_natural");
            } else if (active_defrost_type_ == 1) {
                // Електричний тен: реле ON, компресор OFF
                // EM інтерлок заблокує компресор при type=1
                set_requests(false, true, false, false);
                state_set("defrost.state", "defrost_heater");
            } else {
                // ГГ: компресор ON + реле ON (клапан ГГ)
                set_requests(true, true, false, false);
                state_set("defrost.state", "defrost_hotgas");
            }
            break;

        case Phase::EQUALIZE:
            // dFT=2, Фаза 4: все OFF, тиск падає
            set_requests(false, false, false, false);
            state_set("defrost.state", "equalize");
            break;

        case Phase::DRIP:
            // Все OFF, вода стікає
            set_requests(false, false, false, false);
            state_set("defrost.state", "drip");
            break;

        case Phase::FAD:
            // Компресор ON, конд. вент. ON, реле OFF
            set_requests(true, false, false, true);
            state_set("defrost.state", "fad");
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
    interval_timer_ms_ = static_cast<uint32_t>(read_int("defrost.interval_timer", 0)) * 1000;
    defrost_count_ = read_int("defrost.defrost_count", 0);

    // Початковий стан
    state_set("defrost.active", false);
    state_set("defrost.phase", "idle");
    state_set("defrost.state", "idle");
    state_set("defrost.phase_timer", static_cast<int32_t>(0));
    state_set("defrost.interval_timer", static_cast<int32_t>(interval_timer_ms_ / 1000));
    state_set("defrost.defrost_count", defrost_count_);
    state_set("defrost.last_termination", "none");
    state_set("defrost.consecutive_timeouts", static_cast<int32_t>(0));
    state_set("defrost.heater_alarm", false);
    state_set("defrost.manual_start", false);
    state_set("defrost.manual_stop", false);
    state_set("defrost.req.compressor", false);
    state_set("defrost.req.defrost_relay", false);
    state_set("defrost.req.evap_fan", false);
    state_set("defrost.req.cond_fan", false);

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
    evap_temp_     = read_float("equipment.evap_temp");
    sensor2_ok_    = read_bool("equipment.sensor2_ok");
    compressor_on_ = read_bool("equipment.compressor");
    bool lockout   = read_bool("protection.lockout");

    // Protection lockout → abort defrost
    if (lockout && phase_ != Phase::IDLE) {
        ESP_LOGW(TAG, "Protection lockout — aborting defrost");
        finish_defrost();
        return;
    }

    // Compressor blocked → abort hot gas defrost (потрібен потік хладагенту)
    bool comp_blocked = read_bool("protection.compressor_blocked");
    if (comp_blocked && phase_ != Phase::IDLE && active_defrost_type_ == 2) {
        if (phase_ == Phase::STABILIZE || phase_ == Phase::VALVE_OPEN || phase_ == Phase::ACTIVE) {
            ESP_LOGW(TAG, "Compressor blocked — aborting hot gas defrost");
            finish_defrost();
            return;
        }
    }

    // Ручна зупинка розморозки
    if (read_bool("defrost.manual_stop")) {
        state_set("defrost.manual_stop", false);
        if (phase_ != Phase::IDLE) {
            ESP_LOGI(TAG, "Manual stop — aborting defrost");
            finish_defrost();
            return;
        }
    }

    // Phase dispatch
    switch (phase_) {
        case Phase::IDLE:
            update_idle(dt_ms);
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
    if (read_bool("defrost.manual_start")) {
        state_set("defrost.manual_start", false);
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

    // Перевіряємо triggers
    bool timer_trigger  = (initiation_ == 0 || initiation_ == 2) && check_timer_trigger();
    bool demand_trigger = (initiation_ == 1 || initiation_ == 2) && check_demand_trigger();

    if (timer_trigger || demand_trigger) {
        // Оптимізація: випарник чистий → скасовуємо (тільки в temp-mode)
        if (termination_ == 0 && sensor2_ok_ && evap_temp_ > end_temp_) {
            ESP_LOGI(TAG, "Defrost skipped — evap clean (%.1f > %.1f)",
                     evap_temp_, end_temp_);
            interval_timer_ms_ = 0;
            return;
        }
        start_defrost(timer_trigger ? "timer" : "demand");
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
        && !read_bool("equipment.has_defrost_relay")) {
        ESP_LOGW(TAG, "Defrost type %ld selected but defrost_relay NOT configured — fallback to NATURAL",
                 static_cast<long>(active_defrost_type_));
        active_defrost_type_ = 0;
    }

    ESP_LOGI(TAG, "Starting defrost (%s), type=%ld", reason, static_cast<long>(active_defrost_type_));
    interval_timer_ms_ = 0;

    if (active_defrost_type_ == 2) {
        // Гарячий газ → починаємо зі стабілізації
        enter_phase(Phase::STABILIZE);
    } else {
        // Природна або тен → одразу active
        enter_phase(Phase::ACTIVE);
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
        if (sensor2_ok_ && evap_temp_ >= end_temp_) {
            consecutive_timeouts_ = 0;
            state_set("defrost.heater_alarm", false);
            state_set("defrost.last_termination", "temp");
            finish_active_phase("temp reached");
            return;
        }
    }

    // Завершення по таймеру (dEt) — завжди працює в обох modes
    if (phase_timer_ms_ >= max_duration_ms_) {
        if (termination_ == 0) {
            // temp-mode: таймер = safety backup → лічильник таймаутів
            consecutive_timeouts_++;
            state_set("defrost.consecutive_timeouts", consecutive_timeouts_);
            if (consecutive_timeouts_ >= 3) {
                ESP_LOGW(TAG, "3 consecutive timeouts — possible heater/sensor failure!");
                state_set("defrost.heater_alarm", true);
            }
        } else {
            // timer-mode: нормальне завершення
            consecutive_timeouts_ = 0;
        }
        state_set("defrost.last_termination", "timeout");
        finish_active_phase(termination_ == 0 ? "timeout" : "timer");
        return;
    }
}

void DefrostModule::finish_active_phase(const char* reason) {
    defrost_count_++;
    state_set("defrost.defrost_count", defrost_count_);
    ESP_LOGI(TAG, "Active defrost finished (%s), count=%ld",
             reason, static_cast<long>(defrost_count_));

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
    state_set("defrost.phase_timer", static_cast<int32_t>(phase_timer_ms_ / 1000), false);
    // Зворотній відлік: скільки залишилось до наступної розморозки
    int32_t remaining = (interval_ms_ > interval_timer_ms_)
        ? static_cast<int32_t>((interval_ms_ - interval_timer_ms_) / 1000)
        : 0;
    state_set("defrost.interval_timer", remaining, false);
}

// ═══════════════════════════════════════════════════════════════
// Stop
// ═══════════════════════════════════════════════════════════════

void DefrostModule::on_stop() {
    if (phase_ != Phase::IDLE) {
        ESP_LOGW(TAG, "Stopping during active defrost — aborting");
    }
    clear_requests();
    state_set("defrost.active", false);
    state_set("defrost.phase", "idle");
    state_set("defrost.state", "stopped");
    ESP_LOGI(TAG, "Defrost stopped");
}
