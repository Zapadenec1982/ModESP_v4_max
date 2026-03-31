#pragma once
// ═══════════════════════════════════════════════════════════════
//  PressureAdcDriver — Ratiometric pressure sensor via ADC
//
//  Supports: Carel SPKT (0.5-4.5V), generic 0-5V / 0-10V sensors.
//  KC868-A6 ADC: 0-3.3V after LM224 scaling (5V input → 3.3V ADC).
//
//  Conversion: P = (V - V_min) / (V_max - V_min) * P_max + offset
//  Diagnostics: open circuit (<V_min*0.6), short circuit (>V_max*1.05)
//  IIR filter for ADC noise suppression near compressor.
// ═══════════════════════════════════════════════════════════════

#include "modesp/hal/driver_interfaces.h"
#include <etl/string.h>

#ifndef HOST_BUILD
  #include "esp_adc/adc_oneshot.h"
  #include "hal/adc_types.h"
#endif

namespace modesp {

class PressureAdcDriver : public ISensorDriver {
public:
    PressureAdcDriver() = default;

    /// Configure from DriverManager (after construction, before init)
    void configure(const char* role, int gpio, uint8_t atten);

    // ─── ISensorDriver interface ───────────────────────────
    bool init() override;
    void update(uint32_t dt_ms) override;
    bool read(float& value) override;
    bool is_healthy() const override;
    const char* role() const override { return role_.c_str(); }
    const char* type() const override { return "pressure_adc"; }
    uint32_t error_count() const override { return total_errors_; }

    // ─── Diagnostics ───────────────────────────────────────
    enum class Status : uint8_t {
        OK           = 0,
        OPEN_CIRCUIT = 1,   // Wire broken or sensor disconnected
        SHORT_CIRCUIT = 2,  // Sensor shorted
        OUT_OF_RANGE = 3,   // Pressure outside valid range
        NOT_READY    = 4,   // No valid reading yet
    };

    Status status() const { return status_; }
    float raw_voltage() const { return raw_voltage_; }

private:
#ifndef HOST_BUILD
    static bool gpio_to_adc1_channel(int gpio, adc_channel_t& out);
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_channel_t adc_channel_ = ADC_CHANNEL_0;
#endif

    etl::string<20> role_;
    int gpio_ = 0;
    uint8_t atten_ = 3;  // 11dB default (0-3.3V)
    bool configured_ = false;

    // ─── Sensor parameters (from manifest defaults) ────────
    float v_min_ = 0.5f;          // V at 0 bar (ratiometric offset)
    float v_max_ = 4.5f;          // V at max pressure
    float max_pressure_ = 50.0f;  // Full scale (bar)
    float offset_ = 0.0f;         // Calibration offset (bar)
    float filter_alpha_ = 0.15f;  // IIR filter coefficient

    // ─── ADC scaling ───────────────────────────────────────
    // KC868-A6: LM224 scales 0-5V → 0-3.3V for ESP32 ADC.
    // Scale factor: 3.3/5.0 = 0.66. Inverse: 5.0/3.3 = 1.5152
    static constexpr float ADC_VREF = 3.3f;
    static constexpr int ADC_MAX_RAW = 4095;
    static constexpr float INPUT_SCALE = 5.0f / 3.3f;  // KC868-A6 LM224 inverse

    // ─── Diagnostics thresholds ────────────────────────────
    static constexpr int ADC_OPEN_THRESHOLD = 100;    // < this = open circuit (~0.3V sensor, industry standard)
    static constexpr int ADC_SHORT_THRESHOLD = 3850; // > this = short circuit (~4.7V sensor, industry standard)
    static constexpr uint8_t MAX_CONSECUTIVE_ERRORS = 5;

    // ─── State ─────────────────────────────────────────────
    float current_pressure_ = 0.0f;
    float filtered_pressure_ = 0.0f;
    float raw_voltage_ = 0.0f;
    bool has_valid_ = false;
    bool filter_init_ = false;
    uint32_t ms_since_read_ = 0;
    uint32_t read_interval_ms_ = 500;
    uint8_t consecutive_errors_ = 0;
    uint32_t total_errors_ = 0;
    Status status_ = Status::NOT_READY;
};

}  // namespace modesp
