/**
 * @file eev_stepper_driver.h
 * @brief Bipolar/unipolar stepper valve driver for direct EEV control
 *
 * Drives EEV valves directly via stepper motor driver IC (TMC2209, DRV8825, A4988).
 * Supports bipolar (4-wire: Carel E2V, Danfoss ETS, Emerson EX, Sporlan SEI)
 * and unipolar (5/6-wire: Sanhua DPF, Fujikoki UKV) valves.
 *
 * Features:
 *   - Non-blocking step generation in update() (~50Hz = 20ms per step)
 *   - Homing procedure: overdrive to closed position, reset counter
 *   - NVS position persistence: save on stop, restore on boot
 *   - Emergency close at 2-3x normal rate
 *   - Configurable: max_steps, drive_freq, hold_current reduction
 *
 * Hardware: STEP + DIR + ENABLE GPIO pins → stepper driver IC → valve motor
 * Supply: 12-24V DC depending on valve (NOT from ESP32!)
 */

#pragma once

#include "modesp/hal/driver_interfaces.h"

namespace modesp {

struct EevStepperConfig {
    int      step_gpio;            ///< STEP pin
    int      dir_gpio;             ///< DIR pin
    int      enable_gpio;          ///< ENABLE pin (-1 if not used)
    uint16_t max_steps;            ///< Valve total steps (480, 600, 750, 1596, etc.)
    uint16_t homing_extra_steps;   ///< Extra steps for homing overdrive (default 50)
    uint16_t drive_freq_hz;        ///< Normal drive frequency (default 50)
    uint16_t emergency_freq_hz;    ///< Emergency close frequency (default 150)
    bool     invert_dir;           ///< Invert DIR pin logic
    const char* nvs_namespace;     ///< NVS namespace for position persistence (nullptr = no persist)
};

class EevStepperDriver : public IValveDriver {
public:
    /// Default constructor for pool allocation
    EevStepperDriver() : role_(""), cfg_{} {}

    EevStepperDriver(const char* role, const EevStepperConfig& cfg);

    // ── IValveDriver ──
    bool     set_position(uint16_t target) override;
    uint16_t get_position() const override { return current_pos_; }
    uint16_t max_steps() const override { return cfg_.max_steps; }
    bool     is_calibrated() const override { return calibrated_; }
    bool     calibrate() override;
    void     emergency_close() override;

    // ── IActuatorDriver ──
    bool init() override;
    void update(uint32_t dt_ms) override;
    const char* role() const override { return role_; }
    const char* type() const override { return "eev_stepper"; }
    bool is_healthy() const override { return initialized_; }

private:
    const char*      role_;
    EevStepperConfig cfg_;

    uint16_t current_pos_ = 0;
    uint16_t target_pos_ = 0;
    bool     initialized_ = false;
    bool     calibrated_ = false;
    bool     emergency_mode_ = false;

    // Non-blocking step timing
    uint32_t step_interval_us_ = 0;    // Normal step interval (1/freq)
    uint32_t emergency_interval_us_ = 0;
    uint32_t step_timer_us_ = 0;

    // Homing state
    enum class HomingState : uint8_t {
        IDLE, CLOSING, DONE
    };
    HomingState homing_state_ = HomingState::IDLE;
    uint16_t    homing_steps_remaining_ = 0;

    void step_one(bool dir_open);
    void set_enable(bool enabled);
    void save_position_nvs();
    bool load_position_nvs();
};

} // namespace modesp
