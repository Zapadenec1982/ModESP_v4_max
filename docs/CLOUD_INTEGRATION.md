# ModESP Cloud Integration

How ModESP v4 firmware communicates with the ModESP Cloud platform.

## 1. Overview

ModESP Cloud is a remote monitoring and management platform for ModESP v4 refrigeration controllers. Each ESP32 device connects to the cloud via MQTT over TLS. The firmware publishes telemetry (sensor readings, relay states, alarms) and subscribes to command topics for remote parameter changes and OTA updates.

The MQTT client is implemented in `MqttService` (`components/modesp_mqtt/`), a `BaseModule` with `ModulePriority::HIGH`. It is disabled by default and must be enabled via the local HTTP API (`POST /api/mqtt`) or NVS configuration. Configuration is stored in the `mqtt` NVS namespace.

Connection flow:
1. `MqttService::on_init()` loads config from NVS and waits for WiFi.
2. `on_update()` detects `wifi.connected == true` and calls `start_client()`.
3. On successful connect: publishes `online` status (retained), subscribes to `{prefix}/cmd/+`, publishes alarm states, schedules HA discovery.
4. Telemetry is published every 1 second (only changed values). Heartbeat every 30 seconds.

## 2. MQTT Topic Structure

All topics use the prefix `modesp/v1/{tenant}/{device_id}` where:
- `{tenant}` is the organization slug assigned by the cloud (e.g., `acme-foods`)
- `{device_id}` is a 6-character hex string derived from the last 3 bytes of the WiFi SoftAP MAC address (e.g., `A4CF12`)

If no tenant is assigned yet, the prefix is `modesp/v1/pending/{device_id}`.

The prefix can be overridden manually via NVS or the HTTP API.

### Topics

| Topic | Direction | QoS | Retain | Description |
|---|---|---|---|---|
| `{prefix}/state/{key}` | Device -> Cloud | 0 | No | Telemetry: scalar value per key |
| `{prefix}/state/protection.*` | Device -> Cloud | 1 | Yes | Alarm states (retained for reliability) |
| `{prefix}/cmd/{key}` | Cloud -> Device | 0 | - | Commands: set parameter value |
| `{prefix}/status` | Device -> Cloud | 1 | Yes | LWT: `online` on connect, `offline` on ungraceful disconnect |
| `{prefix}/heartbeat` | Device -> Cloud | 0 | No | JSON metadata every 30s |
| `{prefix}/state/_ota.*` | Device -> Cloud | 0 | No | OTA progress: `_ota.status`, `_ota.progress`, `_ota.error` |

The device subscribes using a single wildcard: `{prefix}/cmd/+`. Incoming keys are validated against `STATE_META` (generated from module manifests). Unknown or non-writable keys are rejected with a log warning.

## 3. Authentication and Provisioning

### Broker Credentials

MQTT credentials are stored in NVS namespace `mqtt`:

| NVS Key | Type | Description |
|---|---|---|
| `broker` | string | Broker hostname or URI (`mqtt://` or `mqtts://`) |
| `port` | int32 | Port number (1883 for plain, 8883 for TLS) |
| `user` | string | MQTT username |
| `pass` | string | MQTT password |
| `tenant` | string | Tenant slug |
| `prefix` | string | Topic prefix override (empty = auto-generated) |
| `enabled` | bool | Enable/disable MQTT |

Credentials can be configured via:
- `POST /api/mqtt` (local HTTP API, requires auth)
- MQTT command `cmd/_set_mqtt_creds` (JSON payload: `{"user":"...","pass":"..."}`)
- MQTT command `cmd/_set_tenant` (plain text payload)

### Auto-Discovery Flow

