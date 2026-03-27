# AWS IoT Core — Інтеграція ModESP v4

## Огляд

ModESP v4 підтримує AWS IoT Core як **compile-time альтернативу** Mosquitto MQTT broker.
Вибір cloud backend через Kconfig menuconfig — один firmware binary = один backend.

**Компонент:** `components/modesp_aws/` — AwsIotService (BaseModule)
**Гілка:** `feature/aws-iot`
**SDK:** ESP-IDF `mqtt` component з mTLS (не esp-aws-iot)

## Архітектура

```
Kconfig: MODESP_CLOUD_BACKEND
  ├─ MQTT (default) → MqttService (modesp_mqtt) — Mosquitto broker
  └─ AWS            → AwsIotService (modesp_aws) — AWS IoT Core mTLS
                        │
                        ├─ Telemetry: modesp/{device_id}/state/{key}
                        ├─ Commands:  modesp/{device_id}/cmd/{key}
                        ├─ Shadow:    $aws/things/{thing}/shadow/...
                        ├─ Jobs:      $aws/things/{thing}/jobs/...
                        ├─ LWT:       modesp/{device_id}/status → "offline"
                        └─ Heartbeat: modesp/{device_id}/heartbeat → JSON
```

Business modules (thermostat, defrost, protection) працюють через SharedState — не знають про cloud backend.

## Compile-Time Switch

### Kconfig (`main/Kconfig.projbuild`)

```kconfig
choice MODESP_CLOUD_BACKEND
    prompt "Cloud backend"
    default MODESP_CLOUD_MQTT
    config MODESP_CLOUD_MQTT
        bool "Mosquitto (self-hosted MQTT)"
    config MODESP_CLOUD_AWS
        bool "AWS IoT Core"
endchoice
```

### Як переключити

**Спосіб 1: menuconfig (інтерактивний)**
```bash
idf.py menuconfig
# → ModESP Configuration → Cloud backend → AWS IoT Core (або Mosquitto)
idf.py build
```

**Спосіб 2: sdkconfig (ручний)**
```bash
# Переключити на AWS:
sed -i 's/CONFIG_MODESP_CLOUD_MQTT=y/# CONFIG_MODESP_CLOUD_MQTT is not set/' sdkconfig
sed -i 's/# CONFIG_MODESP_CLOUD_AWS is not set/CONFIG_MODESP_CLOUD_AWS=y/' sdkconfig
idf.py build

# Переключити назад на Mosquitto:
sed -i 's/CONFIG_MODESP_CLOUD_AWS=y/# CONFIG_MODESP_CLOUD_AWS is not set/' sdkconfig
sed -i 's/# CONFIG_MODESP_CLOUD_MQTT is not set/CONFIG_MODESP_CLOUD_MQTT=y/' sdkconfig
idf.py build
```

**PowerShell (Windows):**
```powershell
# На AWS:
(Get-Content sdkconfig) -replace 'CONFIG_MODESP_CLOUD_MQTT=y','# CONFIG_MODESP_CLOUD_MQTT is not set' `
                         -replace '# CONFIG_MODESP_CLOUD_AWS is not set','CONFIG_MODESP_CLOUD_AWS=y' |
Set-Content sdkconfig

# На Mosquitto:
(Get-Content sdkconfig) -replace 'CONFIG_MODESP_CLOUD_AWS=y','# CONFIG_MODESP_CLOUD_AWS is not set' `
                         -replace '# CONFIG_MODESP_CLOUD_MQTT is not set','CONFIG_MODESP_CLOUD_MQTT=y' |
Set-Content sdkconfig
```

### Перевірка поточного backend

```bash
grep CONFIG_MODESP_CLOUD sdkconfig
# CONFIG_MODESP_CLOUD_MQTT=y  → Mosquitto
# CONFIG_MODESP_CLOUD_AWS=y   → AWS IoT Core
```

### Що змінюється при переключенні

