/**
 * @file eev_pcf8574_stepper_driver.cpp
 * @brief EEV stepper driver via PCF8574 — implementation
 *
 * Step generation: Each step requires two I2C writes (STEP HIGH, STEP LOW).
 * At I2C 100kHz, each write takes ~0.3ms → one step = ~0.6ms.
 * At 50Hz (20ms interval), only 3% of CPU time spent on I2C.
 *
 * The STEP pulse is generated across two update() calls:
 *   Call 1: Set STEP HIGH → write I2C
 *   Call 2: Set STEP LOW → write I2C → increment/decrement position
 * This ensures minimum pulse width of ~10ms (one update cycle at 100Hz).
 */

#include "eev_pcf8574_stepper_driver.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char TAG[] = "EevPCF8574";

namespace modesp {

void EevPcf8574StepperDriver::configure(const char* role,
                                         I2CExpanderResource* expander,
                                         uint8_t step_pin, uint8_t dir_pin,
                                         uint16_t max_steps, const char* nvs_ns) {
    role_ = role;
    expander_ = expander;
    step_pin_ = step_pin;
    dir_pin_ = dir_pin;
    max_steps_ = max_steps > 0 ? max_steps : 480;
    nvs_ns_ = nvs_ns;
    configured_ = true;
}

bool EevPcf8574StepperDriver::init() {
    if (!configured_ || !expander_) return false;

    // Set both STEP and DIR LOW initially
    expander_->output_state &= ~(1 << step_pin_);
    expander_->output_state &= ~(1 << dir_pin_);
    expander_->write_state();

    // Try restore position from NVS
    if (load_position()) {
        ESP_LOGI(TAG, "[%s] Position restored from NVS: %u/%u",
                 role_.c_str(), current_pos_, max_steps_);
        calibrated_ = true;
    } else {
        ESP_LOGI(TAG, "[%s] No saved position — homing required", role_.c_str());
        current_pos_ = 0;
        calibrated_ = false;
    }

    target_pos_ = current_pos_;
    step_phase_ = false;
    initialized_ = true;

    ESP_LOGI(TAG, "[%s] Initialized (STEP=pin%d DIR=pin%d max=%u on '%s')",
             role_.c_str(), step_pin_, dir_pin_, max_steps_,
             expander_->id.c_str());
    return true;
}

void EevPcf8574StepperDriver::update(uint32_t dt_ms) {
    if (!initialized_) return;

    // ── Homing state machine ──
    if (homing_state_ == HomingState::CLOSING) {
        step_timer_ms_ += dt_ms;
        uint32_t interval = STEP_INTERVAL_MS;

        if (step_timer_ms_ >= interval) {
            step_timer_ms_ -= interval;

            if (homing_remaining_ > 0) {
                set_dir(false);  // Close direction
                if (!do_step()) return;  // I2C failed — retry next cycle
                homing_remaining_--;
            } else {
                // Homing complete
                current_pos_ = 0;
                target_pos_ = 0;
                calibrated_ = true;
                homing_state_ = HomingState::DONE;
                save_position();
                ESP_LOGI(TAG, "[%s] Homing complete — position=0", role_.c_str());
            }
        }
        return;
    }

    // ── Normal operation — move towards target ──
    if (current_pos_ == target_pos_) {
        // NVS debounce: зберігаємо тільки якщо позиція стабільна 60с
        if (nvs_dirty_) {
            nvs_stable_ms_ += dt_ms;
            if (nvs_stable_ms_ >= NVS_DEBOUNCE_MS) {
                save_position();
                nvs_dirty_ = false;
            }
        }
        return;
    }

    step_timer_ms_ += dt_ms;
    uint32_t interval = emergency_ ? EMERGENCY_INTERVAL_MS : STEP_INTERVAL_MS;

    if (step_timer_ms_ >= interval) {
        step_timer_ms_ -= interval;

        bool opening = target_pos_ > current_pos_;
        set_dir(opening);
        if (!do_step()) return;  // I2C failed — retry next cycle

        if (opening) {
            current_pos_++;
        } else {
            if (current_pos_ > 0) current_pos_--;
        }

        // Target reached — mark dirty, start debounce timer
        if (current_pos_ == target_pos_) {
            emergency_ = false;
            nvs_dirty_ = true;
            nvs_stable_ms_ = 0;
        }
    }
}

bool EevPcf8574StepperDriver::set_position(uint16_t target) {
    if (!initialized_) return false;
    if (target > max_steps_) target = max_steps_;
    if (!calibrated_) {
        ESP_LOGW(TAG, "[%s] Not calibrated — homing first", role_.c_str());
        return false;
    }

    target_pos_ = target;
    return true;
}

bool EevPcf8574StepperDriver::calibrate() {
    if (!initialized_) return false;

    ESP_LOGI(TAG, "[%s] Starting homing (%u + %u extra steps)",
             role_.c_str(), max_steps_, HOMING_EXTRA_STEPS);

    homing_remaining_ = max_steps_ + HOMING_EXTRA_STEPS;
    homing_state_ = HomingState::CLOSING;
    step_timer_ms_ = 0;
    return true;
}

void EevPcf8574StepperDriver::emergency_close() {
    if (!initialized_) return;

    target_pos_ = 0;
    emergency_ = true;
    step_timer_ms_ = 0;
    ESP_LOGW(TAG, "[%s] Emergency close — fast mode", role_.c_str());
}

bool EevPcf8574StepperDriver::do_step() {
    // STEP HIGH
    expander_->output_state |= (1 << step_pin_);
    if (!expander_->write_state()) {
        ESP_LOGW(TAG, "[%s] I2C STEP write failed", role_.c_str());
        return false;  // Не інкрементуємо position — крок не виконано
    }

    // STEP LOW (I2C write ~0.3ms = достатня ширина імпульсу)
    expander_->output_state &= ~(1 << step_pin_);
    if (!expander_->write_state()) {
        ESP_LOGW(TAG, "[%s] I2C STEP LOW write failed", role_.c_str());
    }
    return true;
}

void EevPcf8574StepperDriver::set_dir(bool open_direction) {
    if (open_direction) {
        expander_->output_state |= (1 << dir_pin_);
    } else {
        expander_->output_state &= ~(1 << dir_pin_);
    }
    // DIR is written together with next STEP write — no separate I2C call needed
}

void EevPcf8574StepperDriver::save_position() {
    if (!nvs_ns_) return;

    nvs_handle_t handle;
    if (nvs_open(nvs_ns_, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u16(handle, "pos", current_pos_);
        nvs_set_u8(handle, "cal", calibrated_ ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool EevPcf8574StepperDriver::load_position() {
    if (!nvs_ns_) return false;

    nvs_handle_t handle;
    if (nvs_open(nvs_ns_, NVS_READONLY, &handle) != ESP_OK) return false;

    uint16_t pos = 0;
    uint8_t cal = 0;
    bool ok = (nvs_get_u16(handle, "pos", &pos) == ESP_OK) &&
              (nvs_get_u8(handle, "cal", &cal) == ESP_OK);
    nvs_close(handle);

    if (ok && cal == 1 && pos <= max_steps_) {
        current_pos_ = pos;
        return true;
    }
    return false;
}

} // namespace modesp
