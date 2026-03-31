// ═══════════════════════════════════════════════════════════════
//  PressureAdcDriver — Ratiometric pressure sensor via ADC
//
//  P = (V_input - V_min) / (V_max - V_min) * P_max + offset
//
//  KC868-A6 ADC chain: Sensor 0-5V → LM224 0.66x → ESP32 ADC 0-3.3V
//  SPKT00G1S0: 0.5-4.5V ratiometric, 0-50 bar (or 0-60 bar)
// ═══════════════════════════════════════════════════════════════

#include "pressure_adc_driver.h"

#ifndef HOST_BUILD
  #include "esp_log.h"
  #include "modesp/hal/adc_shared.h"
#else
  #include "esp_log.h"  // mock
#endif

#include <cmath>

static const char TAG[] = "PressureADC";

namespace modesp {

void PressureAdcDriver::configure(const char* role, int gpio, uint8_t atten) {
    role_ = role;
    gpio_ = gpio;
    atten_ = atten;
    configured_ = true;
}

bool PressureAdcDriver::init() {
    if (!configured_) {
        ESP_LOGE(TAG, "Not configured — call configure() first");
        return false;
    }

#ifndef HOST_BUILD
    // Shared ADC1 handle (єдиний для NTC + PressureAdc + future ADC drivers)
    adc_handle_ = get_shared_adc1_handle();
    if (!adc_handle_) {
        ESP_LOGE(TAG, "[%s] Failed to get shared ADC1 handle", role_.c_str());
        return false;
    }

    // Map GPIO to ADC1 channel
    if (!gpio_to_adc1_channel(gpio_, adc_channel_)) {
        ESP_LOGE(TAG, "[%s] GPIO %d is not a valid ADC1 pin", role_.c_str(), gpio_);
        return false;
    }

    // Configure channel
    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    chan_cfg.atten = static_cast<adc_atten_t>(atten_);
    err = adc_oneshot_config_channel(adc_handle_, adc_channel_, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] ADC channel config failed: %s", role_.c_str(), esp_err_to_name(err));
        return false;
    }
#endif

    ESP_LOGI(TAG, "[%s] Initialized (GPIO=%d, Pmax=%.0f bar, V=%.1f-%.1f, α=%.2f)",
             role_.c_str(), gpio_, max_pressure_, v_min_, v_max_, filter_alpha_);
    return true;
}

void PressureAdcDriver::update(uint32_t dt_ms) {
#ifndef HOST_BUILD
    if (!adc_handle_) return;
#endif

    ms_since_read_ += dt_ms;
    if (ms_since_read_ < read_interval_ms_) return;
    ms_since_read_ = 0;

#ifndef HOST_BUILD
    // Read raw ADC value
    int raw = 0;
    esp_err_t err = adc_oneshot_read(adc_handle_, adc_channel_, &raw);
    if (err != ESP_OK) {
        consecutive_errors_++;
        total_errors_++;
        ESP_LOGW(TAG, "[%s] ADC read failed: %s", role_.c_str(), esp_err_to_name(err));
        return;
    }

    // ── Diagnostics: open circuit / short circuit ──────────
    if (raw < ADC_OPEN_THRESHOLD) {
        consecutive_errors_++;
        total_errors_++;
        if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS && status_ != Status::OPEN_CIRCUIT) {
            status_ = Status::OPEN_CIRCUIT;
            has_valid_ = false;
            ESP_LOGE(TAG, "[%s] OPEN CIRCUIT (ADC=%d)", role_.c_str(), raw);
        }
        return;
    }
    if (raw > ADC_SHORT_THRESHOLD) {
        consecutive_errors_++;
        total_errors_++;
        if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS && status_ != Status::SHORT_CIRCUIT) {
            status_ = Status::SHORT_CIRCUIT;
            has_valid_ = false;
            ESP_LOGE(TAG, "[%s] SHORT CIRCUIT (ADC=%d)", role_.c_str(), raw);
        }
        return;
    }

    // ── Convert raw → voltage (at sensor input, before LM224) ──
    float v_adc = static_cast<float>(raw) / ADC_MAX_RAW * ADC_VREF;
    raw_voltage_ = v_adc * INPUT_SCALE;  // Reconstruct original 0-5V signal

    // ── Convert voltage → pressure (linear ratiometric) ────
    float v_range = v_max_ - v_min_;
    if (v_range < 0.1f) {
        status_ = Status::OUT_OF_RANGE;
        return;
    }

    float pressure = (raw_voltage_ - v_min_) / v_range * max_pressure_ + offset_;

    // ── Validate range ─────────────────────────────────────
    if (pressure < -1.0f || pressure > max_pressure_ * 1.1f) {
        consecutive_errors_++;
        total_errors_++;
        if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS && status_ != Status::OUT_OF_RANGE) {
            status_ = Status::OUT_OF_RANGE;
            ESP_LOGW(TAG, "[%s] Out of range: %.1f bar (V=%.2f)", role_.c_str(), pressure, raw_voltage_);
        }
        return;
    }

    // ── IIR low-pass filter ────────────────────────────────
    if (!filter_init_) {
        filtered_pressure_ = pressure;
        filter_init_ = true;
    } else {
        filtered_pressure_ += (pressure - filtered_pressure_) * filter_alpha_;
    }

    current_pressure_ = roundf(filtered_pressure_ * 100.0f) / 100.0f;  // 0.01 bar resolution
    has_valid_ = true;
    consecutive_errors_ = 0;
    status_ = Status::OK;

    ESP_LOGD(TAG, "[%s] %.2f bar (V=%.2f, ADC=%d)", role_.c_str(), current_pressure_, raw_voltage_, raw);
#endif  // HOST_BUILD
}

bool PressureAdcDriver::read(float& value) {
    if (!has_valid_) return false;
    value = current_pressure_;
    return true;
}

bool PressureAdcDriver::is_healthy() const {
    return has_valid_ && consecutive_errors_ < MAX_CONSECUTIVE_ERRORS;
}

#ifndef HOST_BUILD
bool PressureAdcDriver::gpio_to_adc1_channel(int gpio, adc_channel_t& out) {
    switch (gpio) {
        case 36: out = ADC_CHANNEL_0; return true;
        case 37: out = ADC_CHANNEL_1; return true;
        case 38: out = ADC_CHANNEL_2; return true;
        case 39: out = ADC_CHANNEL_3; return true;
        case 32: out = ADC_CHANNEL_4; return true;
        case 33: out = ADC_CHANNEL_5; return true;
        case 34: out = ADC_CHANNEL_6; return true;
        case 35: out = ADC_CHANNEL_7; return true;
        default: return false;
    }
}
#endif

}  // namespace modesp
