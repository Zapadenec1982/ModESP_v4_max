#include "modesp/hal/adc_shared.h"

#ifndef HOST_BUILD

#include "esp_log.h"

static const char TAG[] = "ADC_Shared";

namespace modesp {

static adc_oneshot_unit_handle_t s_adc1_handle = nullptr;

adc_oneshot_unit_handle_t get_shared_adc1_handle() {
    if (!s_adc1_handle) {
        adc_oneshot_unit_init_cfg_t unit_cfg = {};
        unit_cfg.unit_id = ADC_UNIT_1;
        unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;

        esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc1_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC1 unit init failed: %s", esp_err_to_name(err));
            return nullptr;
        }
        ESP_LOGI(TAG, "ADC1 shared handle created");
    }
    return s_adc1_handle;
}

} // namespace modesp

#endif // HOST_BUILD
