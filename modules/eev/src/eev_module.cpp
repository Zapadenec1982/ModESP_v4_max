/**
 * @file eev_module.cpp
 * @brief EEV superheat PI controller implementation
 *
 * Velocity-form PI (industrial standard):
 *   delta = Kp * (error - prev_error) + Ki * Ts * error
 *   position += delta
 *
 * Benefits over positional PI:
 *   - Natural anti-windup (no integral accumulator)
 *   - Bumpless transfer between states
 *   - Output is delta (steps), not absolute value
 */

#include "eev_module.h"
#include "modesp/refrigerant/saturation.h"
#include "esp_log.h"
#include <cmath>

static const char TAG[] = "EEV";

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

EevModule::EevModule()
    : BaseModule("eev", modesp::ModulePriority::NORMAL)
{}

EevModule::EevModule(const char* ns, etl::span<const modesp::InputBinding> inputs)
    : BaseModule("eev", ns, modesp::ModulePriority::NORMAL, inputs)
{}

// ═══════════════════════════════════════════════════════════════
// Settings sync from SharedState
// ═══════════════════════════════════════════════════════════════

void EevModule::sync_settings() {
    sh_target_      = read_float(ns_key("sh_target"), sh_target_);
    kp_             = read_float(ns_key("kp"), kp_);
    ki_             = read_float(ns_key("ki"), ki_);
    startup_pos_    = read_float(ns_key("startup_pos"), startup_pos_);
    startup_wait_ms_ = static_cast<uint32_t>(read_int(ns_key("startup_wait"), 120)) * 1000;
    min_pos_        = read_float(ns_key("min_pos"), min_pos_);
    max_pos_        = read_float(ns_key("max_pos"), max_pos_);
    safe_pos_       = read_float(ns_key("safe_pos"), safe_pos_);
    low_sh_limit_   = read_float(ns_key("low_sh_limit"), low_sh_limit_);
    mop_pressure_   = read_float(ns_key("mop_pressure"), mop_pressure_);
    deadband_       = read_float(ns_key("deadband"), deadband_);
    pi_interval_ms_ = static_cast<uint32_t>(read_int(ns_key("pi_interval"), 3)) * 1000;
}

// ═══════════════════════════════════════════════════════════════
// State machine
// ═══════════════════════════════════════════════════════════════

const char* EevModule::state_name() const {
    switch (state_) {
        case State::IDLE:           return "idle";
        case State::STARTUP:        return "startup";
        case State::RUNNING:        return "running";
        case State::LOW_SH_PROTECT: return "low_sh";
        case State::DEFROST:        return "defrost";
        case State::SENSOR_FAULT:   return "fault";
    }
    return "unknown";
}

void EevModule::enter_state(State new_state) {
    if (state_ == new_state) return;

    ESP_LOGI(TAG, "%s → %s", state_name(),
             new_state == State::IDLE           ? "idle" :
             new_state == State::STARTUP        ? "startup" :
             new_state == State::RUNNING        ? "running" :
             new_state == State::LOW_SH_PROTECT ? "low_sh" :
             new_state == State::DEFROST        ? "defrost" :
             new_state == State::SENSOR_FAULT   ? "fault" : "?");

    state_ = new_state;
    state_timer_ms_ = 0;

    switch (new_state) {
        case State::IDLE:
            set_valve_position(0.0f);
            reset_pi();
            break;

        case State::STARTUP:
            // Feed-forward: open to startup position immediately
            set_valve_position(startup_pos_);
            reset_pi();
            ESP_LOGI(TAG, "Startup: valve=%.0f%%, PI disabled for %lus",
                     startup_pos_, startup_wait_ms_ / 1000);
            break;

        case State::RUNNING:
            // PI starts from current position (bumpless transfer)
            ESP_LOGI(TAG, "PI active: target=%.1fK, Kp=%.1f, Ki=%.2f",
                     sh_target_, kp_, ki_);
            break;

        case State::LOW_SH_PROTECT:
            ESP_LOGW(TAG, "LOW SUPERHEAT PROTECTION — SH=%.1fK < %.1fK",
                     superheat_, low_sh_limit_);
            break;

        case State::DEFROST:
            set_valve_position(0.0f);
            reset_pi();
            ESP_LOGI(TAG, "Defrost: valve closed, PI reset");
            break;

        case State::SENSOR_FAULT:
            set_valve_position(safe_pos_);
            reset_pi();
            ESP_LOGW(TAG, "SENSOR FAULT — safe position %.0f%%", safe_pos_);
            break;
    }

    state_set(ns_key("state"), state_name());
}

