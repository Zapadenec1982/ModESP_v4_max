/**
 * @file modbus_service.h
 * @brief Modbus RTU Slave service — exposes SharedState via RS-485
 *
 * Register map auto-generated from module manifests (see generated/modbus_regmap.h).
 * Scaling: x10 for float (42.5°C → 425), x1 for int/bool.
 *
 * NVS namespace: "modbus"
 * Default: disabled. Enable via POST /api/modbus or menuconfig.
 *
 * Hardware: UART1 → MAX13487EESA (auto-direction, no DE pin)
 * KC868-A6: TX=GPIO27, RX=GPIO14
 */

#pragma once

#include "modesp/base_module.h"
#include "esp_http_server.h"

namespace modesp {

class SharedState;

class ModbusService : public BaseModule {
public:
    ModbusService() : BaseModule("modbus", ModulePriority::HIGH) {}

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

    // Dependency injection
    void set_state(SharedState* s) { state_ = s; }
    void set_http_server(httpd_handle_t server);

    // Public API
    bool is_running() const { return running_; }
    bool is_enabled() const { return enabled_; }
    uint32_t requests_total() const { return requests_total_; }
    uint32_t errors_total() const { return errors_total_; }

private:
    SharedState* state_ = nullptr;
    httpd_handle_t server_ = nullptr;
    bool http_registered_ = false;

    // esp-modbus handle (v2 API)
    void* mb_handle_ = nullptr;

    // NVS config (namespace "modbus")
    bool enabled_ = false;
    int32_t slave_address_ = 1;       // 1-247
    int32_t baudrate_ = 9600;         // 9600/19200/38400/57600/115200
    int32_t parity_ = 1;             // 0=none, 1=even, 2=odd

    // Runtime state
    bool running_ = false;
    bool restart_pending_ = false;
    uint32_t sync_timer_ms_ = 0;
    static constexpr uint32_t SYNC_INTERVAL_MS = 500;  // SharedState → shadow sync

    // Statistics
    uint32_t requests_total_ = 0;
    uint32_t errors_total_ = 0;

    // Shadow arrays — accessed by esp-modbus task AND on_update()
    // Protected by mbc_slave_lock()/unlock()
    // Sizes match max register offset in generated/modbus_regmap.h:
    //   With zone expansion: max input = 30549 (eev Z2) → offset 548 → need [549]
    //   With zone expansion: max holding = 40499 (eev Z2) → offset 498 → need [499]
    uint16_t shadow_input_[550] = {};     // Input registers (read-only, zone-expanded)
    uint16_t shadow_holding_[500] = {};   // Holding registers (read-write, zone-expanded)
    uint16_t prev_holding_[500] = {};     // Previous holding values (for write detection)
    uint8_t shadow_coils_[16] = {};       // Coils (read-only, alarm bits)
    uint8_t shadow_discrete_[8] = {};     // Discrete inputs (relay states)

    // Internal
    void load_config();
    bool start_modbus();
    void stop_modbus();
    void sync_state_to_shadow();
    void sync_shadow_to_state();
    void register_http_handlers();

    // HTTP handlers
    static esp_err_t handle_get_modbus(httpd_req_t* req);
    static esp_err_t handle_post_modbus(httpd_req_t* req);
    static esp_err_t handle_options(httpd_req_t* req);
    static void set_cors_headers(httpd_req_t* req);
};

} // namespace modesp
