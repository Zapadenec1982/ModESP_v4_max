/**
 * @file protection_module.h
 * @brief Protection module — alarm monitoring, compressor safety, rate diagnostics
 *
 * Independent alarm monitors:
 *   1. High Temp (HAL)    — delayed, blocked during defrost
 *   2. Low Temp (LAL)     — delayed, always active
 *   3. Sensor1 (ERR1)     — instant, air sensor failure
 *   4. Sensor2 (ERR2)     — instant, evap sensor failure (info only)
 *   5. Door open          — delayed, info only
 *   6. Short Cycling      — 3 consecutive runs < min_compressor_run
 *   7. Rapid Cycling      — starts > max_starts_hour in rolling 60-min window
 *   8. Continuous Run     — compressor ON > max_continuous_run
 *   9. Pulldown Failure   — compressor ON > pulldown_timeout, temp not dropped
 *  10. Rate-of-Change     — temp rises > max_rise_rate while compressor ON
 *
 * Each delayed monitor: NORMAL → PENDING (delay) → ALARM → NORMAL (auto-clear)
 * Compressor monitors use CompressorTracker for cycle analysis.
 * Rate monitor uses RateTracker with EWMA (lambda=0.3).
 *
 * Continuous run escalation:
 *   Level 1: compressor_blocked (forced off, fans still run)
 *   Level 2: lockout (permanent, all OFF, manual reset required)
 *
 * Priority: HIGH(1) — runs BEFORE Thermostat(2), AFTER Equipment(0)
 *
 * SharedState keys read:
 *   equipment.air_temp      — float (°C)
 *   equipment.evap_temp     — float (°C, optional, for pulldown)
 *   equipment.sensor1_ok    — bool
 *   equipment.sensor2_ok    — bool
 *   equipment.door_open     — bool
 *   equipment.compressor    — bool (actual relay state)
 *   defrost.active          — bool
 *
 * SharedState keys written:
 *   protection.lockout              — bool (permanent lockout after max retries)
 *   protection.compressor_blocked   — bool (forced off during continuous run)
 *   protection.continuous_run_count — int  (consecutive continuous run events)
 *   protection.alarm_active         — bool (any alarm active)
 *   protection.alarm_code           — string (highest priority code)
 *   protection.high_temp_alarm      — bool
 *   protection.low_temp_alarm       — bool
 *   protection.sensor1_alarm        — bool
 *   protection.sensor2_alarm        — bool
 *   protection.door_alarm           — bool
 *   protection.short_cycle_alarm    — bool
 *   protection.rapid_cycle_alarm    — bool
 *   protection.continuous_run_alarm — bool
 *   protection.pulldown_alarm       — bool
 *   protection.rate_alarm           — bool
 *   protection.compressor_starts_1h — int
 *   protection.compressor_duty      — float (0-100%)
 *   protection.compressor_run_time  — int (seconds, 0 if off)
 *   protection.last_cycle_run       — int (seconds)
 *   protection.last_cycle_off       — int (seconds)
 *   protection.compressor_hours     — float (cumulative hours, persist)
 *
 * Alarm code priority:
 *   lockout > comp_blocked > err1 > rate_rise > high_temp > pulldown >
 *   short_cycle > rapid_cycle > low_temp > continuous_run > err2 > door > none
 */

#pragma once

#include "modesp/base_module.h"

class ProtectionModule : public modesp::BaseModule {
public:
    ProtectionModule();

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

private:
    // Helpers
    void    sync_settings();

    // Структура для одного монітора аварій
    struct AlarmMonitor {
        bool active    = false;   // Аварія зараз
        bool pending   = false;   // В затримці
        uint32_t pending_ms = 0;  // Час в pending стані
    };

    // ── Існуючі монітори (7) ──
    AlarmMonitor high_temp_;
    AlarmMonitor low_temp_;
    AlarmMonitor sensor1_;   // ERR1 — instant (без затримки)
    AlarmMonitor sensor2_;   // ERR2 — instant
    AlarmMonitor door_;
    AlarmMonitor condenser_;        // Condenser high temp alarm
    AlarmMonitor condenser_block_;  // Condenser block (manual reset)

    // ── Компресорні монітори (5) ──
    AlarmMonitor short_cycle_;
    AlarmMonitor rapid_cycle_;
    AlarmMonitor continuous_run_;
    AlarmMonitor pulldown_;
    AlarmMonitor rate_rise_;

