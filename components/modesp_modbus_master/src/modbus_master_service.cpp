/**
 * @file modbus_master_service.cpp
 * @brief Modbus RTU Master — опитування виносних I/O expansion модулів
 *
 * UART2, незалежно від Modbus Slave (UART1).
 * Опитує slaves кожен poll() виклик, публікує в SharedState.
 */

#include "modesp/modbus_master/modbus_master_service.h"
#include "modesp/shared_state.h"
#include "esp_log.h"
#include "driver/uart.h"

// esp-modbus master API
#include "esp_modbus_master.h"

static const char* TAG = "MBM";

namespace modesp {

bool ModbusMasterService::init(int tx_gpio, int rx_gpio, int baudrate) {
    if (running_) return true;

    // Конфігурація master на UART2
    mb_communication_info_t comm = {};
    comm.ser_opts.port = UART_NUM_2;
    comm.ser_opts.mode = MB_RTU;
    comm.ser_opts.baudrate = static_cast<uint32_t>(baudrate);
    comm.ser_opts.parity = UART_PARITY_EVEN;
    comm.ser_opts.data_bits = UART_DATA_8_BITS;
    comm.ser_opts.stop_bits = UART_STOP_BITS_1;
    // Timeout для відповіді slave
    comm.ser_opts.response_tout_ms = 500;

    esp_err_t err = mbc_master_create_serial(&comm, &master_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_master_create_serial failed: %s", esp_err_to_name(err));
        return false;
    }

    // Налаштування GPIO для UART2
    uart_set_pin(UART_NUM_2, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX);

    err = mbc_master_start(master_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_master_start failed: %s", esp_err_to_name(err));
        mbc_master_delete(master_handle_);
        master_handle_ = nullptr;
        return false;
    }

    load_config();
    running_ = true;
    ESP_LOGI(TAG, "Modbus Master started on UART2 (TX=%d, RX=%d, %d baud)", tx_gpio, rx_gpio, baudrate);
    return true;
}

void ModbusMasterService::stop() {
    if (!running_) return;
    running_ = false;

    if (master_handle_) {
        mbc_master_stop(master_handle_);
        mbc_master_delete(master_handle_);
        master_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "Modbus Master stopped");
}

void ModbusMasterService::poll() {
    if (!running_ || !master_handle_) return;

    for (size_t i = 0; i < MAX_EXPANSION_SLAVES; i++) {
        if (slaves_[i].type == 0) continue;  // disabled
        poll_slave(slaves_[i]);
    }
}

void ModbusMasterService::poll_slave(ExpansionSlave& slave) {
    // Читання input registers (sensor data)
    if (slave.input_reg_count > 0) {
        uint16_t buf[32] = {};
        size_t count = (slave.input_reg_count > 32) ? 32 : slave.input_reg_count;

        mb_param_request_t req = {};
        req.slave_addr = slave.address;
        req.command    = 4;  // Read Input Registers
        req.reg_start  = slave.input_reg_start;
        req.reg_size   = static_cast<uint16_t>(count);

        esp_err_t err = mbc_master_send_request(master_handle_, &req, buf);
        if (err == ESP_OK) {
            slave.online = true;
            slave.last_ok_ms = esp_log_timestamp();
            publish_inputs(slave, buf, count);
        } else {
            slave.errors++;
            if (slave.online && slave.errors > 3) {
                slave.online = false;
                ESP_LOGW(TAG, "Slave %d offline (errors=%lu)", slave.address,
                         static_cast<unsigned long>(slave.errors));
            }
        }
    }
}

void ModbusMasterService::publish_inputs(const ExpansionSlave& slave,
                                          const uint16_t* data, size_t count) {
    // Маппінг: expansion.s{addr}.input_{n} → SharedState
    auto& ss = SharedState::instance();
    char key[48];

    for (size_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "expansion.s%d.input_%d",
                 slave.address, static_cast<int>(i));
        // Scale: int16 × 0.1 → float (standard Modbus convention)
        float val = static_cast<int16_t>(data[i]) / 10.0f;
        ss.set(key, val);
    }

    snprintf(key, sizeof(key), "expansion.s%d.online", slave.address);
    ss.set(key, slave.online);
}

bool ModbusMasterService::write_coil(uint8_t slave_addr, uint16_t coil_addr, bool value) {
    if (!running_ || !master_handle_) return false;

    mb_param_request_t req = {};
    req.slave_addr = slave_addr;
    req.command    = 5;  // Write Single Coil
    req.reg_start  = coil_addr;
    req.reg_size   = 1;

    uint16_t coil_val = value ? 0xFF00 : 0x0000;
    esp_err_t err = mbc_master_send_request(master_handle_, &req, &coil_val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Write coil failed: slave=%d, addr=%d, err=%s",
                 slave_addr, coil_addr, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ModbusMasterService::add_slave(uint8_t address, uint8_t type,
                                     uint16_t input_start, uint16_t input_count,
                                     uint16_t coil_start, uint16_t coil_count) {
    for (size_t i = 0; i < MAX_EXPANSION_SLAVES; i++) {
        if (slaves_[i].type == 0) {
            slaves_[i] = {address, type, input_start, input_count,
                          coil_start, coil_count, false, 0, 0};
            save_config();
            ESP_LOGI(TAG, "Slave %d added (type=%d, inputs=%d, coils=%d)",
                     address, type, input_count, coil_count);
            return true;
        }
    }
    ESP_LOGW(TAG, "Max expansion slaves reached (%d)", static_cast<int>(MAX_EXPANSION_SLAVES));
    return false;
}

bool ModbusMasterService::remove_slave(uint8_t address) {
    for (size_t i = 0; i < MAX_EXPANSION_SLAVES; i++) {
        if (slaves_[i].address == address && slaves_[i].type != 0) {
            slaves_[i] = {};
            save_config();
            ESP_LOGI(TAG, "Slave %d removed", address);
            return true;
        }
    }
    return false;
}

size_t ModbusMasterService::slave_count() const {
    size_t count = 0;
    for (size_t i = 0; i < MAX_EXPANSION_SLAVES; i++) {
        if (slaves_[i].type != 0) count++;
    }
    return count;
}

void ModbusMasterService::load_config() {
    // TODO: завантаження з NVS namespace "mbm"
    // Формат: address|type|input_start|input_count|coil_start|coil_count для кожного slot
    ESP_LOGI(TAG, "Config loaded (placeholder)");
}

void ModbusMasterService::save_config() {
    // TODO: збереження в NVS namespace "mbm"
    ESP_LOGI(TAG, "Config saved (placeholder)");
}

void ModbusMasterService::register_http_handlers(void* http_server) {
    // TODO: GET/POST /api/expansion
    // GET: list slaves, status, online/offline
    // POST: add/remove slave, write coil
    ESP_LOGI(TAG, "HTTP handlers registered (placeholder)");
}

} // namespace modesp