1. A new device connects to the cloud broker using a shared bootstrap MQTT credential.
2. The topic prefix is `modesp/v1/pending/{device_id}`.
3. The cloud platform detects the new device from telemetry on the `pending` tenant.
4. Cloud sends `cmd/_set_mqtt_creds` with device-specific credentials (JSON: `{"user":"...","pass":"..."}`). Credentials are saved to NVS but the device does not reconnect yet.
5. Cloud sends `cmd/_set_tenant` with the assigned tenant slug. This triggers a reconnect with the new credentials and new prefix `modesp/v1/{tenant}/{device_id}`.
6. After reconnect, the device publishes telemetry under its assigned tenant.

### Full State Request

Cloud can request a complete re-publish of all state by sending any payload to `cmd/_request_full_state`. This clears the delta-publish cache and forces publication of all 50 read-only keys plus all 62 writable parameters.

## 4. Telemetry

### Published Keys (50 keys)

Telemetry keys are auto-generated from module manifests into `generated/mqtt_topics.h`. The 50 published keys cover:

- **equipment** (6): `air_temp`, `evap_temp`, `cond_temp`, `compressor`, `defrost_relay`, `sensor1_ok`
- **protection** (21): `lockout`, `alarm_active`, `alarm_code`, 10 alarm flags, compressor statistics (`starts_1h`, `duty`, `run_time`, `hours`, etc.)
- **thermostat** (11): `temperature`, `req.compressor`, `req.evap_fan`, `req.cond_fan`, `state`, `comp_on_time`, `comp_off_time`, `night_active`, `effective_setpoint`, `display_temp`
- **defrost** (9): `active`, `phase`, `state`, `phase_timer`, `interval_timer`, `defrost_count`, `last_termination`, `consecutive_timeouts`, `req.compressor`, `req.defrost_relay`
- **datalogger** (3): `records_count`, `events_count`, `flash_used`

Additionally, 3 system keys are published during OTA: `_ota.status`, `_ota.progress`, `_ota.error`.

### Delta-Publish

To minimize bandwidth, only values that have changed since the last publish cycle are sent. The `MqttService` maintains a cache of last published payloads (`last_payloads_[56][16]` — 16 bytes per key). Each publish cycle (every 1 second), the service compares the current formatted value against the cache. If identical, the publish is skipped.

On initial connection, `last_version_` is reset to 0, forcing a full publish of all keys.

### Value Formatting

All values are published as plain-text scalars (not JSON):
- `float`: formatted with `%.2f` (e.g., `4.50`)
- `int32_t`: formatted with `%ld` (e.g., `120`)
- `bool`: `true` or `false`
- `string`: as-is (e.g., `cooling`)

### Alarm Re-Publish

Protection alarm topics (`protection.*`) are re-published every 5 minutes with QoS 1 and retain flag, ensuring the cloud always has current alarm state even after broker restart.

## 5. Commands

### Subscribed Keys (62 keys)

The device subscribes to 62 writable parameter keys, auto-generated from manifests into `generated/mqtt_topics.h`. These cover all configurable parameters across modules:

- **equipment** (5): NTC calibration, DS18B20 offset, filter coefficient
- **protection** (18): alarm limits, delays, compressor protection thresholds
- **thermostat** (16): setpoint, differential, fan modes, night mode settings
- **defrost** (14): type, interval, termination, timing parameters
- **datalogger** (7): enable, retention, sample interval, channel selection

### Validation

Every incoming command is validated against `STATE_META` (63 entries in `generated/state_meta.h`):

1. **Key existence**: must be present in `STATE_META` (linear search via `find_state_meta()`).
2. **Writable flag**: `meta->writable` must be `true`.
3. **Type parsing**: value is parsed according to `meta->type` (`float`, `int`, `bool`).
4. **Range clamping**: numeric values are clamped to `[meta->min_val, meta->max_val]`.

If validation passes, the value is written to `SharedState` via `state_set()`. Modules with `persist: true` in their manifest automatically persist the value to NVS on the next update cycle.

### Special Commands

These commands are not in `STATE_META` and are handled before validation:

| Command | Payload | Description |
|---|---|---|
| `cmd/_ota` | JSON: `{"url":"...","version":"...","checksum":"..."}` | Trigger OTA firmware update |
| `cmd/_set_tenant` | Plain text tenant slug | Set tenant, rebuild prefix, reconnect |
| `cmd/_set_mqtt_creds` | JSON: `{"user":"...","pass":"..."}` | Update MQTT credentials in NVS |
| `cmd/_request_full_state` | Any | Force re-publish of all state keys |

## 6. Heartbeat

The device publishes a JSON heartbeat every 30 seconds to `{prefix}/heartbeat`:

```json
{
  "proto": 1,
  "fw": "1.2.3",
  "up": 86400,
  "heap": 142000,
  "rssi": -65
}
```

| Field | Type | Description |
|---|---|---|
| `proto` | int | Protocol version (always `1`) |
| `fw` | string | Firmware version from `esp_app_desc_t` |
| `up` | int | Uptime in seconds (`esp_timer_get_time() / 1000000`) |
| `heap` | int | Free heap in bytes (`esp_get_free_heap_size()`) |
| `rssi` | int | WiFi signal strength in dBm |

Heartbeat is published at QoS 0 without retain. The first heartbeat is sent immediately on connect.

## 7. OTA Flow

OTA is triggered by sending a JSON payload to `cmd/_ota`:

```json
{
  "url": "https://cdn.modesp.cloud/fw/modesp4/1.3.0/firmware.bin",
  "version": "1.3.0",
  "checksum": "sha256:a1b2c3d4..."
}
```

### Execution

OTA runs in a separate FreeRTOS task (8 KB stack, priority 5) to avoid blocking the MQTT event handler. Only one OTA can run at a time (atomic guard).

**Steps:**

1. **Partition lookup** — find the next OTA partition (`ota_0` or `ota_1`, each 1472 KB).
2. **HTTP download** — open connection to the URL with 30-second timeout and 4 KB buffer.
3. **Magic byte check** — first byte must be `0xE9` (ESP image header).
4. **Board match** — project name in the firmware's `esp_app_desc_t` must match the running firmware. Prevents flashing wrong firmware to a device.
5. **Streaming write** — data is written to the OTA partition as it downloads, with progress reported to `_ota.progress` (0-100%).
6. **SHA256 verification** — if a checksum was provided, the full download is verified against it. The `sha256:` prefix in the checksum string is optional.
7. **ESP-IDF validation** — `esp_ota_end()` performs internal CRC32 validation.
8. **Boot partition set** — `esp_ota_set_boot_partition()` marks the new partition as bootable.
9. **Reboot** — device restarts after a 2-second delay (allows MQTT to publish final status).

### Status Feedback

OTA progress is published via `SharedState` keys that appear on MQTT:

| Key | Values |
|---|---|
| `_ota.status` | `downloading`, `verifying`, `rebooting`, `error` |
| `_ota.progress` | `0` to `100` |
| `_ota.error` | Error description or empty string |

### Error Cases

- HTTP connection failure or non-200 status
- Firmware too large for partition
- Invalid magic byte (not an ESP firmware)
- Board/project name mismatch
- SHA256 checksum mismatch
- Flash write failure
- Firmware too small (< 256 bytes)

### Partition Layout

```
ota_0:  0x020000 - 0x18FFFF  (1472 KB)
ota_1:  0x190000 - 0x2FFFFF  (1472 KB)
```

The device alternates between `ota_0` and `ota_1`. The currently running partition is preserved; the update is written to the other one.

## 8. TLS

When the broker port is 8883 or the URI starts with `mqtts://`, TLS is enabled automatically:

```cpp
mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
```

This uses the ESP-IDF built-in CA certificate bundle, which includes Let's Encrypt and other major CAs. No client certificates are needed for the Mosquitto-based cloud backend (authentication is via username/password).

The MQTT task stack is set to 4 KB, with 1 KB inbound and 1 KB outbound buffers. Auto-reconnect is disabled (managed manually with exponential backoff).

## 9. WiFi Resilience

The firmware is designed to maintain connectivity in harsh industrial environments where WiFi may be intermittent.

### STA Mode (Normal Operation)

