/**
 * @file defrost_module.h
 * @brief Defrost cycle — 7-phase state machine (spec_v3 §3)
 *
 * State machine:
 *   IDLE → [dFT=2: STABILIZE → VALVE_OPEN →] ACTIVE
 *        → [dFT=2: EQUALIZE →] DRIP → FAD → IDLE
 *
 * Defrost types (dFT):
 *   0 = Natural (compressor stop)
 *   1 = Heater (electric)
 *   2 = Hot gas (7-phase with pressure equalization)
 *
 * Initiation methods:
 *   0 = Timer (dit hours, real-time or compressor-time)
 *   1 = Demand (T_evap < dSS)
 *   2 = Combined (first of timer or demand)
 *   3 = Disabled
 *
 * Termination modes:
 *   0 = By temperature: T_evap >= dSt (after 60s min), max_duration as safety backup
 *   1 = By timer: max_duration only, T_evap ignored
 *   - Skip: if T_evap > dSt before start (evaporator clean, temp mode only)
 *
 * Equipment Layer integration:
 *   Defrost does NOT access HAL directly. It publishes requests via
 *   defrost.req.* and sets defrost.active=true.
 *   Equipment Manager arbitrates: Protection > Defrost > Thermostat.
 *
 * SharedState keys read:
 *   equipment.evap_temp     — float (°C), evaporator sensor
 *   equipment.sensor2_ok    — bool, evap sensor health
 *   equipment.compressor    — bool, actual compressor state (for dct=2)
 *   protection.lockout      — bool, emergency stop
 *
 * SharedState keys written:
 *   defrost.active              — bool (main signal for EM/Thermostat/Protection)
 *   defrost.phase               — string (idle/stabilize/valve_open/active/equalize/drip/fad)
 *   defrost.state               — string (human-readable for UI)
 *   defrost.phase_timer         — int (seconds in current phase)
 *   defrost.interval_timer      — int (seconds remaining, NOT persisted)
 *   defrost.defrost_count       — int (NOT persisted, resets on reboot)
 *   defrost.last_termination    — string (temp/timeout)
 *   defrost.consecutive_timeouts — int
 *   defrost.req.compressor      — bool
 *   defrost.req.defrost_relay   — bool
 *   defrost.req.evap_fan        — bool
 *   defrost.req.cond_fan        — bool
 *   defrost.manual_start        — bool (write trigger)
 */

#pragma once

#include "modesp/base_module.h"

class DefrostModule : public modesp::BaseModule {
public:
    DefrostModule();

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

private:
    enum class Phase { IDLE, STABILIZE, VALVE_OPEN, ACTIVE, EQUALIZE, DRIP, FAD };
    Phase phase_ = Phase::IDLE;

    void enter_phase(Phase p);
    const char* phase_name() const;

    // Initiation logic
    void update_idle(uint32_t dt_ms);
    bool check_timer_trigger();
    bool check_demand_trigger();
    void start_defrost(const char* reason);

    // Phase handlers
    void update_active_phase(uint32_t dt_ms);
    void update_drip(uint32_t dt_ms);
    void update_fad(uint32_t dt_ms);
    void update_stabilize(uint32_t dt_ms);
    void update_valve_open(uint32_t dt_ms);
    void update_equalize(uint32_t dt_ms);

    // Termination
    void finish_active_phase(const char* reason);
    void finish_defrost();

    // Requests до EM
    void set_requests(bool comp, bool relay, bool evap_fan, bool cond_fan);
    void clear_requests();

    // Helpers
    void    sync_settings();
    void    publish_state();

    // === Settings (з SharedState, persist) ===
    int32_t  defrost_type_     = 0;         // dFT: 0/1/2
    uint32_t interval_ms_      = 28800000;  // dit: 8h → ms
    int32_t  counter_mode_     = 1;         // dct: 1=real, 2=compressor
    int32_t  initiation_       = 0;         // 0=timer, 1=demand, 2=combo, 3=disabled
    int32_t  termination_      = 0;         // 0=by temp, 1=by timer
    float    end_temp_         = 8.0f;      // dSt °C
    uint32_t max_duration_ms_  = 1800000;   // dEt: 30min → ms
    float    demand_temp_      = -25.0f;    // dSS °C
    uint32_t drip_time_ms_     = 120000;    // dPt: 120s → ms
    uint32_t fan_delay_ms_     = 120000;    // FAd: 120s → ms
    float    fad_temp_         = -5.0f;     // FAT °C
    uint32_t stabilize_ms_     = 30000;     // Phase 1: 30s
    uint32_t valve_delay_ms_   = 3000;      // Phase 2: 3s
    uint32_t equalize_ms_      = 90000;     // Phase 4: 90s

    // === Runtime ===
    uint32_t phase_timer_ms_        = 0;
    uint32_t interval_timer_ms_     = 0;      // persist в NVS
    int32_t  defrost_count_         = 0;      // persist в NVS
    int32_t  consecutive_timeouts_  = 0;
    int32_t  active_defrost_type_   = 0;      // Тип defrost кешований при start (BUG-002 fix)

    // Кешований стан requests (для delta-update)
    bool req_comp_    = false;
    bool req_relay_   = false;
    bool req_evap_    = false;
    bool req_cond_    = false;

    // Inputs cache
    float evap_temp_      = 0.0f;
    bool  sensor2_ok_     = false;
    bool  compressor_on_  = false;

    // Мінімальний час ACTIVE фази перед перевіркою end_temp (60с)
    // Стандартна практика промислових контролерів (Dixell/Danfoss)
    static constexpr uint32_t MIN_ACTIVE_CHECK_MS = 60000;
};
