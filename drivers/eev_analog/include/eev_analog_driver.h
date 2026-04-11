/**
 * @file eev_analog_driver.h
 * @brief DAC 0-10V valve driver for analog-controlled EEV positioners
 *
 * Controls EEV valves through an external positioner driver (Carel EVD Mini,
 * Danfoss EKF) via 0-10V analog output from ESP32 DAC.
 *
 * Signal chain: ESP32 DAC (8-bit) → LM258 op-amp → 0-10V → EVD Mini → E2V valve
 *
 * Simplest EEV driver — no stepper control, homing, or NVS position save.
 * The external positioner handles valve mechanics and power-loss protection.
 */

#pragma once

#include "modesp/hal/driver_interfaces.h"
#include "esp_err.h"
#ifndef HOST_BUILD
#include "driver/dac_oneshot.h"
#endif

namespace modesp {

class EevAnalogDriver : public IValveDriver {
public:
    /// Default constructor for pool allocation
    EevAnalogDriver() : role_(""), dac_gpio_(-1), max_steps_(255) {}

    /// @param role       Driver role name (e.g., "eev_z1")
    /// @param dac_gpio   GPIO pin for DAC output (ESP32: GPIO25 or GPIO26)
    /// @param max_steps  Logical resolution (default 255 for 8-bit DAC)
    EevAnalogDriver(const char* role, int dac_gpio, uint16_t max_steps = 255);

    /// Configure after pool allocation (used by DriverManager)
    void configure(const char* role, int dac_gpio, uint16_t max_steps = 255) {
        role_ = role;
        dac_gpio_ = dac_gpio;
        max_steps_ = max_steps > 0 ? max_steps : 255;
    }

    // ── IValveDriver ──
    bool     set_position(uint16_t target) override;
    uint16_t get_position() const override { return position_; }
    uint16_t max_steps() const override { return max_steps_; }
    bool     is_calibrated() const override { return true; }  // No calibration needed
    bool     calibrate() override { return true; }
    void     emergency_close() override;

    // ── IActuatorDriver ──
    bool init() override;
    void update(uint32_t dt_ms) override;
    const char* role() const override { return role_; }
    const char* type() const override { return "eev_analog"; }
    bool is_healthy() const override { return initialized_; }

private:
    const char* role_;
    int         dac_gpio_;
    uint16_t    max_steps_;
    uint16_t    position_ = 0;
    bool        initialized_ = false;

#ifndef HOST_BUILD
    dac_oneshot_handle_t dac_handle_ = nullptr;
#endif

    /// Write DAC value (0-255) to GPIO
    void write_dac(uint8_t value);
};

} // namespace modesp
