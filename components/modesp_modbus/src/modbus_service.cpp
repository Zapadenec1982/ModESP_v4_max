/**
 * @file modbus_service.cpp
 * @brief Modbus RTU Slave — ESP-IDF esp-modbus v2 integration
 *
 * Sync loop: SharedState ↔ shadow arrays (every 500ms)
 * Thread safety: mbc_slave_lock()/unlock() for shadow access
 */

#include "modesp/modbus/modbus_service.h"
#include "modesp/shared_state.h"
#include "modesp/services/nvs_helper.h"
#include "modbus_regmap.h"

#include "mbcontroller.h"  // esp-modbus
#include "esp_log.h"
#include "jsmn.h"

#include <cstring>
#include <cmath>

static const char* TAG = "modbus";

namespace modesp {

// ═══════════════════════════════════════════════════════════════
//  NVS Config
// ═══════════════════════════════════════════════════════════════

void ModbusService::load_config() {
    nvs_helper::read_bool("modbus", "enabled", enabled_);
    nvs_helper::read_i32("modbus", "address", slave_address_);
    nvs_helper::read_i32("modbus", "baudrate", baudrate_);
    nvs_helper::read_i32("modbus", "parity", parity_);

    // Clamp to valid ranges
    if (slave_address_ < 1 || slave_address_ > 247) slave_address_ = 1;
    if (baudrate_ != 9600 && baudrate_ != 19200 && baudrate_ != 38400 &&
        baudrate_ != 57600 && baudrate_ != 115200) {
        baudrate_ = 9600;
    }
    if (parity_ < 0 || parity_ > 2) parity_ = 1;  // default EVEN

    ESP_LOGI(TAG, "Config: enabled=%d addr=%d baud=%d parity=%d",
             enabled_, (int)slave_address_, (int)baudrate_, (int)parity_);
}

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

bool ModbusService::on_init() {
    load_config();
    if (enabled_) {
        return start_modbus();
    }
    ESP_LOGI(TAG, "Modbus disabled (enable via /api/modbus or menuconfig)");
    return true;
}

void ModbusService::on_update(uint32_t dt_ms) {
    if (restart_pending_) {
        restart_pending_ = false;
        stop_modbus();
        if (enabled_) {
            start_modbus();
        }
        return;
    }

    if (!running_ || !state_) return;

    // Register HTTP handlers (deferred until server available)
    if (server_ && !http_registered_) {
        register_http_handlers();
    }

    // Periodic sync SharedState → shadow
    sync_timer_ms_ += dt_ms;
    if (sync_timer_ms_ >= SYNC_INTERVAL_MS) {
        sync_timer_ms_ = 0;
        sync_state_to_shadow();
        sync_shadow_to_state();  // Check for master writes
    }
}

void ModbusService::on_stop() {
    stop_modbus();
}

// ═══════════════════════════════════════════════════════════════
//  esp-modbus Start/Stop
// ═══════════════════════════════════════════════════════════════

bool ModbusService::start_modbus() {
    // Create Modbus slave controller
    esp_err_t err = mbc_slave_init(MB_PORT_SERIAL_SLAVE, &mb_handle_);
    if (err != ESP_OK || !mb_handle_) {
        ESP_LOGE(TAG, "mbc_slave_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // UART communication config
    // KC868-A6: RS485 via MAX13487EESA (auto-direction)
    // TX=GPIO27, RX=GPIO14, no RTS needed
    mb_communication_info_t comm_info = {};
    comm_info.mode = MB_MODE_RTU;
    comm_info.slave_addr = static_cast<uint8_t>(slave_address_);
    comm_info.port = UART_NUM_1;
    comm_info.baudrate = static_cast<uint32_t>(baudrate_);
    comm_info.parity = (parity_ == 0) ? UART_PARITY_DISABLE :
                       (parity_ == 1) ? UART_PARITY_EVEN : UART_PARITY_ODD;

    err = mbc_slave_setup(&comm_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_setup failed: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        mb_handle_ = nullptr;
        return false;
    }

    // Configure UART pins (after mbc_slave_setup which creates the UART driver)
    uart_set_pin(UART_NUM_1, 27, 14, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Register shadow arrays as Modbus areas
    // Input registers (read-only)
    mb_register_area_descriptor_t input_area = {};
    input_area.type = MB_PARAM_INPUT;
    input_area.start_offset = 0;
    input_area.address = shadow_input_;
    input_area.size = sizeof(shadow_input_);
    mbc_slave_set_descriptor(input_area);

    // Holding registers (read-write)
    mb_register_area_descriptor_t holding_area = {};
    holding_area.type = MB_PARAM_HOLDING;
    holding_area.start_offset = 0;
    holding_area.address = shadow_holding_;
    holding_area.size = sizeof(shadow_holding_);
    mbc_slave_set_descriptor(holding_area);

    // Coils (read-only alarm bits)
    mb_register_area_descriptor_t coil_area = {};
    coil_area.type = MB_PARAM_COIL;
    coil_area.start_offset = 0;
    coil_area.address = shadow_coils_;
    coil_area.size = sizeof(shadow_coils_);
    mbc_slave_set_descriptor(coil_area);

    // Discrete inputs (relay states)
    mb_register_area_descriptor_t discrete_area = {};
    discrete_area.type = MB_PARAM_DISCRETE;
    discrete_area.start_offset = 0;
    discrete_area.address = shadow_discrete_;
    discrete_area.size = sizeof(shadow_discrete_);
    mbc_slave_set_descriptor(discrete_area);

    // Start Modbus stack (creates internal FreeRTOS task)
    err = mbc_slave_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_start failed: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        mb_handle_ = nullptr;
        return false;
    }

    running_ = true;
    ESP_LOGI(TAG, "Modbus RTU Slave started: addr=%d baud=%d parity=%d",
             (int)slave_address_, (int)baudrate_, (int)parity_);

    // Initial sync
    if (state_) {
        sync_state_to_shadow();
    }

    return true;
}

void ModbusService::stop_modbus() {
    if (mb_handle_) {
        mbc_slave_destroy();
        mb_handle_ = nullptr;
    }
    running_ = false;
    ESP_LOGI(TAG, "Modbus stopped");
}

// ═══════════════════════════════════════════════════════════════
//  Sync: SharedState → Shadow Arrays (read path, every 500ms)
// ═══════════════════════════════════════════════════════════════

void ModbusService::sync_state_to_shadow() {
    if (!state_ || !mb_handle_) return;

    // Input registers (RO): sensor values, status
    for (size_t i = 0; i < gen::MODBUS_INPUT_REG_COUNT; ++i) {
        const auto& entry = gen::MODBUS_INPUT_REGS[i];
        auto val = state_->get(entry.state_key);
        if (!val.has_value()) continue;

        int16_t raw = 0;
        if (entry.type == 1) {  // float_x10
            const auto* fp = etl::get_if<float>(&val.value());
            if (fp) raw = static_cast<int16_t>(std::round(*fp * entry.scale));
        } else if (entry.type == 0) {  // int16
            const auto* ip = etl::get_if<int32_t>(&val.value());
            if (ip) raw = static_cast<int16_t>(*ip);
        } else if (entry.type == 2) {  // bool
            const auto* bp = etl::get_if<bool>(&val.value());
            if (bp) raw = *bp ? 1 : 0;
        }

        // Write to shadow (address offset from base)
        uint16_t offset = entry.address - 30001;
        if (offset < sizeof(shadow_input_) / sizeof(uint16_t)) {
            shadow_input_[offset] = static_cast<uint16_t>(raw);
        }
    }

    // Holding registers (RW): sync current values to shadow
    for (size_t i = 0; i < gen::MODBUS_HOLDING_REG_COUNT; ++i) {
        const auto& entry = gen::MODBUS_HOLDING_REGS[i];
        auto val = state_->get(entry.state_key);
        if (!val.has_value()) continue;

        int16_t raw = 0;
        if (entry.type == 1) {
            const auto* fp = etl::get_if<float>(&val.value());
            if (fp) raw = static_cast<int16_t>(std::round(*fp * entry.scale));
        } else if (entry.type == 0) {
            const auto* ip = etl::get_if<int32_t>(&val.value());
            if (ip) raw = static_cast<int16_t>(*ip);
        } else if (entry.type == 2) {
            const auto* bp = etl::get_if<bool>(&val.value());
            if (bp) raw = *bp ? 1 : 0;
        }

        uint16_t offset = entry.address - 40001;
        if (offset < sizeof(shadow_holding_) / sizeof(uint16_t)) {
            shadow_holding_[offset] = static_cast<uint16_t>(raw);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  Sync: Shadow → SharedState (write path — master wrote to holding)
// ═══════════════════════════════════════════════════════════════

void ModbusService::sync_shadow_to_state() {
    if (!state_ || !mb_handle_) return;

    // Check for master write events (non-blocking)
    mb_param_info_t info = {};
    esp_err_t err = mbc_slave_check_event(static_cast<mb_event_group_t>(MB_EVENT_HOLDING_REG_WR));
    if (err != ESP_OK) return;  // No write events

    // Get info about what was written
    err = mbc_slave_get_param_info(&info, pdMS_TO_TICKS(10));
    if (err != ESP_OK) return;

    requests_total_++;

    // Find which holding register was changed and update SharedState
    for (size_t i = 0; i < gen::MODBUS_HOLDING_REG_COUNT; ++i) {
        const auto& entry = gen::MODBUS_HOLDING_REGS[i];
        uint16_t offset = entry.address - 40001;
        if (offset >= sizeof(shadow_holding_) / sizeof(uint16_t)) continue;

        int16_t raw = static_cast<int16_t>(shadow_holding_[offset]);

        if (entry.type == 1) {  // float_x10
            float val = static_cast<float>(raw) / entry.scale;
            state_->set(entry.state_key, val);
        } else if (entry.type == 0) {  // int16
            state_->set(entry.state_key, static_cast<int32_t>(raw));
        } else if (entry.type == 2) {  // bool
            state_->set(entry.state_key, raw != 0);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  HTTP API
// ═══════════════════════════════════════════════════════════════

void ModbusService::set_http_server(httpd_handle_t server) {
    server_ = server;
    if (server_ && !http_registered_) {
        register_http_handlers();
    }
}

void ModbusService::set_cors_headers(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

esp_err_t ModbusService::handle_options(httpd_req_t* req) {
    set_cors_headers(req);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t ModbusService::handle_get_modbus(httpd_req_t* req) {
    auto* self = static_cast<ModbusService*>(req->user_ctx);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"address\":%d,\"baudrate\":%d,\"parity\":%d,"
        "\"status\":\"%s\",\"input_reg_count\":%zu,\"holding_reg_count\":%zu,"
        "\"requests_total\":%u,\"errors_total\":%u}",
        self->enabled_ ? "true" : "false",
        (int)self->slave_address_,
        (int)self->baudrate_,
        (int)self->parity_,
        self->running_ ? "running" : "stopped",
        gen::MODBUS_INPUT_REG_COUNT,
        gen::MODBUS_HOLDING_REG_COUNT,
        (unsigned)self->requests_total_,
        (unsigned)self->errors_total_);

    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

esp_err_t ModbusService::handle_post_modbus(httpd_req_t* req) {
    auto* self = static_cast<ModbusService*>(req->user_ctx);
    set_cors_headers(req);

    char body[256] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    // Parse JSON with jsmn
    jsmn_parser parser;
    jsmntok_t tokens[16];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, body, received, tokens, 16);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    bool changed = false;
    for (int i = 1; i < r; i++) {
        if (tokens[i].type != JSMN_STRING) continue;
        int key_len = tokens[i].end - tokens[i].start;
        const char* key = body + tokens[i].start;
        i++;
        if (i >= r) break;
        const char* val = body + tokens[i].start;
        int val_len = tokens[i].end - tokens[i].start;

        if (key_len == 7 && strncmp(key, "enabled", 7) == 0) {
            self->enabled_ = (strncmp(val, "true", 4) == 0);
            nvs_helper::write_bool("modbus", "enabled", self->enabled_);
            changed = true;
        } else if (key_len == 7 && strncmp(key, "address", 7) == 0) {
            char tmp[8] = {};
            memcpy(tmp, val, val_len < 7 ? val_len : 7);
            self->slave_address_ = atoi(tmp);
            nvs_helper::write_i32("modbus", "address", self->slave_address_);
            changed = true;
        } else if (key_len == 8 && strncmp(key, "baudrate", 8) == 0) {
            char tmp[8] = {};
            memcpy(tmp, val, val_len < 7 ? val_len : 7);
            self->baudrate_ = atoi(tmp);
            nvs_helper::write_i32("modbus", "baudrate", self->baudrate_);
            changed = true;
        } else if (key_len == 6 && strncmp(key, "parity", 6) == 0) {
            char tmp[4] = {};
            memcpy(tmp, val, val_len < 3 ? val_len : 3);
            self->parity_ = atoi(tmp);
            nvs_helper::write_i32("modbus", "parity", self->parity_);
            changed = true;
        }
    }

    if (changed) {
        self->restart_pending_ = true;
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"restart\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No valid fields");
    }
    return ESP_OK;
}

void ModbusService::register_http_handlers() {
    if (!server_ || http_registered_) return;

    httpd_uri_t get_uri = {
        .uri = "/api/modbus",
        .method = HTTP_GET,
        .handler = handle_get_modbus,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &get_uri);

    httpd_uri_t post_uri = {
        .uri = "/api/modbus",
        .method = HTTP_POST,
        .handler = handle_post_modbus,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &post_uri);

    httpd_uri_t options_uri = {
        .uri = "/api/modbus",
        .method = HTTP_OPTIONS,
        .handler = handle_options,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &options_uri);

    http_registered_ = true;
    ESP_LOGI(TAG, "HTTP handlers registered: /api/modbus");
}

} // namespace modesp