// ═══════════════════════════════════════════════════════════════
// PI Controller — velocity form
// ═══════════════════════════════════════════════════════════════

void EevModule::update_pi(float superheat, float target) {
    float error = superheat - target;

    // Deadband: don't adjust if within band
    if (fabsf(error) < deadband_) {
        prev_error_ = error;
        return;
    }

    // Velocity-form PI: delta = Kp*(e-e_prev) + Ki*Ts*e
    // Ts in seconds
    float ts = static_cast<float>(pi_interval_ms_) / 1000.0f;
    float delta = kp_ * (error - prev_error_) + ki_ * ts * error;

    // Apply delta to position
    // Positive error (SH too high) → open valve more (increase position)
    // Negative error (SH too low) → close valve (decrease position)
    position_ += delta;

    // Clamp to min/max
    if (position_ < min_pos_) position_ = min_pos_;
    if (position_ > max_pos_) position_ = max_pos_;

    prev_error_ = error;

    state_set(ns_key("pi_output"), delta);
}

void EevModule::reset_pi() {
    prev_error_ = 0.0f;
    pi_timer_ms_ = 0;
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_init
// ═══════════════════════════════════════════════════════════════

bool EevModule::on_init() {
    sync_settings();

    // Publish initial state
    state_set(ns_key("superheat"), 0.0f);
    state_set(ns_key("t_sat"), 0.0f);
    state_set(ns_key("valve_pos"), 0.0f);
    state_set(ns_key("state"), "idle");
    state_set(ns_key("pi_output"), 0.0f);
    state_set(ns_key("req.valve_pos"), 0.0f);
    state_set(ns_key("sh_target"), sh_target_);

    ESP_LOGI(TAG, "Initialized (SH target=%.1fK, Kp=%.1f, Ki=%.2f)", sh_target_, kp_, ki_);
    ESP_LOGI(TAG, "  startup=%.0f%% wait=%lus, min=%.0f%% max=%.0f%%",
             startup_pos_, startup_wait_ms_ / 1000, min_pos_, max_pos_);
    ESP_LOGI(TAG, "  low_sh=%.1fK, MOP=%.1f bar, deadband=%.1fK",
             low_sh_limit_, mop_pressure_, deadband_);
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle: on_update — main control loop
// ═══════════════════════════════════════════════════════════════

void EevModule::on_update(uint32_t dt_ms) {
    // 1. Sync settings (WebUI/API може їх змінити)
    sync_settings();

    // 2. Read inputs
    suction_bar_    = read_input_float("equipment.suction_bar");
    evap_temp_      = read_input_float("equipment.evap_temp");
    compressor_on_  = read_input_bool("equipment.compressor");
    defrost_active_ = read_input_bool("defrost.active");
    sensor_ok_      = read_input_bool("equipment.sensor2_ok", true);
    pressure_ok_    = read_input_bool("equipment.has_suction_p");
    refrigerant_idx_ = read_input_int("equipment.refrigerant", 0);

    // 3. Calculate saturation temperature and superheat
    //    Use dew_point_temp() — correct for zeotropic blends (glide > 0).
    //    For azeotropes (glide=0) returns identical to saturation_temp().
    if (pressure_ok_ && suction_bar_ > 0.0f) {
        auto ref = static_cast<modesp::Refrigerant>(refrigerant_idx_);
        t_sat_ = modesp::dew_point_temp(ref, suction_bar_);
        superheat_ = evap_temp_ - t_sat_;
    } else {
        t_sat_ = NAN;
        superheat_ = NAN;
    }

    // 4. Update timers
    state_timer_ms_ += dt_ms;
    pi_timer_ms_ += dt_ms;

    // 5. Check sensor health
    bool sensors_valid = sensor_ok_ && pressure_ok_ && !std::isnan(superheat_);

    // ═══ Compressor edge detection ═══
    bool comp_rising = compressor_on_ && !prev_compressor_;
    bool comp_falling = !compressor_on_ && prev_compressor_;
    prev_compressor_ = compressor_on_;

    // ═══ State machine ═══
    switch (state_) {

        // ── IDLE: compressor off, valve closed ──
        case State::IDLE:
            if (defrost_active_) {
                enter_state(State::DEFROST);
            } else if (comp_rising) {
                if (sensors_valid) {
                    enter_state(State::STARTUP);
                } else {
                    enter_state(State::SENSOR_FAULT);
                }
            }
            break;

        // ── STARTUP: feed-forward opening, wait for system to stabilize ──
        case State::STARTUP:
            if (!compressor_on_) {
                enter_state(State::IDLE);
                break;
            }
            if (defrost_active_) {
                enter_state(State::DEFROST);
                break;
            }
            if (!sensors_valid) {
                enter_state(State::SENSOR_FAULT);
                break;
            }

            // Low SH protection active even during startup
            if (superheat_ < low_sh_limit_ && superheat_ > -50.0f) {
                // Aggressive close during startup
                position_ -= 2.0f;
                if (position_ < min_pos_) position_ = min_pos_;
                set_valve_position(position_);
            }

            // Wait for stabilization
            if (state_timer_ms_ >= startup_wait_ms_) {
                enter_state(State::RUNNING);
            }
            break;

        // ── RUNNING: PI regulation ──
        case State::RUNNING:
            if (!compressor_on_) {
                enter_state(State::IDLE);
                break;
            }
            if (defrost_active_) {
                enter_state(State::DEFROST);
                break;
            }
            if (!sensors_valid) {
                enter_state(State::SENSOR_FAULT);
                break;
            }

            // MOP check: close valve if suction pressure too high
            if (mop_pressure_ > 0.0f && suction_bar_ > mop_pressure_) {
                position_ -= 3.0f;  // Aggressive close
                if (position_ < min_pos_) position_ = min_pos_;
                set_valve_position(position_);
                break;
            }

            // Low superheat protection
            if (superheat_ < low_sh_limit_) {
                enter_state(State::LOW_SH_PROTECT);
                break;
            }

            // PI calculation at configured interval
            if (pi_timer_ms_ >= pi_interval_ms_) {
                pi_timer_ms_ = 0;
                update_pi(superheat_, sh_target_);
                set_valve_position(position_);
            }
            break;

        // ── LOW_SH_PROTECT: superheat dangerously low ──
        case State::LOW_SH_PROTECT:
            if (!compressor_on_) {
                enter_state(State::IDLE);
                break;
            }
            if (!sensors_valid) {
                enter_state(State::SENSOR_FAULT);
                break;
            }

            // Aggressive close: 2-3x normal rate
            if (pi_timer_ms_ >= pi_interval_ms_) {
                pi_timer_ms_ = 0;
                position_ -= 3.0f;  // Close 3% per interval
                if (position_ < min_pos_) position_ = min_pos_;
                set_valve_position(position_);
            }

            // Subcooled (SH < 0): emergency close at 150Hz (3x normal speed)
            if (superheat_ < 0.0f) {
                position_ = 0.0f;
                set_valve_position(0.0f);
                state_set(ns_key("req.emergency_close"), true);  // Equipment → IValveDriver::emergency_close()
                ESP_LOGW(TAG, "SUBCOOLED — emergency valve close!");
            }

            // Exit when superheat recovers above target
            if (superheat_ > sh_target_) {
                ESP_LOGI(TAG, "Superheat recovered: %.1fK > %.1fK", superheat_, sh_target_);
                enter_state(State::RUNNING);
            }
            break;

        // ── DEFROST: valve closed until defrost ends ──
        case State::DEFROST:
            if (!defrost_active_) {
                if (compressor_on_ && sensors_valid) {
                    enter_state(State::STARTUP);
                } else {
                    enter_state(State::IDLE);
                }
            }
            break;

        // ── SENSOR_FAULT: safe position until sensors recover ──
        case State::SENSOR_FAULT:
            if (comp_falling) {
                enter_state(State::IDLE);
                break;
            }
            if (sensors_valid) {
                ESP_LOGI(TAG, "Sensors recovered — restarting PI");
                if (compressor_on_) {
                    enter_state(State::STARTUP);
                } else {
                    enter_state(State::IDLE);
                }
            }
            break;
    }

    // 6. Publish state
    state_set(ns_key("superheat"), superheat_, false);  // Silent (no WS broadcast every 10ms)
    state_set(ns_key("t_sat"), t_sat_, false);
    state_set(ns_key("valve_pos"), position_, false);
}

// ═══════════════════════════════════════════════════════════════
// Output: set valve position
// ═══════════════════════════════════════════════════════════════

void EevModule::set_valve_position(float pos_pct) {
    if (pos_pct < 0.0f) pos_pct = 0.0f;
    if (pos_pct > 100.0f) pos_pct = 100.0f;
    position_ = pos_pct;

    // Publish as request for Equipment to forward to IValveDriver
    state_set(ns_key("req.valve_pos"), pos_pct);
}