- On disconnect, the device attempts reconnection with exponential backoff: 2s, 4s, 8s, 16s, 32s (cap).
- After 3 failed retries, the device switches to AP mode as a fallback.
- RSSI is updated every 10 seconds via `wifi.rssi` state key.

### AP Mode (Fallback)

When no WiFi credentials are stored or STA connection fails, the device creates an access point:
- SSID: `ModESP-XXYY` (from MAC address), configurable via NVS
- IP: `192.168.4.1`
- Max connections: 2
- Auth: WPA2-PSK if password >= 8 chars, otherwise open

### AP-to-STA Probe

While in AP mode, if STA credentials exist, the device periodically probes for the configured WiFi network:

1. Switch from AP to APSTA mode (AP remains active for local clients).
2. Attempt STA connection with a 15-second timeout.
3. **On success**: switch to pure STA mode, release AP resources.
4. **On failure or timeout**: revert to pure AP mode.
5. Backoff between probes: 30s, 60s, 120s, 240s, 300s (cap at 5 minutes).
6. Probe is skipped if free heap drops below 50 KB (APSTA mode consumes more memory).

### STA Watchdog

In STA mode, the device tracks cumulative disconnect time. If total disconnect exceeds 10 minutes, the device performs a full restart (`esp_restart()`). The disconnect counter resets after 1 hour of stable connection.

## 10. MQTT Reconnect

The MQTT client uses its own reconnection logic with exponential backoff, independent of WiFi:

- Initial delay: 5 seconds
- Maximum delay: 5 minutes (300 seconds)
- On successful connect, backoff resets to 5 seconds.
- Auto-reconnect is disabled in ESP-MQTT (`disable_auto_reconnect = true`); reconnection is managed in `on_update()`.

Reconnects triggered by configuration changes (HTTP API, `_set_tenant`) are deferred to the main loop via `reconnect_requested_` flag to avoid race conditions with the httpd task.

## 11. Home Assistant Auto-Discovery

On every MQTT connect, the device publishes Home Assistant MQTT discovery messages for 22 entities:

- 5 temperature sensors (`air_temp`, `evap_temp`, `cond_temp`, `setpoint`, `effective_setpoint`)
- 4 binary sensors for relays (`compressor`, `defrost_relay`, `evap_fan`, `cond_fan`)
- 6 alarm binary sensors (`alarm_active`, `high_alarm`, `low_alarm`, `rate_alarm`, `short_cycle_alarm`, `rapid_cycle_alarm`)
- 2 text sensors (`alarm_code`, `defrost.state`)
- 1 defrost active binary sensor
- 4 diagnostic sensors (`motor_hours`, `duty_cycle`, `uptime`, `free_heap`)

Discovery topics follow the HA convention: `homeassistant/{type}/modesp_{device_id}/{object_id}/config` with QoS 1 and retain.

Each entity includes availability tracking via the `{prefix}/status` topic (`online`/`offline`).

## 12. HTTP Configuration API

The MQTT service registers local HTTP endpoints for configuration:

### `GET /api/mqtt`

Returns current MQTT configuration and status:

```json
{
  "enabled": true,
  "connected": true,
  "broker": "mqtts://mqtt.modesp.cloud",
  "port": 8883,
  "user": "device_a4cf12",
  "prefix": "modesp/v1/acme-foods/A4CF12",
  "tenant": "acme-foods",
  "device_id": "A4CF12",
  "status": "connected"
}
```

Status values: `disabled`, `no_broker`, `waiting_wifi`, `connecting`, `connected`, `disconnected`, `error`.

### `POST /api/mqtt`

Update MQTT configuration (requires authentication). Accepts JSON with any combination of: `broker`, `port`, `user`, `password`, `prefix`, `tenant`, `enabled`. Keys can optionally be prefixed with `mqtt.` (e.g., `mqtt.broker`).

After saving, the client automatically reconnects with the new configuration. If tenant is changed without an explicit prefix, the prefix is cleared and auto-rebuilt from the new tenant.
