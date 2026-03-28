/**
 * @file eev_pcf8574_stepper_driver.h
 * @brief EEV stepper driver via PCF8574 I2C expander (STEP + DIR)
 *
 * Controls bipolar stepper EEV valves (Carel E2V, Danfoss ETS, etc.)
 * through 2 pins of PCF8574 I2C expander acting as STEP and DIR outputs.
 *
 * Hardware modification: Remove EL357 optocouplers from DIN positions,
 * PCF8574 pins become direct 3.3V outputs → connect to H-bridge (L298N).
 *
 * Timing: I2C @ 100kHz → ~0.5ms per write → max ~100Hz step rate.
 * E2V requires 50Hz → well within limits.
 *
 * Non-blocking: step generation happens in update() with timing control.
 * NVS position persistence for power-loss recovery.
 */

#pragma once

#include "modesp/hal/driver_interfaces.h"
#include "modesp/hal/hal_types.h"
#include "etl/string.h"

namespace modesp {

class EevPcf8574StepperDriver : public IValveDriver {
public:
    /// Configure driver with PCF8574 expander and pin assignments
    /// @param role       Driver role (e.g., "eev_z1")
    /// @param expander   Shared PCF8574 I2C expander resource
    /// @param step_pin   Bit index (0-7) for STEP signal
    /// @param dir_pin    Bit index (0-7) for DIR signal
    /// @param max_steps  Valve total steps (480 for E2V, 600 for ETS, etc.)
    /// @param nvs_ns     NVS namespace for position persistence (nullptr = no persist)
    void configure(const char* role, I2CExpanderResource* expander,
                   uint8_t step_pin, uint8_t dir_pin,
                   uint16_t max_steps = 480, const char* nvs_ns = nullptr);

    // ── IValveDriver ──
    bool     set_position(uint16_t target) override;
    uint16_t get_position() const override { return current_pos_; }
    uint16_t max_steps() const override { return max_steps_; }
    bool     is_calibrated() const override { return calibrated_; }
    bool     calibrate() override;
    void     emergency_close() override;

    // ── IActuatorDriver ──
    bool init() override;
    void update(uint32_t dt_ms) override;
    const char* role() const override { return role_.c_str(); }
    const char* type() const override { return "eev_pcf8574_stepper"; }
    bool is_healthy() const override { return initialized_ && expander_ != nullptr; }

private:
    etl::string<16> role_;
    I2CExpanderResource* expander_ = nullptr;
    uint8_t  step_pin_  = 0;
    uint8_t  dir_pin_   = 0;
    uint16_t max_steps_ = 480;
    const char* nvs_ns_ = nullptr;

    uint16_t current_pos_ = 0;
    uint16_t target_pos_  = 0;
    bool     initialized_ = false;
    bool     configured_  = false;
    bool     calibrated_  = false;
    bool     emergency_   = false;

    // Step timing (non-blocking)
    // Normal: 50Hz = 20ms per step. Emergency: 150Hz = 6.7ms per step.
    static constexpr uint32_t STEP_INTERVAL_MS      = 20;   // 50Hz
    static constexpr uint32_t EMERGENCY_INTERVAL_MS  = 7;    // ~150Hz
    static constexpr uint16_t HOMING_EXTRA_STEPS     = 50;   // overdrive for homing

    uint32_t step_timer_ms_ = 0;
    bool     step_phase_    = false;  // Toggle for STEP pulse

    // Homing state
    enum class HomingState : uint8_t { IDLE, CLOSING, DONE };
    HomingState homing_state_ = HomingState::IDLE;
    uint16_t   homing_remaining_ = 0;

    /// Generate one step pulse via PCF8574
    void do_step();

    /// Set DIR pin on PCF8574
    void set_dir(bool open_direction);

    /// Save position to NVS
    void save_position();

    /// Load position from NVS
    bool load_position();
};

} // namespace modesp
