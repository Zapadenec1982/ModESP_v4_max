/**
 * @file eev_module.h
 * @brief Electronic Expansion Valve — superheat PI controller
 *
 * State machine:
 *   IDLE → STARTUP → RUNNING ↔ LOW_SH_PROTECT
 *                  ↘ DEFROST → RUNNING
 *                  ↘ SENSOR_FAULT
 *
 * Algorithm: Velocity-form PI (industrial standard for superheat)
 *   delta = Kp * (error - prev_error) + Ki * Ts * error
 *   position += delta
 *
 * Inputs:
 *   - equipment.suction_bar → T_sat (via refrigerant lookup)
 *   - equipment.evap_temp   → T_evap outlet
 *   - equipment.compressor  → compressor state
 *   - defrost.active        → defrost pause
 *
 * Output:
 *   - eev.req.valve_pos (0-100%) → Equipment → IValveDriver
 */

#pragma once

#include "modesp/base_module.h"

class EevModule : public modesp::BaseModule {
public:
    EevModule();
    EevModule(const char* ns, etl::span<const modesp::InputBinding> inputs);

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;

private:
    // ── State machine ──
    enum class State : uint8_t {
        IDLE,              // Compressor OFF, valve closed
        STARTUP,           // Compressor just started, feed-forward opening, PI disabled
        RUNNING,           // Normal PI regulation
        LOW_SH_PROTECT,    // Superheat dangerously low, aggressive close
        DEFROST,           // Defrost active, valve closed
        SENSOR_FAULT       // Sensor failure, safe position
    };

    State state_ = State::IDLE;
    uint32_t state_timer_ms_ = 0;

    void enter_state(State new_state);
    const char* state_name() const;

    // ── PI Controller (velocity form) ──
    float prev_error_ = 0.0f;
    float position_ = 0.0f;        // Current valve position 0-100%
    uint32_t pi_timer_ms_ = 0;

    void update_pi(float superheat, float target);
    void reset_pi();

    // ── Settings (synced from SharedState) ──
    void sync_settings();

    float sh_target_ = 8.0f;       // Superheat setpoint (K)
    float kp_ = 3.0f;              // Proportional gain
    float ki_ = 0.5f;              // Integral gain
    float startup_pos_ = 50.0f;    // % at compressor start
    uint32_t startup_wait_ms_ = 120000;  // ms before PI enables
    float min_pos_ = 5.0f;         // Minimum position %
    float max_pos_ = 95.0f;        // Maximum position %
    float safe_pos_ = 40.0f;       // Position on sensor fault %
    float low_sh_limit_ = 2.0f;    // Low SH protection threshold (K)
    float mop_pressure_ = 0.0f;    // MOP bar (0=disabled)
    float deadband_ = 1.0f;        // Anti-hunting deadband (K)
    uint32_t pi_interval_ms_ = 3000;  // PI calculation interval

    // ── Inputs cache ──
    float suction_bar_ = 0.0f;
    float evap_temp_ = 0.0f;
    float t_sat_ = 0.0f;
    float superheat_ = 0.0f;
    bool compressor_on_ = false;
    bool prev_compressor_ = false;
    bool defrost_active_ = false;
    bool sensor_ok_ = false;
    bool pressure_ok_ = false;
    int32_t refrigerant_idx_ = 0;

    // ── Output ──
    void set_valve_position(float pos_pct);
};
