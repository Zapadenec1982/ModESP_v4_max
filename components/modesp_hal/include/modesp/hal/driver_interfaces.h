/**
 * @file driver_interfaces.h
 * @brief Abstract interfaces for sensor and actuator drivers
 *
 * Business logic modules (thermostat, alarm, etc.) interact
 * with hardware through these interfaces only — never
 * accessing GPIO or bus protocols directly.
 *
 * Concrete drivers (DS18B20Driver, RelayDriver, etc.)
 * implement these interfaces and are created by DriverManager.
 */

#pragma once

#include <cstdint>

namespace modesp {

// ═══════════════════════════════════════════════════════════════
// ISensorDriver — read-only hardware input
// ═══════════════════════════════════════════════════════════════

class ISensorDriver {
public:
    virtual ~ISensorDriver() = default;

    virtual bool init() = 0;
    virtual void update(uint32_t dt_ms) = 0;

    /// Read latest valid value. Returns true if a valid reading exists.
    virtual bool read(float& value) = 0;

    virtual bool is_healthy() const = 0;
    virtual const char* role() const = 0;
    virtual const char* type() const = 0;
    virtual uint32_t error_count() const { return 0; }
};

// ═══════════════════════════════════════════════════════════════
// IActuatorDriver — controllable hardware output
// ═══════════════════════════════════════════════════════════════

class IActuatorDriver {
public:
    virtual ~IActuatorDriver() = default;

    virtual bool init() = 0;
    virtual void update(uint32_t dt_ms) = 0;

    // Discrete control
    virtual bool set(bool state) = 0;
    virtual bool get_state() const = 0;

    // Analog control — default maps to discrete
    virtual bool set_value(float value_0_1) { return set(value_0_1 > 0.5f); }
    virtual float get_value() const { return get_state() ? 1.0f : 0.0f; }
    virtual bool supports_analog() const { return false; }

    virtual const char* role() const = 0;
    virtual const char* type() const = 0;
    virtual bool is_healthy() const = 0;

    virtual void emergency_stop() { set(false); }
    virtual uint32_t switch_count() const { return 0; }
};

// ═══════════════════════════════════════════════════════════════
// IValveDriver — electronic expansion valve control
// ═══════════════════════════════════════════════════════════════
//
// Universal interface for ALL EEV types:
//   - Bipolar stepper (Carel E2V, Danfoss ETS, Emerson EX, Sporlan SEI)
//   - Unipolar stepper (Sanhua DPF, Fujikoki UKV)
//   - PWM solenoid (Danfoss AKV, Emerson EX2)
//   - 0-10V analog positioner (via Carel EVD Mini, Danfoss EKF)
//   - 4-20mA analog positioner
//
// Position units: 0 = fully closed, max_steps() = fully open
// For PWM/analog types: max_steps = 1000 (0.1% resolution)

class IValveDriver : public IActuatorDriver {
public:
    // ── Position control (primary API) ──

    /// Move valve to target position (0..max_steps)
    virtual bool set_position(uint16_t target) = 0;

    /// Get current valve position (0..max_steps)
    virtual uint16_t get_position() const = 0;

    /// Maximum position value (valve-specific: 480, 600, 750, 1596, 6386, etc.)
    virtual uint16_t max_steps() const = 0;

    // ── Calibration ──

    /// Has the valve been calibrated (homing done)?
    virtual bool is_calibrated() const = 0;

    /// Run calibration/homing procedure (blocks or starts async)
    /// For stepper: overdrive to closed position, reset counter to 0
    /// For PWM/analog: no calibration needed, always returns true
    virtual bool calibrate() = 0;

    // ── Safety ──

    /// Emergency close at maximum speed (stepper: 2-3x normal rate)
    void emergency_stop() override { emergency_close(); }

    /// Emergency close — valve-specific fast close
    virtual void emergency_close() = 0;

    // ── IActuatorDriver compatibility ──

    /// Maps float 0.0-1.0 to position 0..max_steps
    bool set_value(float value_0_1) override {
        uint16_t pos = static_cast<uint16_t>(value_0_1 * max_steps());
        return set_position(pos);
    }

    float get_value() const override {
        if (max_steps() == 0) return 0.0f;
        return static_cast<float>(get_position()) / static_cast<float>(max_steps());
    }

    bool set(bool state) override {
        return state ? set_position(max_steps()) : set_position(0);
    }

    bool get_state() const override { return get_position() > 0; }

    bool supports_analog() const override { return true; }
};

} // namespace modesp
