/**
 * @file main.cpp
 * @brief ModESP v4 — Phase 9.1: Equipment Layer
 *
 * Staged boot sequence:
 *   1. NVS init (required for WiFi credentials)
 *   2. Register system services -> init_all (phase 1)
 *   3. ConfigService reads board.json + bindings.json
 *   4. Register WiFi service
 *   5. HAL initializes GPIO from BoardConfig
 *   6. DriverManager creates drivers from bindings
 *   7. Register EquipmentModule (binds drivers) + business modules -> init_all (phase 2)
 *   8. Inject dependencies, register HTTP + WS -> init_all (phase 3)
 *   9. Connect WS handler to HTTP server
 *  10. Main loop: drivers update -> modules update -> WDT reset
 *
 * Three-phase init works because module_manager.init_all()
 * skips modules with state != CREATED.
 */

#include "modesp/app.h"
#include "modesp/base_module.h"

// Phase 2: System services
#include "modesp/error_service.h"
#include "modesp/watchdog_service.h"
#include "modesp/system_monitor.h"
#include "modesp/logger_service.h"

// Phase 4: Configuration + HAL + Persist
#include "modesp/services/config_service.h"
#include "modesp/services/persist_service.h"
#include "modesp/hal/hal.h"
#include "modesp/hal/driver_manager.h"

// Phase 5a: Network
#include "modesp/net/wifi_service.h"
#include "modesp/net/http_service.h"
#include "modesp/net/ws_service.h"
#include "modesp/services/nvs_helper.h"

// Cloud backend (compile-time Kconfig choice)
#if defined(CONFIG_MODESP_CLOUD_AWS)
  #include "modesp/net/aws_iot_service.h"
#else
  #include "modesp/net/mqtt_service.h"
#endif

// Modbus RTU Slave (optional, compile-time Kconfig)
#if defined(CONFIG_MODESP_MODBUS_ENABLED)
  #include "modesp/modbus/modbus_service.h"
#endif

// Equipment Layer + Business modules
#include "equipment_module.h"
#include "protection_module.h"
#include "thermostat_module.h"
#include "defrost_module.h"
#include "lighting_module.h"
#include "datalogger_module.h"
#include "eev_module.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "main";

// ═══════════════════════════════════════════════════════════════
// Static instances (zero heap allocation)
// ═══════════════════════════════════════════════════════════════

// System services (CRITICAL priority — start first)
static modesp::ErrorService    error_service;
static modesp::LoggerService   logger_service;
static modesp::ConfigService   config_service;
static modesp::PersistService  persist_service;

// Services with dependencies
static modesp::SystemMonitor   system_monitor(error_service);

// HAL + DriverManager (not BaseModule — managed directly)
static modesp::HAL             hal;
static modesp::DriverManager   driver_manager;

// Network services
static modesp::WiFiService     wifi_service;
static modesp::HttpService     http_service;
static modesp::WsService       ws_service;

// Cloud backend (compile-time Kconfig choice)
#if defined(CONFIG_MODESP_CLOUD_AWS)
static modesp::AwsIotService   cloud_service;
#else
static modesp::MqttService     cloud_service;
#endif

// Modbus RTU Slave (optional)
#if defined(CONFIG_MODESP_MODBUS_ENABLED)
static modesp::ModbusService   modbus_service;
#endif

// Equipment Layer (CRITICAL priority — owns all HAL drivers)
static EquipmentModule         equipment;

// Protection (HIGH priority — alarm monitoring, runs before thermostat)
// Zone 1 = primary (owns compressor tracker), Zone 2 = temperature alarms only

