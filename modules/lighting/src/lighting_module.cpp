/**
 * @file lighting_module.cpp
 * @brief Lighting control — simple mode-based relay request
 */

#include "lighting_module.h"
#include "esp_log.h"

static const char* TAG = "Lighting";

LightingModule::LightingModule()
    : BaseModule("lighting", modesp::ModulePriority::NORMAL)
{}

bool LightingModule::on_init() {
    // PersistService вже відновив lighting.mode з NVS
    mode_ = read_int("lighting.mode", 0);

    state_set("lighting.mode", mode_);
    state_set("lighting.state", false);
    state_set("lighting.req.light", false);

    ESP_LOGI(TAG, "Initialized (mode=%ld)", mode_);
    return true;
}

void LightingModule::on_update(uint32_t dt_ms) {
    (void)dt_ms;

    // Sync settings (WebUI/MQTT може змінити)
    mode_ = read_int("lighting.mode", 0);

    // Обчислюємо запит
    bool request = false;
    switch (mode_) {
        case 0:  // OFF
            request = false;
            break;
        case 1:  // ON
            request = true;
            break;
        case 2:  // AUTO — день=ON, ніч=OFF (ECO інтеграція)
            request = !read_bool("thermostat.night_active");
            break;
        default:
            request = false;
            break;
    }

    // Публікуємо тільки при зміні
    if (request != light_request_) {
        light_request_ = request;
        state_set("lighting.req.light", request);
        ESP_LOGI(TAG, "Light → %s (mode=%ld)", request ? "ON" : "OFF", mode_);
    }

    // Дзеркалюємо фактичний стан від Equipment
    bool actual = read_bool("equipment.light");
    if (actual != last_actual_) {
        last_actual_ = actual;
        state_set("lighting.state", actual);
    }
}

void LightingModule::on_stop() {
    state_set("lighting.req.light", false);
    ESP_LOGI(TAG, "Lighting stopped");
}
