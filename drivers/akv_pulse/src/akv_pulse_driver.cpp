/**
 * @file akv_pulse_driver.cpp
 * @brief PWM solenoid valve driver implementation
 *
 * Software PWM at very low frequency (0.167 Hz = 6 second cycle).
 * LEDC hardware timer could be used but is overkill for 6s cycle —
 * software timing in update() is simpler and adequate.
 */

#include "akv_pulse_driver.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char TAG[] = "AkvPulse";

namespace modesp {

AkvPulseDriver::AkvPulseDriver(const char* role, int gpio, uint32_t cycle_ms,
                               uint8_t min_duty_pct)
    : role_(role)
    , gpio_(gpio)
    , cycle_ms_(cycle_ms > 0 ? cycle_ms : 6000)
    , min_duty_pct_(min_duty_pct)
{}

bool AkvPulseDriver::init() {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << gpio_);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] GPIO %d config failed: %s", role_, gpio_, esp_err_to_name(err));
        return false;
    }

    set_output(false);
    position_ = 0;
    initialized_ = true;

    ESP_LOGI(TAG, "[%s] Initialized (GPIO=%d, cycle=%lums, min_duty=%u%%)",
             role_, gpio_, cycle_ms_, min_duty_pct_);
    return true;
}

void AkvPulseDriver::update(uint32_t dt_ms) {
    if (!initialized_) return;

    // Position 0 = always OFF
    if (position_ == 0) {
        if (output_state_) set_output(false);
        cycle_timer_ms_ = 0;
        return;
    }

    // Position 1000 = always ON (100%)
    if (position_ >= 1000) {
        if (!output_state_) set_output(true);
        cycle_timer_ms_ = 0;
        return;
    }

    // Calculate ON time within cycle
    // position_ 0..1000 maps to 0..100% duty
    uint32_t on_time_ms = (static_cast<uint32_t>(position_) * cycle_ms_) / 1000;

    // Apply minimum duty — below min_duty, valve is OFF
    uint32_t min_on_ms = (static_cast<uint32_t>(min_duty_pct_) * cycle_ms_) / 100;
    if (on_time_ms < min_on_ms) {
        if (output_state_) set_output(false);
        cycle_timer_ms_ = 0;
        return;
    }

    // Software PWM
    cycle_timer_ms_ += dt_ms;
    if (cycle_timer_ms_ >= cycle_ms_) {
        cycle_timer_ms_ = 0;  // Reset cycle
    }

    bool should_be_on = (cycle_timer_ms_ < on_time_ms);
    if (should_be_on != output_state_) {
        set_output(should_be_on);
    }
}

bool AkvPulseDriver::set_position(uint16_t target) {
    if (!initialized_) return false;
    if (target > 1000) target = 1000;
    position_ = target;
    return true;
}

void AkvPulseDriver::emergency_close() {
    position_ = 0;
    set_output(false);
    cycle_timer_ms_ = 0;
    ESP_LOGW(TAG, "[%s] Emergency close — solenoid OFF", role_);
}

void AkvPulseDriver::set_output(bool on) {
    gpio_set_level(static_cast<gpio_num_t>(gpio_), on ? 1 : 0);
    output_state_ = on;
}

} // namespace modesp
