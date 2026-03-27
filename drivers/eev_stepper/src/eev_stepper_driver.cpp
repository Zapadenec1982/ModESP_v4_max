/**
 * @file eev_stepper_driver.cpp
 * @brief Bipolar/unipolar stepper EEV driver implementation
 */

#include "eev_stepper_driver.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char TAG[] = "EevStepper";

namespace modesp {

EevStepperDriver::EevStepperDriver(const char* role, const EevStepperConfig& cfg)
    : role_(role)
    , cfg_(cfg)
{
    // Calculate step intervals from frequency
    if (cfg_.drive_freq_hz > 0) {
        step_interval_us_ = 1000000 / cfg_.drive_freq_hz;
    } else {
        step_interval_us_ = 20000;  // 50Hz default
    }
    if (cfg_.emergency_freq_hz > 0) {
        emergency_interval_us_ = 1000000 / cfg_.emergency_freq_hz;
    } else {
        emergency_interval_us_ = step_interval_us_ / 3;  // 3x normal
    }
}

bool EevStepperDriver::init() {
    // Configure GPIO pins
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    // STEP pin
    io_conf.pin_bit_mask = (1ULL << cfg_.step_gpio);
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] STEP GPIO %d config failed", role_, cfg_.step_gpio);
        return false;
    }

    // DIR pin
    io_conf.pin_bit_mask = (1ULL << cfg_.dir_gpio);
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] DIR GPIO %d config failed", role_, cfg_.dir_gpio);
        return false;
    }

    // ENABLE pin (optional)
    if (cfg_.enable_gpio >= 0) {
        io_conf.pin_bit_mask = (1ULL << cfg_.enable_gpio);
        if (gpio_config(&io_conf) != ESP_OK) {
            ESP_LOGE(TAG, "[%s] ENABLE GPIO %d config failed", role_, cfg_.enable_gpio);
            return false;
        }
        set_enable(false);  // Disabled initially
    }

    // Try to restore position from NVS
    bool pos_restored = load_position_nvs();
    if (pos_restored) {
        ESP_LOGI(TAG, "[%s] Position restored from NVS: %u/%u",
                 role_, current_pos_, cfg_.max_steps);
        calibrated_ = true;  // Trust NVS position
    } else {
        ESP_LOGI(TAG, "[%s] No saved position — homing required", role_);
        current_pos_ = 0;
        calibrated_ = false;
    }

    target_pos_ = current_pos_;
    initialized_ = true;

    ESP_LOGI(TAG, "[%s] Initialized (STEP=%d DIR=%d EN=%d max=%u freq=%uHz)",
             role_, cfg_.step_gpio, cfg_.dir_gpio, cfg_.enable_gpio,
             cfg_.max_steps, cfg_.drive_freq_hz);
    return true;
}

void EevStepperDriver::update(uint32_t dt_ms) {
    if (!initialized_) return;

    uint32_t dt_us = dt_ms * 1000;

    // Homing state machine
    if (homing_state_ == HomingState::CLOSING) {
        step_timer_us_ += dt_us;
        while (step_timer_us_ >= step_interval_us_ && homing_steps_remaining_ > 0) {
            step_one(false);  // Close direction
            homing_steps_remaining_--;
            step_timer_us_ -= step_interval_us_;
        }
        if (homing_steps_remaining_ == 0) {
            current_pos_ = 0;
            target_pos_ = 0;
            calibrated_ = true;
            homing_state_ = HomingState::DONE;
            save_position_nvs();
            ESP_LOGI(TAG, "[%s] Homing complete — position=0", role_);
        }
        return;
    }

    // Normal operation — move towards target
    if (current_pos_ == target_pos_) return;

    uint32_t interval = emergency_mode_ ? emergency_interval_us_ : step_interval_us_;
    step_timer_us_ += dt_us;

    while (step_timer_us_ >= interval && current_pos_ != target_pos_) {
        bool opening = target_pos_ > current_pos_;
        step_one(opening);
        if (opening) {
            current_pos_++;
        } else {
            if (current_pos_ > 0) current_pos_--;
        }
        step_timer_us_ -= interval;
    }

    // Save position when target reached
    if (current_pos_ == target_pos_) {
        emergency_mode_ = false;
        save_position_nvs();
    }
}

bool EevStepperDriver::set_position(uint16_t target) {
    if (!initialized_) return false;
    if (target > cfg_.max_steps) target = cfg_.max_steps;
    if (!calibrated_) {
        ESP_LOGW(TAG, "[%s] Not calibrated — homing first", role_);
        return false;
    }

    target_pos_ = target;
    if (cfg_.enable_gpio >= 0) set_enable(true);
    return true;
}

bool EevStepperDriver::calibrate() {
    if (!initialized_) return false;

    ESP_LOGI(TAG, "[%s] Starting homing (%u + %u extra steps)",
             role_, cfg_.max_steps, cfg_.homing_extra_steps);

    if (cfg_.enable_gpio >= 0) set_enable(true);

    homing_steps_remaining_ = cfg_.max_steps + cfg_.homing_extra_steps;
    homing_state_ = HomingState::CLOSING;
    step_timer_us_ = 0;
    return true;
}

void EevStepperDriver::emergency_close() {
    if (!initialized_) return;

    target_pos_ = 0;
    emergency_mode_ = true;
    if (cfg_.enable_gpio >= 0) set_enable(true);
    ESP_LOGW(TAG, "[%s] Emergency close — fast mode (%uHz)",
             role_, cfg_.emergency_freq_hz);
}

void EevStepperDriver::step_one(bool dir_open) {
    // Set direction
    bool dir_level = dir_open;
    if (cfg_.invert_dir) dir_level = !dir_level;
    gpio_set_level(static_cast<gpio_num_t>(cfg_.dir_gpio), dir_level ? 1 : 0);

    // Pulse STEP pin (minimum 2µs pulse width for most drivers)
    gpio_set_level(static_cast<gpio_num_t>(cfg_.step_gpio), 1);
    // Busy-wait 5µs — acceptable in update() context
    volatile int delay = 40;  // ~5µs at 160MHz
    while (delay--) {}
    gpio_set_level(static_cast<gpio_num_t>(cfg_.step_gpio), 0);
}

void EevStepperDriver::set_enable(bool enabled) {
    if (cfg_.enable_gpio < 0) return;
    // Most stepper drivers: ENABLE active LOW
    gpio_set_level(static_cast<gpio_num_t>(cfg_.enable_gpio), enabled ? 0 : 1);
}

void EevStepperDriver::save_position_nvs() {
    if (!cfg_.nvs_namespace) return;

    nvs_handle_t handle;
    if (nvs_open(cfg_.nvs_namespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u16(handle, "pos", current_pos_);
        nvs_set_u8(handle, "cal", calibrated_ ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool EevStepperDriver::load_position_nvs() {
    if (!cfg_.nvs_namespace) return false;

    nvs_handle_t handle;
    if (nvs_open(cfg_.nvs_namespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint16_t pos = 0;
    uint8_t cal = 0;
    bool ok = (nvs_get_u16(handle, "pos", &pos) == ESP_OK) &&
              (nvs_get_u8(handle, "cal", &cal) == ESP_OK);
    nvs_close(handle);

    if (ok && cal == 1 && pos <= cfg_.max_steps) {
        current_pos_ = pos;
        return true;
    }
    return false;
}

} // namespace modesp
