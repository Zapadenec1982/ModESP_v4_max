/**
 * @file thermostat_module.h
 * @brief Thermostat v2 — full spec_v3 logic with asymmetric differential
 *
 * Algorithm (spec_v3 §2.1):
 *   ON:  T_air >= setpoint + differential  (upper threshold)
 *   OFF: T_air <= setpoint                 (lower threshold = setpoint)
 *
 * State machine:
 *   STARTUP → IDLE → COOLING → (IDLE)
 *                  ↘ SAFETY_RUN ↗  (sensor failure → cyclic compressor)
 *
 * Compressor protection:
 *   min_on_time  (cOt) — compressor won't stop before this elapsed
 *   min_off_time (cFt) — compressor won't start before this elapsed
 *   startup_delay      — delay after boot before first compressor start
 *
 * Fan control:
 *   Evaporator fan: 3 modes (constant / with compressor / by T_evap)
 *   Condenser fan: with compressor + delay after OFF (COd)
 *
 * Equipment Layer integration:
 *   Thermostat does NOT access HAL directly. It reads temperature from
 *   equipment.air_temp and publishes requests via thermostat.req.*
 *   Equipment Manager applies arbitration and drives the actual relays.
 *
 * SharedState keys read:
 *   equipment.air_temp      — float (°C), from Equipment Manager
 *   equipment.evap_temp     — float (°C), evaporator sensor (optional)
 *   equipment.sensor1_ok    — bool, air sensor health
 *   equipment.sensor2_ok    — bool, evap sensor health (optional)
 *   defrost.active          — bool, defrost in progress (optional)
 *   defrost.phase           — string, defrost phase e.g. "fad" (optional)
 *   protection.lockout      — bool, emergency stop (optional)
 *
 * SharedState keys written:
 *   thermostat.temperature      — float (mirror of equipment.air_temp)
 *   thermostat.setpoint         — float (°C, readwrite, persisted)
 *   thermostat.differential     — float (°C, readwrite, persisted)
 *   thermostat.req.compressor   — bool (request to EM)
 *   thermostat.req.evap_fan     — bool (request to EM)
 *   thermostat.req.cond_fan     — bool (request to EM)
 *   thermostat.state            — string (startup|idle|cooling|safety_run)
 *   thermostat.comp_on_time     — int (seconds)
 *   thermostat.comp_off_time    — int (seconds)
 */

#pragma once

#include "modesp/base_module.h"

class ThermostatModule : public modesp::BaseModule {
public:
    ThermostatModule();

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_message(const etl::imessage& msg) override;
    void on_stop() override;

private:
    // State machine
    enum class State { STARTUP, IDLE, COOLING, SAFETY_RUN };
    State state_ = State::STARTUP;
    void enter_state(State new_state);
    const char* state_name() const;

    // Логіка регулювання
    void update_regulation(uint32_t dt_ms);
    void update_evap_fan();
    void update_cond_fan(uint32_t dt_ms);
    void update_safety_run(uint32_t dt_ms);

    // Helpers
    void sync_settings();
    bool is_night_active();

    // Requests до Equipment Manager
    void request_compressor(bool on);
    void request_evap_fan(bool on);
    void request_cond_fan(bool on);

    // Публікація стану
    void publish_outputs();

    // === Settings (з SharedState, persist) ===
    float    setpoint_        = 4.0f;     // °C
    float    differential_    = 2.0f;     // °C (вгору від уставки)
    uint32_t min_off_ms_      = 180000;   // cFt (мс)
    uint32_t min_on_ms_       = 60000;    // cOt (мс)
    uint32_t startup_delay_ms_ = 60000;   // затримка після boot (мс)
    int32_t  evap_fan_mode_   = 1;        // FAn: 0=постійно, 1=з компр, 2=за T_evap
    float    fan_stop_temp_   = -25.0f;   // FST °C
    float    fan_stop_hyst_   = 2.0f;     // FST гістерезис °C
    uint32_t cond_fan_delay_ms_ = 30000;  // COd (мс)
    uint32_t safety_on_ms_    = 1200000;  // 20 хв
    uint32_t safety_off_ms_   = 600000;   // 10 хв

    // === Night setback ===
    float    night_setback_    = 3.0f;     // °C зміщення
    int32_t  night_mode_       = 0;        // 0=off, 1=schedule, 2=DI, 3=manual
    int32_t  night_start_      = 22;       // година початку
    int32_t  night_end_        = 6;        // година завершення
    bool     night_active_     = false;
    float    effective_sp_     = 4.0f;     // setpoint + night_setback (коли active)

    // === Display during defrost ===
    int32_t  display_defrost_  = 1;        // 0=real, 1=frozen, 2="-d-"
    float    frozen_temp_      = 0.0f;     // T зафіксована на початку відтайки

    // === Runtime state ===
    float current_temp_    = 0.0f;
    float evap_temp_       = 0.0f;
    bool  sensor1_ok_      = false;
    bool  sensor2_ok_      = false;

    bool  compressor_on_   = false;
    bool  evap_fan_on_     = false;
    bool  cond_fan_on_     = false;

    uint32_t state_timer_ms_   = 0;        // Час в поточному стані
    uint32_t comp_on_time_ms_  = 0;        // Час з моменту ON
    uint32_t comp_off_time_ms_ = modesp::TIMER_SATISFIED;   // Час з моменту OFF (великий для першого старту)
    uint32_t cond_fan_off_timer_ms_ = 0;   // Таймер затримки вимкнення вент. конд.
    bool     cond_fan_delay_active_ = false;

    // Safety Run
    bool     safety_phase_on_  = false;    // true=ON phase, false=OFF phase
    uint32_t safety_timer_ms_  = 0;

    // Зовнішні сигнали
    bool  defrost_active_      = false;
    bool  was_defrost_active_  = false;    // для детекції start/end transition
    bool  protection_lockout_  = false;
    bool  was_lockout_active_  = false;    // для детекції lockout→clear transition

    // Кеш попередніх значень для уникнення зайвих version bumps
    float last_effective_sp_   = -999.0f;
    float last_display_temp_   = -999.0f;
};
