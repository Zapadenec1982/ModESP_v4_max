# ModESP v4 — Message Bus: коли використовувати, коли ні

**Версія:** 1.0
**Створено:** 2026-04-18
**Статус:** Активна архітектурна політика

---

## TL;DR

ModESP має **дві** канали комунікації між модулями:

1. **SharedState** — єдине джерело правди про стани. Persistent, polling, deltas.
2. **Message Bus** (`etl::message_bus`) — broadcast транзитних подій. Fan-out, fire-and-forget.

**За замовчуванням використовуй SharedState.** Message bus — для вузького переліку випадків (див. нижче). Більшість того, що *виглядає* як event, насправді є зміна стану і має жити у SharedState.

Поточні легітимні події на шині (станом на 2026-04-18):

| Event | Publisher | Subscribers | Чому event, а не стан |
|-------|-----------|-------------|------------------------|
| `SYSTEM_SAFE_MODE` | ErrorService | Equipment, Thermostat | Broadcast fan-out: ≥2 модулі мають синхронно скинути виходи |
| `MODULE_TIMEOUT` | WatchdogService | ErrorService | Transient signal: «модуль X не відповів», не зберігається |

Решта (sensor readings, setpoint, FSM state, requests) — у SharedState.

---

## Decision Flow

Перед тим як додати новий `etl::message<>` тип — пройдись по дереву:

```
┌─────────────────────────────────────────────────────────────┐
│ Питання 1: Чи це persistent state (значення, яке хтось     │
│            захоче прочитати ПІСЛЯ події)?                   │
└─────────────────────────────────────────────────────────────┘
       │
       ├─ ТАК ──► SharedState.set(key, value)
       │         (приклади: setpoint, FSM state, alarm active,
       │          relay actual state, sensor reading)
       │
       └─ НІ ──► Питання 2

┌─────────────────────────────────────────────────────────────┐
│ Питання 2: Скільки модулів має зреагувати?                  │
└─────────────────────────────────────────────────────────────┘
       │
       ├─ 1 ──► Прямий method call або InputBinding
       │       (приклади: Equipment → Driver, EEV → Refrigerant lib)
       │
       └─ ≥2 ──► Питання 3

┌─────────────────────────────────────────────────────────────┐
│ Питання 3: Частота події?                                   │
└─────────────────────────────────────────────────────────────┘
       │
       ├─ >1Hz ──► SharedState (delta tracking, WS broadcast)
       │          (приклади: temperature, pressure, RPM)
       │
       └─ <1Hz ──► Message Bus (publish/subscribe)
                  (приклади: SAFE_MODE, defrost lifecycle, OTA progress)
```

Якщо вийшов на "Message Bus" — переходь до розділу **Правила публікації**.

---

## Коли використовувати Message Bus

### ✅ Випадок 1: Transient signal без state representation

Подія, яка «сталася і пройшла», її немає сенсу зберігати у SharedState.

**Приклади:**
- `WATCHDOG_TIMEOUT` — модуль X не оновився за N мс. Після обробки сигнал «згорає».
- `OTA_CHUNK_RECEIVED` — отримано блок прошивки (опційно для UI прогресу).
- `BUTTON_LONG_PRESSED` — кнопку утримано >3с (UI reaction, не зберігається).

**Чому не SharedState:** немає логічного «значення» цього стану через 1 секунду. SharedState засмітиться flag-ами які треба вручну скидати.

### ✅ Випадок 2: Broadcast fan-out, ≥2 синхронних реакцій

Одна подія, кілька модулів мають **синхронно** зреагувати в межах одного циклу.

**Приклад:** `SYSTEM_SAFE_MODE`
- Equipment: вимкнути всі реле, ігнорувати requests
- Thermostat: скинути requests у false, перейти у IDLE
- Defrost: перервати поточну фазу
- Logger: залогувати причину

Якби це було через SharedState, кожен модуль poll-ив би `system.safe_mode` у власному on_update(). Потенційний lag до 100мс між модулями. Для аварійної зупинки актуаторів — неприпустимо.

**Чому не прямий call:** ErrorService не має знати про Thermostat/Defrost/Equipment. Bus дає decoupling.

### ✅ Випадок 3: Discrete lifecycle event частотою <1Hz

Подія-маркер, на яку треба реагувати один раз, а не polling-ом.