static constexpr modesp::InputBinding z1_prot_inputs[] = {
    {"equipment.air_temp",       "equipment.air_temp"},
    {"equipment.sensor1_ok",     "equipment.sensor1_ok"},
    {"equipment.sensor2_ok",     "equipment.sensor2_ok"},
    {"equipment.door_open",      "equipment.door_open"},
    {"equipment.compressor",     "equipment.compressor"},
    {"defrost.active",           "defrost_z1.active"},
    {"defrost.phase",            "defrost_z1.phase"},
    {"defrost.type",             "defrost_z1.type"},
    {"defrost.demand_temp",      "defrost_z1.demand_temp"},
    {"defrost.manual_start",     "defrost_z1.manual_start"},
    {"thermostat.cc_remaining",  "thermo_z1.cc_remaining"},
    {"thermostat.cc_alarm_bypass","thermo_z1.cc_alarm_bypass"},
};
static constexpr modesp::InputBinding z2_prot_inputs[] = {
    {"equipment.zone_enabled",   "equipment.zone2_enabled"},
    {"equipment.air_temp",       "equipment.air_temp_z2"},
    {"equipment.sensor1_ok",     "equipment.sensor1_z2_ok"},
    {"equipment.sensor2_ok",     "equipment.sensor2_z2_ok"},
    {"equipment.door_open",      "equipment.door_open"},
    {"equipment.compressor",     "equipment.compressor"},
    {"defrost.active",           "defrost_z2.active"},
    {"defrost.phase",            "defrost_z2.phase"},
    {"thermostat.cc_remaining",  "thermo_z2.cc_remaining"},
    {"thermostat.cc_alarm_bypass","thermo_z2.cc_alarm_bypass"},
};

static ProtectionModule protection(
    "protection", z1_prot_inputs, true);   // primary — compressor tracker
static ProtectionModule protection_z2(
    "protection_z2", z2_prot_inputs, false); // secondary — temp alarms only

// Business modules (NORMAL priority — work through SharedState)
// Always compiled for 2 zones. Zone 2 activation is RUNTIME via WebUI.
// Zone 1 = always active. Zone 2 = enabled via equipment.active_zones setting.

// ── Zone 1 InputBindings ──
static constexpr modesp::InputBinding z1_thermo_inputs[] = {
    {"equipment.air_temp",     "equipment.air_temp"},       // zone 1 = global air temp
    {"equipment.evap_temp",    "equipment.evap_temp_z1"},  // per-zone
    {"equipment.sensor1_ok",   "equipment.sensor1_ok"},    // zone 1 = global sensor health
    {"equipment.sensor2_ok",   "equipment.sensor2_z1_ok"}, // per-zone
    {"equipment.compressor",   "equipment.compressor"},    // shared
    {"equipment.night_input",  "equipment.night_input"},   // shared
    {"defrost.active",         "defrost_z1.active"},       // per-zone
    {"protection.lockout",     "protection.lockout"},      // shared
};
static constexpr modesp::InputBinding z1_defrost_inputs[] = {
    {"equipment.compressor",   "equipment.compressor"},
    {"equipment.evap_temp",    "equipment.evap_temp_z1"},
    {"equipment.has_defrost_relay", "equipment.has_defrost_relay_z1"},
    {"protection.lockout",     "protection.lockout"},
    {"protection.compressor_blocked", "protection.compressor_blocked"},
};
static constexpr modesp::InputBinding z1_eev_inputs[] = {
    {"equipment.compressor",   "equipment.compressor"},
    {"equipment.evap_temp",    "equipment.evap_temp_z1"},
    {"equipment.suction_bar",  "equipment.suction_bar_z1"},
    {"equipment.has_suction_p","equipment.has_suction_p_z1"},
    {"equipment.refrigerant",  "equipment.refrigerant"},
    {"defrost.active",         "defrost_z1.active"},
};

// ── Zone 2 InputBindings (includes zone_enabled check) ──
static constexpr modesp::InputBinding z2_thermo_inputs[] = {
    {"equipment.zone_enabled", "equipment.zone2_enabled"},  // runtime enable
    {"equipment.air_temp",     "equipment.air_temp_z2"},   // zone 2 has own air temp sensor
    {"equipment.evap_temp",    "equipment.evap_temp_z2"},
    {"equipment.sensor1_ok",   "equipment.sensor1_z2_ok"}, // zone 2 sensor health
    {"equipment.sensor2_ok",   "equipment.sensor2_z2_ok"},
    {"equipment.compressor",   "equipment.compressor"},
    {"equipment.night_input",  "equipment.night_input"},
    {"defrost.active",         "defrost_z2.active"},
    {"protection.lockout",     "protection.lockout"},
};
static constexpr modesp::InputBinding z2_defrost_inputs[] = {
    {"equipment.zone_enabled", "equipment.zone2_enabled"},
    {"equipment.compressor",   "equipment.compressor"},
    {"equipment.evap_temp",    "equipment.evap_temp_z2"},
    {"equipment.has_defrost_relay", "equipment.has_defrost_relay_z2"},
    {"protection.lockout",     "protection.lockout"},
    {"protection.compressor_blocked", "protection.compressor_blocked"},
};
static constexpr modesp::InputBinding z2_eev_inputs[] = {
    {"equipment.zone_enabled", "equipment.zone2_enabled"},
    {"equipment.compressor",   "equipment.compressor"},
    {"equipment.evap_temp",    "equipment.evap_temp_z2"},
    {"equipment.suction_bar",  "equipment.suction_bar_z2"},
    {"equipment.has_suction_p","equipment.has_suction_p_z2"},
    {"equipment.refrigerant",  "equipment.refrigerant"},
    {"defrost.active",         "defrost_z2.active"},
};

