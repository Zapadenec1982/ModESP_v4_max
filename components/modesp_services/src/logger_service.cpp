/**
 * @file logger_service.cpp
 * @brief Реалізація LoggerService — ring buffer логів
 */

#include "modesp/logger_service.h"
#include "esp_log.h"

static const char TAG[] = "LoggerSvc";

namespace modesp {

LoggerService::LoggerService()
    : BaseModule("logger", ModulePriority::LOW)
{
    mutex_ = xSemaphoreCreateMutex();
    configASSERT(mutex_ != nullptr);
}

LoggerService::~LoggerService() {
    if (mutex_) vSemaphoreDelete(mutex_);
}

bool LoggerService::on_init() {
    state_set("logger.count", static_cast<int32_t>(0));
    ESP_LOGI(TAG, "LoggerService initialized (buffer=%d, min_level=%d)",
             MODESP_MAX_LOG_ENTRIES, static_cast<int>(min_level));
    return true;
}

void LoggerService::on_update(uint32_t dt_ms) {
    uptime_ms_ += dt_ms;
}

void LoggerService::on_stop() {
    ESP_LOGI(TAG, "LoggerService stopped. Total logged: %lu", total_logged_);
}

void LoggerService::log(LogLevel level, const char* source, const char* message) {
    // Фільтр мінімального рівня (без mutex — read-only члена)
    if (level < min_level) return;

    // ESP_LOG поза mutex — щоб не блокувати логер при контенції
    switch (level) {
        case LogLevel::DEBUG:
            ESP_LOGD(source, "%s", message);
            break;
        case LogLevel::INFO:
            ESP_LOGI(source, "%s", message);
            break;
        case LogLevel::WARNING:
            ESP_LOGW(source, "%s", message);
            break;
        case LogLevel::ERROR:
            ESP_LOGE(source, "%s", message);
            break;
        case LogLevel::CRITICAL:
            ESP_LOGE(source, "[CRIT] %s", message);
            break;
    }

    // Критична секція: push в circular buffer + інкремент total_logged_
    uint32_t new_total = 0;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        total_logged_++;
        new_total = total_logged_;

        LogEntry entry;
        entry.timestamp_ms = uptime_ms_;
        entry.level = level;
        entry.source = source;
        entry.message = message;
        entries_.push(entry);
        xSemaphoreGive(mutex_);
    } else {
        ESP_LOGE("LoggerSvc", "Mutex timeout in log() — entry lost");
        return;
    }

    // SharedState + bus publish ПОЗА mutex
    state_set("logger.count", static_cast<int32_t>(new_total));

    if (publish_to_bus) {
        MsgLogEntry msg;
        msg.level = level;
        msg.source = source;
        msg.message = message;
        publish(msg);
    }
}

void LoggerService::read_recent(size_t count, EntryCallback cb, void* user_data) const {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;

    if (!entries_.empty()) {
        size_t start = 0;
        if (entries_.size() > count) {
            start = entries_.size() - count;
        }
        for (size_t i = start; i < entries_.size(); i++) {
            cb(entries_[i], user_data);
        }
    }
    xSemaphoreGive(mutex_);
}

size_t LoggerService::entry_count() const {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    size_t s = entries_.size();
    xSemaphoreGive(mutex_);
    return s;
}

} // namespace modesp