    // ── Компресорний трекер ──
    struct CompressorTracker {
        bool     prev_state   = false;
        uint32_t current_run_ms  = 0;
        uint32_t current_off_ms  = 0;
        uint32_t last_run_ms     = 0;
        uint32_t last_off_ms     = 0;
        float    temp_at_start   = 0.0f;
        float    evap_at_start   = 0.0f;   // evap_temp при старті (для pulldown)

        static constexpr size_t MAX_STARTS = 30;
        uint32_t start_timestamps[MAX_STARTS] = {};
        uint8_t  start_head      = 0;
        uint8_t  start_count     = 0;
        uint8_t  short_cycle_count = 0;
        uint32_t total_on_1h_ms  = 0;
        uint32_t window_ms       = 0;
    };

    // ── Трекер швидкості зміни температури ──
    struct RateTracker {
        float    prev_temp          = 0.0f;
        float    ewma_rate          = 0.0f;
        uint32_t rising_duration_ms = 0;
        bool     initialized        = false;
    };

    CompressorTracker comp_;
    RateTracker       rate_;
    uint32_t          diag_timer_ = 0;
    uint16_t          hours_persist_counter_ = 0;  // persist кожні 720 циклів (1 год)

    // Логіка існуючих моніторів
    void update_high_temp(float temp, bool sensor_ok, bool defrost_active, uint32_t dt_ms);
    void update_low_temp(float temp, bool sensor_ok, uint32_t dt_ms);
    void update_sensor_alarm(AlarmMonitor& m, bool sensor_ok, const char* label);
    void update_door_alarm(bool door_open, uint32_t dt_ms);
    void update_condenser_alarm(float cond_temp, bool has_cond, uint32_t dt_ms);
    void check_reset_command();
    void publish_alarms();

    // Логіка компресорного захисту
    void update_compressor_tracker(bool compressor_on, float temp, bool defrost_active, uint32_t dt_ms);
    void update_rate_tracker(float temp, uint32_t dt_ms);
    void publish_compressor_diagnostics();
    int  count_starts_in_window(uint32_t window_ms) const;

    // ── Налаштування (з SharedState, persist) — існуючі ──
    float    high_limit_      = 12.0f;     // °C (HAL)
    float    low_limit_       = -35.0f;    // °C (LAL)
    uint32_t high_alarm_delay_ms_ = 1800000;  // 30 хв
    uint32_t low_alarm_delay_ms_  = 1800000;  // 30 хв
    uint32_t door_delay_ms_   = 300000;    // 5 хв
    bool     manual_reset_    = false;

    // ── Налаштування — компресорний захист ──
    uint32_t min_compressor_run_ms_    = 120000;    // 120 сек
    int      max_starts_hour_          = 12;
    uint32_t max_continuous_run_ms_    = 21600000;  // 360 хв
    uint32_t pulldown_timeout_ms_      = 3600000;   // 60 хв
    float    pulldown_min_drop_        = 2.0f;      // °C
    float    max_rise_rate_            = 0.5f;      // °C/хв
    uint32_t rate_duration_ms_         = 300000;    // 5 хв
    float    compressor_hours_         = 0.0f;      // persist, кумулятивний наробіток

    // ── Ескалація continuous run (forced off → defrost → retry → lockout) ──
    bool     forced_off_active_        = false;     // Примусова зупинка активна
    uint32_t forced_off_timer_ms_      = 0;         // Таймер примусової зупинки
    uint8_t  continuous_run_count_     = 0;          // Лічильник послідовних спрацювань
    bool     permanent_lockout_        = false;      // Перманентна блокіровка
    uint32_t forced_off_period_ms_     = 1200000;    // 20 хв default
    int32_t  max_retries_   = 3;          // default 3

    // ── Condenser protection ──
    float    condenser_alarm_limit_    = 80.0f;   // °C — alarm threshold
    float    condenser_block_limit_    = 85.0f;   // °C — block threshold (manual reset)

    // ── Door → compressor delay ──
    uint32_t door_comp_delay_ms_      = 900000;   // 900s = 15 min (Danfoss C04 default)
    bool     door_comp_blocked_       = false;     // compressor blocked by door
    uint32_t door_comp_timer_ms_      = 0;

    // Post-defrost suppression (HAL + Rate alarm)
    bool     was_defrost_active_       = false;
    bool     post_defrost_suppression_ = false;
    uint32_t post_defrost_timer_ms_    = 0;
    uint32_t post_defrost_delay_ms_    = 1800000;  // 30 хв default

    // Кешований код аварії
    const char* alarm_code_ = "none";
};
