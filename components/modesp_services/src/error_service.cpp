/**
 * @file error_service.cpp
 * @brief Реалізація ErrorService — збір помилок + Safe Mode
 */

#include "modesp/error_service.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char TAG[] = "ErrorService";

namespace modesp {

ErrorService::ErrorService()
    : BaseModule("error", ModulePriority::CRITICAL)
{
    mutex_ = xSemaphoreCreateMutex();
    configASSERT(mutex_ != nullptr);
}

ErrorService::~ErrorService() {
    if (mutex_) vSemaphoreDelete(mutex_);
}

bool ErrorService::on_init() {
    state_set("error.count", static_cast<int32_t>(0));
    state_set("error.safe_mode", false);
    ESP_LOGI(TAG, "ErrorService initialized");
    return true;
}

void ErrorService::on_update(uint32_t dt_ms) {
    uptime_ms_ += dt_ms;
}

void ErrorService::on_message(const etl::imessage& msg) {
    // ErrorService може реагувати на системні повідомлення
    // Наприклад, MsgModuleTimeout від WatchdogService
    if (msg.get_message_id() == msg_id::MODULE_TIMEOUT) {
        const auto& timeout_msg = static_cast<const MsgModuleTimeout&>(msg);
        report(timeout_msg.module_name.c_str(), -1,
               ErrorSeverity::ERROR, "Module timeout");
    }
}

void ErrorService::on_stop() {
    ESP_LOGI(TAG, "ErrorService stopped. Total errors: %lu", total_errors_);
}

void ErrorService::report(const char* source, int32_t code,
                          ErrorSeverity severity, const char* description) {
    // ESP_LOG поза mutex — щоб не блокувати логер при контенції
    switch (severity) {
        case ErrorSeverity::INFO:
            ESP_LOGI(TAG, "[%s] code=%ld: %s", source, code, description);
            break;
        case ErrorSeverity::WARNING:
            ESP_LOGW(TAG, "[%s] code=%ld: %s", source, code, description);
            break;
        case ErrorSeverity::ERROR:
            ESP_LOGE(TAG, "[%s] code=%ld: %s", source, code, description);
            break;
        case ErrorSeverity::CRITICAL:
            ESP_LOGE(TAG, "CRITICAL [%s] code=%ld: %s", source, code, description);
            break;
        case ErrorSeverity::FATAL:
            ESP_LOGE(TAG, "*** FATAL [%s] code=%ld: %s ***", source, code, description);
            break;
    }

    // Критична секція: push в circular buffer + інкремент total_errors_
    // (захищає від race з for_each_history() з HTTP/WS task)
    uint32_t new_total = 0;
    bool trigger_safe_mode = false;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        total_errors_++;
        new_total = total_errors_;

        ErrorRecord record;
        record.timestamp_ms = uptime_ms_;
        record.source = source;
        record.code = code;
        record.severity = severity;
        record.description = description;
        history_.push(record);

        if (severity == ErrorSeverity::FATAL && !safe_mode_) {
            trigger_safe_mode = true;
        }
        xSemaphoreGive(mutex_);
    } else {
        ESP_LOGE(TAG, "Mutex timeout in report() — record lost");
        return;
    }

    // Публікуємо state/повідомлення ПОЗА mutex (щоб уникнути deadlock з SharedState)
    state_set("error.count", static_cast<int32_t>(new_total));

    if (severity >= ErrorSeverity::WARNING) {
        MsgSystemError err_msg;
        err_msg.source = source;
        err_msg.code = code;
        err_msg.severity = severity;
        err_msg.description = description;
        publish(err_msg);
    }

    if (trigger_safe_mode) {
        enter_safe_mode(description);
    }
}

void ErrorService::for_each_history(HistoryCallback cb, void* user_data) const {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;
    for (const auto& r : history_) {
        cb(r, user_data);
    }
    xSemaphoreGive(mutex_);
}

void ErrorService::enter_safe_mode(const char* reason) {
    safe_mode_ = true;
    state_set("error.safe_mode", true);
    state_set("error.safe_mode_reason", reason);

    ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGE(TAG, "║     *** SAFE MODE ACTIVATED ***   ║");
    ESP_LOGE(TAG, "║  Reason: %-24s ║", reason);
    ESP_LOGE(TAG, "╚═══════════════════════════════════╝");

    // Публікуємо на шину — актуатори повинні вимкнутись
    MsgSafeMode msg;
    msg.reason = reason;
    publish(msg);
}

} // namespace modesp