| | Mosquitto (default) | AWS IoT Core |
|---|---|---|
| Сервіс | MqttService | AwsIotService |
| Бібліотека | ESP-IDF mqtt | ESP-IDF mqtt + mbedtls mTLS |
| Порт | 1883 / 8883 | 8883 (тільки TLS) |
| Аутентифікація | user/password | X.509 сертифікат |
| Конфігурація | /api/mqtt | /api/cloud |
| Shadow | — | ✅ Device Shadow |
| OTA | /api/ota (HTTP upload) | IoT Jobs |
| Розмір binary | ~1.0 MB | ~1.2 MB (+200KB TLS) |
| RAM при init | ~167 KB free | ~124 KB free (~43KB TLS) |

> **Важливо:** fullclean не потрібен при переключенні. Звичайний `idf.py build` перекомпілює тільки змінені файли. Але якщо build cache зіпсований — `idf.py fullclean && idf.py build`.

Default build (без menuconfig) = **Mosquitto** — binary ідентичний поточному.

### main.cpp

```cpp
#if defined(CONFIG_MODESP_CLOUD_AWS)
  static modesp::AwsIotService cloud_service;
#else
  static modesp::MqttService cloud_service;
#endif
```

## Сертифікати

### Зберігання

NVS namespace `"awscert"`, PEM blobs:

| Key | Тип | Розмір | Опис |
|-----|-----|--------|------|
| `cert` | blob | ~1.2KB | Device certificate (PEM) |
| `key` | blob | ~1.7KB | Private key (PEM) |
| `endpoint` | string | ~64B | AWS IoT endpoint |
| `thing` | string | ~32B | AWS Thing name |
| `enabled` | bool | 1B | Enable/disable |

AWS Root CA (AmazonRootCA1) embedded в firmware (`aws_root_ca.h`), valid until 2038.

### NVS Partition

NVS збільшено до **32KB** (з 16KB) для cert+key storage.

### Завантаження сертифікатів

**HTTP API:**
```bash
# Config
curl -X POST http://<IP>/api/cloud \
  -H "Content-Type: application/json" \
  -d '{"endpoint":"xxx.iot.eu-central-1.amazonaws.com","thing_name":"modesp-test","enabled":true}'

# Certificates (через Python для правильного JSON escaping)
python3 -c "
import json, urllib.request
cert = open('certificate.pem.crt').read()
key = open('private.pem.key').read()
data = json.dumps({'cert': cert, 'key': key}).encode()
req = urllib.request.Request('http://<IP>/api/cloud', data=data,
      headers={'Content-Type': 'application/json'}, method='POST')
print(urllib.request.urlopen(req).read().decode())
"
```

**WebUI:** Network → AWS IoT Core → CertUpload widget (PEM paste).

## Topic Structure

| Topic | Напрямок | QoS | Retain | Опис |
|-------|----------|-----|--------|------|
| `modesp/{device_id}/state/{key}` | ESP→AWS | 0 | Ні | Telemetry (delta-publish) |
| `modesp/{device_id}/cmd/{key}` | AWS→ESP | 1 | Ні | Commands (STATE_META validated) |
| `modesp/{device_id}/status` | ESP→AWS | 1 | Так | LWT: "online"/"offline" |
| `modesp/{device_id}/heartbeat` | ESP→AWS | 0 | Ні | JSON: fw, uptime, heap, rssi |
| `$aws/things/{thing}/shadow/update` | ESP→AWS | 1 | — | Shadow reported state |
| `$aws/things/{thing}/shadow/update/delta` | AWS→ESP | 1 | — | Shadow desired changes |
| `$aws/things/{thing}/jobs/notify-next` | AWS→ESP | 1 | — | IoT Jobs (OTA) |
| `$aws/things/{thing}/jobs/{id}/update` | ESP→AWS | 1 | — | Job status update |

## Device Shadow

### Reported State

62 writable params (з MQTT_SUBSCRIBE) публікуються batch JSON кожні 5 секунд:

```json
{"state":{"reported":{"thermostat.setpoint":-18.0,"defrost.type":0,...}}}
```

Розмір документу: ~1842 bytes.

### Delta (Desired → Apply)

Cloud змінює desired → ESP32 отримує delta → валідує через STATE_META → застосовує до SharedState → NVS persist → publishes updated reported.