// Zone 1 — always active
static ThermostatModule thermostat_z1("thermo_z1", z1_thermo_inputs);
static DefrostModule    defrost_z1("defrost_z1", z1_defrost_inputs);
static EevModule        eev_z1("eev_z1", z1_eev_inputs);

// Zone 2 — runtime activation via equipment.active_zones
static ThermostatModule thermostat_z2("thermo_z2", z2_thermo_inputs);
static DefrostModule    defrost_z2("defrost_z2", z2_defrost_inputs);
static EevModule        eev_z2("eev_z2", z2_eev_inputs);

// Lighting (NORMAL priority — chamber light control, always single)
static LightingModule          lighting;

// DataLogger (LOW priority — logging, runs after business logic)
static DataLoggerModule        datalogger;

// ═══════════════════════════════════════════════════════════════
// Entry point
// ═══════════════════════════════════════════════════════════════

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "  ModESP v4.0.0 — Phase 5a");
    ESP_LOGI(TAG, "  WiFi + HTTP API + WebSocket");
    ESP_LOGI(TAG, "======================================");

    // ── Step 0: NVS init (before everything) ──
    modesp::nvs_helper::init();

    // ── Step 0b: OTA validity — deferred validation (60s timeout) ──
    const esp_partition_t* running = esp_ota_get_running_partition();
    bool ota_pending_verify = false;
    {
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ota_pending_verify = true;
                ESP_LOGW(TAG, "OTA: New firmware — validating (60s timeout)...");
            }
        }
    }
    ESP_LOGI(TAG, "Running from: %s (0x%lx)",
             running->label, (unsigned long)running->address);

    auto& app = modesp::App::instance();

    // ── Step 1: Core init ──
    if (!app.init()) {
        ESP_LOGE(TAG, "App init failed!");
        return;
    }

    // Публікуємо інформацію про прошивку в SharedState
    {
        const esp_app_desc_t* desc = esp_app_get_description();
        app.state().set("_ota.version", desc->version);
        app.state().set("_ota.partition", running->label);
        app.state().set("_ota.idf", desc->idf_ver);

        // Дата + час збірки
        char build_dt[32];
        snprintf(build_dt, sizeof(build_dt), "%s %s", desc->date, desc->time);
        app.state().set("_ota.date", build_dt);
        app.state().set("_ota.board", desc->project_name);

        // OTA статус для cloud feedback (оновлюється з ota_handler.cpp)
        app.state().set("_ota.status", "idle");
        app.state().set("_ota.progress", static_cast<int32_t>(0));
        app.state().set("_ota.error", "");
    }

    // WatchdogService needs ModuleManager reference
    static modesp::WatchdogService watchdog_service(error_service, app.modules());

    // ── Step 2: Register system services (CRITICAL) ──
    app.modules().register_module(error_service);
    app.modules().register_module(watchdog_service);
    app.modules().register_module(logger_service);
    app.modules().register_module(config_service);

    // PersistService: відновлює persisted settings з NVS → SharedState
    // Повинен ініціалізуватися ДО бізнес-модулів (Phase 2)
    persist_service.set_state(&app.state());
    app.modules().register_module(persist_service);

    app.modules().register_module(system_monitor);

    ESP_LOGI(TAG, "Phase 1: Initializing system services...");
    app.modules().init_all(app.state());

    // ── Step 3: Read config ──
    const auto& board_cfg = config_service.board_config();
    const auto& bindings  = config_service.binding_table();

    ESP_LOGI(TAG, "Board: %s v%s (%d gpio_out, %d onewire buses)",
             board_cfg.board_name.c_str(),
             board_cfg.board_version.c_str(),
             (int)board_cfg.gpio_outputs.size(),
             (int)board_cfg.onewire_buses.size());

    // ── Step 4: Register WiFi + MQTT (HIGH priority) ──
    app.modules().register_module(wifi_service);
    cloud_service.set_state(&app.state());
    cloud_service.set_backfill_provider(&datalogger);
    app.modules().register_module(cloud_service);

