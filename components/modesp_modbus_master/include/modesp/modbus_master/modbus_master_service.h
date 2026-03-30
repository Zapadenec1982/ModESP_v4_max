/**
 * @file modbus_master_service.h
 * @brief Modbus RTU Master — опитування виносних I/O модулів розширення
 *
 * Працює на UART2, незалежно від Modbus Slave (UART1).
 * Опитує expansion slaves, маппить registers → SharedState.
 *
 * Підтримувані типи expansion модулів:
 *   - Relay output modules (8/16 каналів)
 *   - Digital input modules (8/16 каналів)
 *   - Analog input modules (NTC, 4-20mA, 0-10V)
 *   - Combo modules (relay + DI + AI)
 *
 * Конфігурація через HTTP API: GET/POST /api/expansion
 * Mapping зберігається в NVS namespace "mbm"
 */

#pragma once

#include "modesp/shared_state.h"
#include <cstdint>

namespace modesp {

/// Один slave device на шині
struct ExpansionSlave {
    uint8_t  address;           ///< Modbus slave address (1-247)
    uint8_t  type;              ///< 0=disabled, 1=relay, 2=di, 3=ai, 4=combo
    uint16_t input_reg_start;   ///< Стартовий input register для читання
    uint16_t input_reg_count;   ///< Кількість input registers
    uint16_t coil_start;        ///< Стартовий coil для запису (relay)
    uint16_t coil_count;        ///< Кількість coils
    bool     online;            ///< Чи відповідає slave
    uint32_t errors;            ///< Лічильник помилок комунікації
    uint32_t last_ok_ms;        ///< Timestamp останньої успішної відповіді
};

static constexpr size_t MAX_EXPANSION_SLAVES = 8;

class ModbusMasterService {
public:
    /// Ініціалізація master на UART2
    /// @param tx_gpio GPIO для TX (default 17)
    /// @param rx_gpio GPIO для RX (default 16)
    /// @param baudrate Baud rate (default 9600)
    bool init(int tx_gpio = 17, int rx_gpio = 16, int baudrate = 9600);

    /// Periodic poll — викликати з main loop або окремого таску
    void poll();

    /// Зупинка master
    void stop();

    /// HTTP handlers для /api/expansion
    void register_http_handlers(void* http_server);

    /// Додати/видалити slave
    bool add_slave(uint8_t address, uint8_t type,
                   uint16_t input_start, uint16_t input_count,
                   uint16_t coil_start, uint16_t coil_count);
    bool remove_slave(uint8_t address);

    /// Записати coil (relay) на slave
    bool write_coil(uint8_t slave_addr, uint16_t coil_addr, bool value);

    /// SharedState pointer (set before init)
    void set_state(SharedState* s) { state_ = s; }

    /// Статус
    bool is_running() const { return running_; }
    size_t slave_count() const;

private:
    void* master_handle_ = nullptr;
    bool  running_ = false;
    SharedState* state_ = nullptr;

    ExpansionSlave slaves_[MAX_EXPANSION_SLAVES] = {};

    void poll_slave(ExpansionSlave& slave);
    void publish_inputs(const ExpansionSlave& slave, const uint16_t* data, size_t count);
    void load_config();
    void save_config();
};

} // namespace modesp
