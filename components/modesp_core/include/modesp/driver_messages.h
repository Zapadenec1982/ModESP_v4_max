/**
 * @file driver_messages.h
 * @brief Повідомлення для драйверів та модулів (Phase 3)
 *
 * Спільні структури повідомлень для сенсорів, актуаторів
 * та модулів бізнес-логіки. Використовують msg_id діапазони 110-249.
 */

#pragma once

#include "modesp/types.h"
#include "etl/message.h"
#include "etl/string.h"

namespace modesp {

// ═══════════════════════════════════════════════════════════════
// Sensor messages (110-119)
// ═══════════════════════════════════════════════════════════════

/// Сенсор надіслав нове значення
struct MsgSensorReading : etl::message<msg_id::SENSOR_READING> {
    etl::string<16> sensor_id;    // "temp1", "pressure1", ...
    float value;
    uint32_t timestamp_ms;
};

/// Сенсор повідомив про помилку
struct MsgSensorError : etl::message<msg_id::SENSOR_ERROR> {
    etl::string<16> sensor_id;
    int32_t error_code;
};

// ═══════════════════════════════════════════════════════════════
// Actuator messages (120-129)
// ═══════════════════════════════════════════════════════════════

/// Команда актуатору (від бізнес-логіки)
struct MsgActuatorCommand : etl::message<msg_id::ACTUATOR_COMMAND> {
    etl::string<16> actuator_id;  // "compressor", "fan", "heater"
    bool state;                   // true = ON, false = OFF
};

/// Зворотний зв'язок від актуатора
struct MsgActuatorFeedback : etl::message<msg_id::ACTUATOR_FEEDBACK> {
    etl::string<16> actuator_id;
    bool actual_state;            // Фактичний стан
    bool was_rejected;            // true = команда відхилена (min switch time, safe mode)
};

// ═══════════════════════════════════════════════════════════════
// Module messages (150+)
// ═══════════════════════════════════════════════════════════════
//
// MsgSetpointChanged видалено — setpoint це persistent state, не event.
// HTTP/MQTT/config пишуть напряму у SharedState, модулі polling-ують
// через sync_settings() у on_update(). Див. docs/13_message_bus.md.
//
// Доменні events для модулів додавати тут за потреби (alarm aggregation,
// defrost lifecycle тощо). Перед додаванням — перевірити decision flow
// у docs/13_message_bus.md: чи це справді event, чи краще state+polling.

} // namespace modesp