#if defined(CONFIG_MODESP_MODBUS_ENABLED)
    modbus_service.set_state(&app.state());
    app.modules().register_module(modbus_service);
#endif

    // ── Step 5: Initialize HAL (GPIO setup) ──
    if (!hal.init(board_cfg)) {
        ESP_LOGE(TAG, "HAL init failed!");
        return;
    }

    // ── Step 6: Create and init all drivers ──
    if (!driver_manager.init(bindings, hal)) {
        ESP_LOGE(TAG, "DriverManager init failed!");
        return;
    }
    ESP_LOGI(TAG, "Drivers: %d sensors, %d actuators",
             (int)driver_manager.sensor_count(),
             (int)driver_manager.actuator_count());

    // ── Step 7: Register Equipment Manager + business modules ──
    // Read active_zones ONCE — used for both Equipment and zone module registration.
    // PersistService already restored this from NVS in Phase 1.
    int32_t active_zones = 1;
    {
        auto val = app.state().get("equipment.active_zones");
        if (val.has_value()) {
            const auto* iv = etl::get_if<int32_t>(&val.value());
            if (iv) active_zones = *iv;
        }
    }

    // EM — єдиний модуль з доступом до HAL (CRITICAL priority)
    equipment.set_zone_count(static_cast<size_t>(active_zones));
    equipment.bind_drivers(driver_manager);
    app.modules().register_module(equipment);

    // Protection — моніторинг аварій (HIGH priority, перед thermostat)
    app.modules().register_module(protection);

    // Thermostat + Defrost + EEV — Zone 1 always, Zone 2 conditional.
    // Zone change requires restart (промисловий стандарт: Danfoss/CAREL теж).
    app.modules().register_module(thermostat_z1);
    app.modules().register_module(defrost_z1);
    app.modules().register_module(eev_z1);

    // Zone 2 — conditional registration
    if (active_zones >= 2) {
        app.modules().register_module(protection_z2);
        app.modules().register_module(thermostat_z2);
        app.modules().register_module(defrost_z2);
        app.modules().register_module(eev_z2);
        ESP_LOGI(TAG, "Zone 2 modules registered (active_zones=%ld)",
                 static_cast<long>(active_zones));
    } else {
        ESP_LOGI(TAG, "Single zone mode (active_zones=%ld)",
                 static_cast<long>(active_zones));
    }

    // Lighting — освітлення камери (NORMAL priority)
    app.modules().register_module(lighting);

    // DataLogger — логування температури та подій (LOW priority)
    app.modules().register_module(datalogger);

    ESP_LOGI(TAG, "Phase 2: Initializing WiFi + business modules...");
    app.modules().init_all(app.state());

    // ── Step 7b: NTP time sync ──
    {
        bool ntp_enabled = true;
        modesp::nvs_helper::read_bool("time", "ntp_enabled", ntp_enabled);

        char tz[48] = "EET-2EEST,M3.5.0/3,M10.5.0/4";  // Київ
        modesp::nvs_helper::read_str("time", "timezone", tz, sizeof(tz));
        setenv("TZ", tz, 1);
        tzset();

        if (ntp_enabled) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP started (TZ=%s)", tz);
        } else {
            ESP_LOGI(TAG, "NTP disabled, manual time mode (TZ=%s)", tz);
        }
        app.state().set("system.time", "--:--:--");
        app.state().set("system.date", "--.--.----");
    }

    // ── Step 8: Inject dependencies + register HTTP + WS ──
    http_service.set_state(&app.state());
    http_service.set_config(&config_service);
    http_service.set_modules(&app.modules());
    http_service.set_wifi(&wifi_service);
    http_service.set_persist(&persist_service);
    http_service.set_hal(&hal);
    http_service.set_datalogger(&datalogger);

    ws_service.set_state(&app.state());

    app.modules().register_module(http_service);
    app.modules().register_module(ws_service);

    ESP_LOGI(TAG, "Phase 3: Initializing HTTP + WebSocket...");
    app.modules().init_all(app.state());

    // ── Step 9: Connect WS handler + finalize HTTP routes ──
    // Order matters: WS handler must be registered BEFORE the
    // wildcard static handler (/*), otherwise httpd_uri_match_wildcard
    // considers /* as already matching /ws for HTTP_GET.
    if (http_service.server()) {
        ws_service.set_http_server(http_service.server());
        cloud_service.set_http_server(http_service.server());
#if defined(CONFIG_MODESP_MODBUS_ENABLED)
        modbus_service.set_http_server(http_service.server());
#endif
        http_service.register_static_handler();  // Must be last (wildcard catch-all)
    } else {
        ESP_LOGW(TAG, "HTTP server not started, WebSocket unavailable");
    }

    // ── Step 10: Main loop ──
    ESP_LOGI(TAG, "Registered %d modules total", (int)app.modules().module_count());
    ESP_LOGI(TAG, "Free heap after init: %lu bytes", esp_get_free_heap_size());

    // HW Watchdog — ESP-IDF v5.x auto-ініціалізує TWDT, тому reconfigure
    bool wdt_subscribed = false;
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms  = MODESP_WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (wdt_err != ESP_OK) {
        // TWDT не ініціалізований — створюємо
        wdt_err = esp_task_wdt_init(&wdt_cfg);
    }
    if (wdt_err == ESP_OK) {
        esp_err_t add_err = esp_task_wdt_add(nullptr);
        if (add_err == ESP_OK) {
            wdt_subscribed = true;
            ESP_LOGI(TAG, "HW watchdog: %d ms timeout", MODESP_WDT_TIMEOUT_MS);
        } else if (add_err == ESP_ERR_INVALID_STATE) {
            // Задача вже підписана — ОК
            wdt_subscribed = true;
            ESP_LOGI(TAG, "HW watchdog: task already subscribed (%d ms)", MODESP_WDT_TIMEOUT_MS);
        } else {
            ESP_LOGW(TAG, "HW watchdog: add task failed: %s", esp_err_to_name(add_err));
        }
    } else {
        ESP_LOGW(TAG, "HW watchdog init failed: %s", esp_err_to_name(wdt_err));
    }

    constexpr int LOOP_HZ = MODESP_MAIN_LOOP_HZ;
    const TickType_t period = pdMS_TO_TICKS(1000 / LOOP_HZ);
    const uint32_t dt_ms = 1000 / LOOP_HZ;

    ESP_LOGI(TAG, "Entering main loop (%d Hz)...", LOOP_HZ);

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        // 1. Update all drivers (sensors read, actuators apply)
        driver_manager.update_all(dt_ms);

        // 2. Update all modules (business logic reads from drivers)
        app.modules().update_all(dt_ms);

        // 3. OTA rollback validation (60s timeout)
        if (ota_pending_verify) {
            bool net_ok = wifi_service.is_connected() && http_service.server();
            if (net_ok) {
                esp_ota_mark_app_valid_cancel_rollback();
                ota_pending_verify = false;
                ESP_LOGI(TAG, "OTA: Firmware validated (WiFi + HTTP OK after %lus)",
                         (unsigned long)app.uptime_sec());
            } else if (app.uptime_sec() > 60) {
                ESP_LOGE(TAG, "OTA: Validation timeout (60s) — rollback on next reboot");
                esp_restart();
            }
        }

        // 4. Uptime + heap diagnostics in SharedState (once per second)
        static uint32_t sec_counter = 0;
        sec_counter += dt_ms;
        if (sec_counter >= 1000) {
            sec_counter = 0;
            // track_change=false: діагностичні ключі не тригерять WS delta
            app.state().set("system.uptime",
                            static_cast<int32_t>(app.uptime_sec()), false);
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            app.state().set("system.heap_largest",
                            static_cast<int32_t>(largest), false);
        }

        // 5. HW watchdog reset
        if (wdt_subscribed) esp_task_wdt_reset();

        // 6. Precise loop timing
        vTaskDelayUntil(&last_wake, period);
    }
}
