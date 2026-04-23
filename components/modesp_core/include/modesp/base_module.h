/**
 * @file base_module.h
 * @brief Базовий клас для всіх модулів ModESP v4
 *
 * Кожен модуль (драйвер, сервіс, бізнес-логіка) наслідує BaseModule.
 * Модуль отримує доступ до:
 *   - publish()      → відправити повідомлення на шину
 *   - state_set/get  → читати/писати SharedState
 *   - on_init()      → ініціалізація (викликається один раз)
 *   - on_update()    → періодичне оновлення (100Hz)
 *   - on_message()   → обробка вхідних повідомлень
 *   - on_stop()      → graceful shutdown
 *
 * Модуль НЕ знає про App, ModuleManager, або Message Bus напряму.
 * Зв'язок встановлюється через ModuleManager при реєстрації.
 */

#pragma once

#include "modesp/types.h"
#include "features_config.h"
#include "etl/message.h"
#include "etl/optional.h"
#include "etl/array.h"
#include "etl/span.h"

namespace modesp {

// Forward declarations — модуль не знає деталей реалізації
class ModuleManager;
class SharedState;

/// Cross-module input binding: maps a role name to a SharedState key
struct InputBinding {
    const char* role;         ///< e.g., "air_temp"
    const char* default_key;  ///< e.g., "equipment.air_temp"
};

class BaseModule {
public:
    /// Original constructor — backward compatible (ns = name)
    BaseModule(const char* name, ModulePriority priority = ModulePriority::NORMAL)
        : name_(name)
        , ns_(name)
        , priority_(priority)
    {}

    /// Namespace constructor — for multi-instance modules
    /// @param name  Module type name (for features lookup, e.g., "thermostat")
    /// @param ns    Namespace for state keys (e.g., "thermo_z1")
    /// @param priority  Update priority
    /// @param inputs  Cross-module input bindings (optional)
    BaseModule(const char* name, const char* ns, ModulePriority priority,
               etl::span<const InputBinding> inputs = {})
        : name_(name)
        , ns_(ns)
        , priority_(priority)
        , inputs_(inputs)
    {}

    virtual ~BaseModule() = default;

    // ── Lifecycle (перевизначаються модулем) ──
    virtual bool on_init()                            { return true; }
    virtual void on_update(uint32_t dt_ms)            {}
    virtual void on_message(const etl::imessage& msg) {}
    virtual void on_stop()                            {}

    // ── Ідентифікація ──
    const char*    name()     const { return name_; }
    ModulePriority priority() const { return priority_; }

    // ── Стан модуля ──
    enum class State : uint8_t {
        CREATED,
        INITIALIZED,
        RUNNING,
        STOPPED,
        ERROR
    };
    State state() const { return state_; }

    // ── Heartbeat (оновлюється автоматично ModuleManager) ──
    uint32_t last_update_ms() const { return last_update_ms_; }

protected:
    // ── API для модулів (доступний після реєстрації) ──
    
    /// Відправити повідомлення на шину (broadcast усім підписникам).
    ///
    /// ВИКОРИСТОВУЙ ЛИШЕ для:
    ///   1. Transient signals без state representation (alarm fired, watchdog timeout)
    ///   2. Broadcast fan-out де ≥2 модулі мають реагувати синхронно (SAFE_MODE)
    ///   3. Discrete events частотою <1Hz (defrost lifecycle markers)
    ///
    /// НЕ ВИКОРИСТОВУЙ для:
    ///   - State updates (setpoint, mode, FSM state) → SharedState + sync_settings()
    ///   - High-frequency data (sensor readings) → SharedState
    ///   - Request-response 1→1 → прямий method call або InputBinding
    ///
    /// Повна документація: docs/13_message_bus.md
    void publish(const etl::imessage& msg);

    // Записати значення в SharedState
    /// @param track_change false = тихий set (не тригерить WS broadcast)
    bool state_set(const StateKey& key, const StateValue& value, bool track_change = true);

    // Зручні обгортки для state_set
    bool state_set(const char* key, int32_t value, bool track_change = true);
    bool state_set(const char* key, float value, bool track_change = true);
    bool state_set(const char* key, bool value, bool track_change = true);
    bool state_set(const char* key, const char* value, bool track_change = true);

    // Прочитати значення з SharedState
    etl::optional<StateValue> state_get(const StateKey& key) const;
    etl::optional<StateValue> state_get(const char* key) const;

