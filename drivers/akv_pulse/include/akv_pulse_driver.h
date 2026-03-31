/**
 * @file akv_pulse_driver.h
 * @brief PWM solenoid valve driver for Danfoss AKV and similar
 *
 * Controls PWM-type solenoid expansion valves (Danfoss AKV 10P/15/20, Emerson EX2).
 * These valves modulate flow by varying the ON duty cycle within a fixed period.
 *
 * Typical: 6-second cycle, duty 10-100%, 24V DC coil via MOSFET + flyback diode.
 * Fail-safe: solenoid closes when power is removed (no Ultracap needed).
 *
 * Position = duty cycle: 0 = closed (0%), max_steps (1000) = fully open (100%)
 */

#pragma once

#include "modesp/hal/driver_interfaces.h"

namespace modesp {

class AkvPulseDriver : public IValveDriver {
public:
    /// Default constructor for pool allocation
    AkvPulseDriver() : role_(""), gpio_(-1), cycle_ms_(6000), min_duty_pct_(10) {}

    /// @param role         Driver role name (e.g., "akv_z1")
    /// @param gpio         GPIO pin for MOSFET gate
    /// @param cycle_ms     PWM cycle period in ms (default 6000 = 6 seconds)
    /// @param min_duty_pct Minimum duty cycle % below which valve is OFF (default 10)
    AkvPulseDriver(const char* role, int gpio, uint32_t cycle_ms = 6000,
                   uint8_t min_duty_pct = 10);

    /// Configure after pool allocation (used by DriverManager)
    void configure(const char* role, int gpio, uint32_t cycle_ms = 6000,
                   uint8_t min_duty_pct = 10) {
        role_ = role; gpio_ = gpio;
        cycle_ms_ = cycle_ms > 0 ? cycle_ms : 6000;
        min_duty_pct_ = min_duty_pct;
    }

    // ── IValveDriver ──
    bool     set_position(uint16_t target) override;
    uint16_t get_position() const override { return position_; }
    uint16_t max_steps() const override { return 1000; }  // 0.1% resolution
    bool     is_calibrated() const override { return true; }
    bool     calibrate() override { return true; }
    void     emergency_close() override;

    // ── IActuatorDriver ──
    bool init() override;
    void update(uint32_t dt_ms) override;
    const char* role() const override { return role_; }
    const char* type() const override { return "akv_pulse"; }
    bool is_healthy() const override { return initialized_; }

private:
    const char* role_;
    int         gpio_;
    uint32_t    cycle_ms_;
    uint8_t     min_duty_pct_;

    uint16_t    position_ = 0;       // 0..1000
    bool        initialized_ = false;

    // Software PWM state (LEDC timer for slow PWM)
    uint32_t    cycle_timer_ms_ = 0;
    bool        output_state_ = false;

    void set_output(bool on);
};

} // namespace modesp