```json
// Cloud sends:
{"state":{"desired":{"thermostat.setpoint":-20}}}

// ESP32 receives delta, applies, publishes reported
```

## OTA через IoT Jobs

### Flow

1. Firmware binary на HTTP/S3 сервері
2. AWS Console → IoT Core → Jobs → Create custom job
3. Job document:
```json
{
  "url": "http://server/firmware.bin",
  "version": "1.0.2",
  "checksum": "sha256:..."
}
```
4. ESP32 отримує job → download → board check → flash → reboot
5. OTA validation (60s timeout) → mark valid

### Статус

```
Job received → IN_PROGRESS → download → flash → reboot
                → FAILED (якщо помилка)
```

## HTTP API

| Endpoint | Method | Опис |
|----------|--------|------|
| `/api/cloud` | GET | Config + status JSON |
| `/api/cloud` | POST | Save endpoint, thing_name, cert, key, enabled |

### GET /api/cloud Response

```json
{
  "provider": "aws",
  "endpoint": "xxx.iot.eu-central-1.amazonaws.com",
  "thing_name": "modesp-test",
  "device_id": "C7B0E9",
  "enabled": true,
  "connected": true,
  "cert_loaded": true
}
```

## AWS Console Setup

### 1. IoT Core → Settings → Device data endpoint

Скопіювати endpoint URL.

### 2. Security → Policies → Create policy

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {"Effect": "Allow", "Action": "iot:*", "Resource": "*"}
  ]
}
```

> **TODO:** звузити до конкретних дозволів для production.

### 3. Manage → Things → Create thing

- Thing name: `modesp-{device_id}` (наприклад `modesp-C7B0E9`)
- Auto-generate certificate
- Attach policy
- **Завантажити cert + private key** (одноразово!)

### 4. Device Shadows → Create Shadow

- Unnamed (classic) shadow

## Файлова структура

```
components/modesp_aws/
├── CMakeLists.txt                    # REQUIRES modesp_core modesp_services mqtt esp-tls
├── include/modesp/net/
│   └── aws_iot_service.h             # AwsIotService : BaseModule
└── src/
    ├── aws_iot_service.cpp           # ~550 рядків: mTLS, telemetry, Shadow, Jobs, HTTP
    ├── aws_root_ca.h                 # AmazonRootCA1 PEM (embedded, valid until 2038)
    └── (aws_cert_store.cpp)          # NVS cert read/write (inline в aws_iot_service.cpp)
```

## Залежності

| Компонент | Залежить від modesp_aws? |
|-----------|--------------------------|
| `modesp_mqtt` | **НІ** — не змінюється |
| `modesp_net` | **НІ** |
| `modesp_core` | **НІ** |
| Business modules | **НІ** (SharedState) |
| `main.cpp` | **ТАК** — `#if CONFIG_MODESP_CLOUD_AWS` |

## Вартість AWS (~100 пристроїв)

Delta-publish: тільки змінені ключі (~20-60 msg/хв per device).

| Компонент | ~$/міс |
|-----------|--------|
| IoT Core messages | $3-10 |
| Shadow operations | $1-3 |
| S3 (firmware) | $0.10 |
| CloudWatch | $1-2 |
| **Total** | **$5-15** |

Free tier: 250K messages/month × 12 місяців.

## Відомі обмеження

- **Policy:** зараз `iot:*` — потребує hardening для production
- **Shadow:** публікується кожні 5с навіть без змін — потрібна оптимізація
- **OTA Job status:** SUCCEEDED не відправляється після reboot (потрібен callback)
- **Fleet Provisioning:** не реалізовано (Phase 8)
- **ESP32 обмеження:** тільки один TLS клієнт (MQTT або AWS, не обидва)
- **Fullclean build:** потрібно прибрати esp-clang з PATH для ETL component

## Changelog

- 2026-03-15 — Phase 0-7: повна AWS IoT Core інтеграція. mTLS, telemetry, commands,
  Device Shadow, IoT Jobs OTA. 12 комітів на feature/aws-iot. Verified on real ESP32.