    // BUG-013: типізовані reader-и (замість дублювання в кожному модулі)
    float read_float(const char* key, float def = 0.0f) const {
        auto v = state_get(key);
        if (!v.has_value()) return def;
        const auto* fp = etl::get_if<float>(&v.value());
        return fp ? *fp : def;
    }
    bool read_bool(const char* key, bool def = false) const {
        auto v = state_get(key);
        if (!v.has_value()) return def;
        const auto* bp = etl::get_if<bool>(&v.value());
        return bp ? *bp : def;
    }
    int32_t read_int(const char* key, int32_t def = 0) const {
        auto v = state_get(key);
        if (!v.has_value()) return def;
        const auto* ip = etl::get_if<int32_t>(&v.value());
        return ip ? *ip : def;
    }

    // Перевірити чи feature активна (з features_config.h)
    bool has_feature(const char* feature_name) const {
        return modesp::gen::is_feature_active(name(), feature_name);
    }

    // ── Namespace support (Block A) ──

    /// Get the namespace prefix (e.g., "thermostat" or "thermo_z1")
    const char* ns() const { return ns_; }

    /// Construct namespaced key: ns_ + "." + short_key
    /// Returns pointer to internal buffer (valid until next ns_key() call)
    const char* ns_key(const char* short_key) {
        ns_buf_[0] = '\0';
        // Manual concatenation (no heap, no snprintf dependency)
        char* p = ns_buf_;
        const char* s = ns_;
        while (*s && p < ns_buf_ + sizeof(ns_buf_) - 2) *p++ = *s++;
        *p++ = '.';
        s = short_key;
        while (*s && p < ns_buf_ + sizeof(ns_buf_) - 1) *p++ = *s++;
        *p = '\0';
        return ns_buf_;
    }

    /// Check if this module's zone is enabled (runtime activation).
    /// Zone 1 modules have no "equipment.zone_enabled" binding → default true (always active).
    /// Zone 2+ modules bind "equipment.zone_enabled" → "equipment.zone2_enabled" etc.
    bool is_zone_enabled() const {
        return read_input_bool("equipment.zone_enabled", true);
    }

    /// Resolve all keys for a module from short names array into pre-built keys
    /// Called once in on_init(). After this, keys[KeyEnum] is a fully-qualified StateKey.
    /// @param short_names  Array of short key names (from generated module_keys.h)
    /// @param keys         Output array to fill with resolved "ns.short_name" strings
    /// @param count        Number of keys to resolve
    template<size_t N>
    void resolve_keys(const char* const short_names[], etl::array<StateKey, N>& keys, size_t count) {
        for (size_t i = 0; i < count && i < N; ++i) {
            keys[i].clear();
            keys[i].append(ns_);
            keys[i].append(".");
            keys[i].append(short_names[i]);
        }
    }

    /// Read a cross-module input by role name.
    /// Looks up role in InputBinding list, falls back to default_key.
    /// @param role   Input role name (e.g., "air_temp")
    /// @param def    Default value if key not found in SharedState
    float read_input_float(const char* role, float def = 0.0f) const {
        return read_float(resolve_input(role), def);
    }
    bool read_input_bool(const char* role, bool def = false) const {
        return read_bool(resolve_input(role), def);
    }
    int32_t read_input_int(const char* role, int32_t def = 0) const {
        return read_int(resolve_input(role), def);
    }

protected:
    /// Resolve input role to SharedState key via InputBinding lookup
    const char* resolve_input(const char* role) const {
        for (const auto& b : inputs_) {
            // Simple strcmp
            const char* a = b.role;
            const char* r = role;
            while (*a && *a == *r) { a++; r++; }
            if (*a == *r) return b.default_key;
        }
        return role;  // fallback: use role as-is
    }

private:
    friend class ModuleManager;  // ModuleManager встановлює зв'язки

    const char*    name_;
    const char*    ns_;                          ///< Namespace for state keys (== name_ by default)
    ModulePriority priority_;
    etl::span<const InputBinding> inputs_ = {};  ///< Cross-module input bindings
    State          state_ = State::CREATED;
    volatile uint32_t last_update_ms_ = 0;

    // Namespace key buffer (for ns_key() — single-threaded, reused)
    char ns_buf_[64] = {};

    // Зворотні посилання (встановлює ModuleManager при реєстрації)
    ModuleManager* manager_ = nullptr;
    SharedState*   shared_state_ = nullptr;
};

} // namespace modesp
