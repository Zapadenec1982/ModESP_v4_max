/**
 * @file eev_analog_driver.cpp
 * @brief DAC 0-10V valve driver implementation
 */

#include "eev_analog_driver.h"
#include "esp_log.h"
#include "driver/dac_oneshot.h"

static const char TAG[] = "EevAnalog";

namespace modesp {

EevAnalogDriver::EevAnalogDriver(const char* role, int dac_gpio, uint16_t max_steps)
    : role_(role)
    , dac_gpio_(dac_gpio)
    , max_steps_(max_steps > 0 ? max_steps : 255)
{}

bool EevAnalogDriver::init() {
    // Validate GPIO — ESP32 DAC only on GPIO25 (DAC1) and GPIO26 (DAC2)
    if (dac_gpio_ != 25 && dac_gpio_ != 26) {
        ESP_LOGE(TAG, "[%s] Invalid DAC GPIO %d (must be 25 or 26)", role_, dac_gpio_);
        return false;
    }

    // Init DAC handle once (zero-heap: не створюємо/знищуємо кожен write)
    dac_channel_t channel = (dac_gpio_ == 25) ? DAC_CHAN_0 : DAC_CHAN_1;
    dac_oneshot_config_t cfg = { .chan_id = channel };
    esp_err_t err = dac_oneshot_new_channel(&cfg, &dac_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] DAC init failed: %s", role_, esp_err_to_name(err));
        return false;
    }

    // Set initial position to 0 (closed)
    write_dac(0);
    position_ = 0;
    initialized_ = true;

    ESP_LOGI(TAG, "[%s] Initialized (GPIO=%d, max_steps=%u, 0-10V output)",
             role_, dac_gpio_, max_steps_);
    return true;
}

void EevAnalogDriver::update(uint32_t dt_ms) {
    (void)dt_ms;
    // No periodic action needed — DAC holds value
}

bool EevAnalogDriver::set_position(uint16_t target) {
    if (!initialized_) return false;
    if (target > max_steps_) target = max_steps_;

    position_ = target;

    // Map position to DAC value (0-255)
    uint8_t dac_val = static_cast<uint8_t>(
        (static_cast<uint32_t>(target) * 255) / max_steps_);
    write_dac(dac_val);
    return true;
}

void EevAnalogDriver::emergency_close() {
    position_ = 0;
    write_dac(0);
    ESP_LOGW(TAG, "[%s] Emergency close — DAC=0", role_);
}

void EevAnalogDriver::write_dac(uint8_t value) {
    if (dac_handle_) {
        dac_oneshot_output_voltage(dac_handle_, value);
    }
}

} // namespace modesp
