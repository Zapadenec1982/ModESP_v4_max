#pragma once
/**
 * @brief Interface for providing historical data for backfill after MQTT reconnect.
 *
 * DataLogger implements this to bridge with MqttService without direct coupling.
 * Lives in modesp_core to avoid circular dependencies between mqtt and datalogger.
 */

#include <cstdint>

namespace modesp {

class BackfillProvider {
public:
    virtual ~BackfillProvider() = default;

    /// Number of unsynced temp records (across .old + current files)
    virtual uint32_t get_unsync_temp_count() = 0;

    /// Read up to max_count temp records into buf, return actual count
    virtual uint32_t read_unsync_temp(void* buf, uint32_t max_count) = 0;

    /// Advance temp sync position by count records
    virtual void advance_temp_sync(uint32_t count) = 0;

    /// Number of unsynced event records
    virtual uint32_t get_unsync_event_count() = 0;

    /// Read up to max_count event records into buf, return actual count
    virtual uint32_t read_unsync_events(void* buf, uint32_t max_count) = 0;

    /// Advance event sync position by count records
    virtual void advance_event_sync(uint32_t count) = 0;

    /// Flush RAM buffers to flash (called on disconnect to preserve offline data)
    virtual void on_disconnect_flush() {}

    /// Persist sync position to NVS (called when backfill completes)
    virtual void save_sync_position() {}
};

} // namespace modesp