**Приклади (потенційні, ще не реалізовані):**
- `DEFROST_STARTED` / `DEFROST_ENDED` — для DataLogger щоб додати маркер у графік
- `ALARM_CLEARED` — для аудит-логу хто/коли скинув alarm

Альтернатива через SharedState: тримати `defrost.cycle_id` (int32) і polling-ом дивитись чи змінився. Працює, але семантично подія discrete — bus природніший.

---

## Коли НЕ використовувати Message Bus

### ❌ Антипатерн 1: State updates через event

```cpp
// ❌ ПОГАНО — setpoint це state, не event
struct MsgSetpointChanged : etl::message<160> {
    float new_setpoint;
};

// HTTP handler:
publish(MsgSetpointChanged{5.0f});

// Thermostat:
void on_message(...) {
    if (msg_id == 160) setpoint_ = ((MsgSetpointChanged&)msg).new_setpoint;
}
```

**Проблеми:**
- Якщо повідомлення загубилось (queue overflow) — Thermostat не дізнається про новий setpoint назавжди.
- При рестарті модуль не знає поточного setpoint — треба окремий «дай мені поточний» запит.
- WebSocket broadcast не побачить зміни (bus не інтегрований з WS deltas).
- Дублює логіку: setpoint вже у SharedState (persist=true).

```cpp
// ✅ ДОБРЕ — setpoint у SharedState
// HTTP handler:
state_->set("thermo_z1.setpoint", 5.0f);

// Thermostat (sync_settings polling кожен on_update):
void sync_settings() {
    setpoint_ = read_float(ns_key("setpoint"), setpoint_);
}
```

**Правило:** якщо значення має зберігатись після рестарту або відображатись у UI — це state, не event.

### ❌ Антипатерн 2: High-frequency data stream

```cpp
// ❌ ПОГАНО — sensor reading 10Hz через bus
struct MsgTemperatureUpdate : etl::message<111> {
    float temp;
};
publish(MsgTemperatureUpdate{air_temp_});  // 10 разів/сек × N сенсорів
```

**Проблеми:**
- Queue переповниться (capacity 8 routers × N messages).
- WS broadcast буде дублюватись (state вже синхронізується через delta tracking).
- Subscribers роблять polling однаково — тільки гірше через зайвий шар.

```cpp
// ✅ ДОБРЕ — sensor → SharedState, споживачі polling-ують
state_->set("equipment.air_temp", temp);
// Будь-який модуль: read_input_float("air_temp") у on_update()
```

### ❌ Антипатерн 3: Request-response 1→1

```cpp
// ❌ ПОГАНО — Equipment запитує дані у драйвера через bus
publish(MsgRequestSensorRead{"temp1"});
// чекати відповідь у on_message... race condition
```

**Правильно:** прямий method call (`driver->read(value)`) або InputBinding (`read_input_float("air_temp")`). Bus це **broadcast**, не RPC.

---

## Правила публікації

Якщо ти пройшов decision flow і дійсно потрібен event:

### 1. Message ID реєструй у `types.h::msg_id`

Дотримуйся діапазонів:
- `0–49` — системні (SYSTEM_*, STATE_CHANGED)
- `50–99` — сервіси (MODULE_TIMEOUT, CONFIG_*, MQTT_*)
- `100–109` — HAL (GPIO_CHANGED)
- `110–149` — драйвери (SENSOR_*, ACTUATOR_*)
- `150–249` — модулі (ALARM_*, DEFROST_*)

### 2. Структура повідомлення — POD, фіксований розмір

```cpp
// driver_messages.h або <module>_messages.h
struct MsgAlarmTriggered : etl::message<msg_id::ALARM_TRIGGERED> {
    etl::string<16> source;    // "protection.HighTemp"
    int32_t code;
    uint32_t timestamp_ms;
};
```

Без `std::string`, без покажчиків на heap. ETL only.

### 3. Subscriber реєструє інтерес у `on_init()`

```cpp
bool MyModule::on_init() {
    // Підписатись на SAFE_MODE
    subscribe(msg_id::SYSTEM_SAFE_MODE);
    return true;
}

void MyModule::on_message(const etl::imessage& msg) {
    switch (msg.get_message_id()) {
        case msg_id::SYSTEM_SAFE_MODE:
            handle_safe_mode();
            break;
    }
}
```

### 4. Коментар над `publish()` — чому це event, а не state

Кожна публікація має inline-коментар з посиланням на правило з цього документа:

```cpp
// SAFE_MODE — broadcast fan-out (правило 2 у docs/13_message_bus.md):
// Equipment + Thermostat + Defrost мають синхронно зреагувати <100ms.
publish(MsgSafeMode{reason});
```

Якщо не можеш написати такий коментар — швидше за все, це не event.

---

## Capacity та продуктивність

`MODESP_MAX_BUS_ROUTERS = 8` (у `module_manager.h`).

Розрахунок:
- Реальне використання станом на 2026-04-18: **3 routers** (Equipment, Thermostat, ErrorService).
- Запас 8 = ~2.5× для майбутніх event-handlers (alarm aggregation, OTA progress, audit log).
- Кожен router у ETL bus коштує ~16 байт на pointer + table → 128 байт RAM.

Якщо потрібно >8 — спочатку перевір, чи новий handler справді event-driven, чи можна зробити SharedState polling. Збільшення capacity — останній варіант.

---

## Архітектурне обґрунтування

### Чому не "все через bus" як ESPHome / Home Assistant?

ESPHome використовує event bus агресивно: будь-яка зміна стану — event. Це працює тому що:
- ESPHome генерує **runtime** event handlers (Python YAML → код).
- Користувач описує реакції декларативно: «коли temp > 25, увімкни fan».
- Bus це абстракція над прямими method calls для «безкодового» досвіду.

ModESP — інший контекст:
- **Compile-time** C++ архітектура. InputBinding вирішує decoupling без runtime overhead.
- Модулі пишуться розробником (не end-user), знають про SharedState.
- Polling SharedState у `on_update(100Hz)` — детерміністичний lag ≤10мс, прийнятно для всього крім аварій.
- WebSocket delta broadcast вже інтегрований з SharedState — bus це додатковий шар без вигоди.

Висновок: agressive event bus у нашому контексті = cargo cult з ESPHome. Тримаємо bus вузьким.

### Чому SharedState виграє у 95% випадків?

| Критерій | SharedState | Message Bus |
|----------|-------------|-------------|
| Persistent (виживає рестарт) | ✅ через PersistService | ❌ ефемерний |
| WebSocket auto-broadcast | ✅ delta tracking | ❌ окремий шар |
| HTTP/MQTT integration | ✅ універсальний read/write | ❌ треба ручний bridge |
| Polling rate | 100Hz (детерміністично) | Подієво (queue depth ризик) |
| Debugging (state inspection) | ✅ snapshot через WS | ❌ треба логувати кожен event |
| Lost message recovery | ✅ останнє значення завжди в state | ❌ якщо не обробив — назавжди |

Bus виграє тільки коли подія справді transient + broadcast. Решта — SharedState.

---

## Migration history

### 2026-04-18 — SETPOINT_CHANGED видалено

**Що:** `MsgSetpointChanged` (msg_id 160), його handler у `ThermostatModule::on_message`.

**Чому:** Setpoint це persistent state. HTTP/MQTT/config записують у SharedState (`thermo_z1.setpoint`), `sync_settings()` polling-ує у кожному `on_update()`. Handler був dead code — нікого не публікував у production.

**Зміни:**
- Видалено `MsgSetpointChanged` з `driver_messages.h`
- Видалено `msg_id::SETPOINT_CHANGED = 160` з `types.h`
- Видалено гілку у `ThermostatModule::on_message`
- `MODESP_MAX_BUS_ROUTERS` зменшено з 24 до 8 (реальне використання — 3)

**Що залишилось на bus:** `SYSTEM_SAFE_MODE`, `MODULE_TIMEOUT` — обидва відповідають правилам цього документа.

---

## Checklist: перед додаванням нового message type

- [ ] Пройшов decision flow — це не state і не RPC?
- [ ] ≥2 subscribers або справді transient signal?
- [ ] Частота <1Hz (або це аварійний broadcast)?
- [ ] Можу написати коментар над `publish()` зі специфічним правилом?
- [ ] msg_id у правильному діапазоні (`types.h::msg_id`)?
- [ ] Структура — POD, ETL containers, без heap?
- [ ] Subscriber реєструє інтерес у `on_init()`, не у `on_update()`?
- [ ] Якщо additional router потрібен — лишилось місце у `MODESP_MAX_BUS_ROUTERS = 8`?

Якщо хоча б одне «ні» — швидше за все, твій кейс це SharedState, не bus.
