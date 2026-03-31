/**
 * @file adc_shared.h
 * @brief Shared ADC1 handle for all ADC-based sensor drivers
 *
 * ESP32 ADC1 unit може мати тільки один handle (adc_oneshot_new_unit).
 * NTC та PressureAdc drivers використовують ADC1 — shared handle
 * запобігає конфлікту при ініціалізації обох drivers одночасно.
 */

#pragma once

#ifndef HOST_BUILD
  #include "esp_adc/adc_oneshot.h"

namespace modesp {

/// Get or create shared ADC1 oneshot handle.
/// First call creates the handle, subsequent calls return the same handle.
/// @return handle on success, nullptr on failure.
adc_oneshot_unit_handle_t get_shared_adc1_handle();

} // namespace modesp

#endif // HOST_BUILD
