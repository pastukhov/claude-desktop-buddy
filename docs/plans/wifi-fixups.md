# WiFi Direct Hooks — доработки после первой реализации

Реализация от qwen-агента компилируется и покрывает общую структуру (WiFi STA/AP,
captive portal, NTP, info page, serial command, hooks config). Но есть критические
расхождения с планом, из-за которых **основной use-case — HTTP хуки от Claude Code —
не будет работать**.

Ниже — задачи, сгруппированные по приоритету.

---

## P0 — без этого ничего не работает

### F1. HTTP body parsing: хуки шлют JSON, а не form params

**Проблема:** Все POST-хендлеры проверяют `req->hasParam("body", true)` — это ищет
form-encoded параметр `body`. Claude Code HTTP hooks отправляют **JSON в теле
запроса** (`Content-Type: application/json`). Текущий код вернёт 400 на каждый
запрос от хуков.

**Файл:** `src/wifi_server.cpp`

**Что сделать:** ESPAsyncWebServer v3 передаёт тело через body handler. Нужно:

1. Зарегистрировать body handler для каждого POST-роута через `.onBody()` или
   использовать `server.onRequestBody()` callback.
2. Парсить JSON из `data` (тело запроса) через ArduinoJson.
3. Убрать все `req->hasParam("body", true)` — они не нужны.

Паттерн для ESPAsyncWebServer v3:
```cpp
server.on("/session-start", HTTP_POST,
    [](AsyncWebServerRequest* req) { /* вызывается после body */ },
    nullptr, // upload handler
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        // data содержит JSON тело
        JsonDocument doc;
        deserializeJson(doc, data, len);
        // обработка...
        req->send(200, "application/json", "{}");
    }
);
```

---

### F2. /permission — должен держать соединение открытым

**Проблема:** Это самая критическая часть. Текущая реализация `/permission`
**сразу отвечает** `{"status":"ok"}`. По плану, хук `PermissionRequest` ожидает
**синхронного ответа** с решением `allow`/`deny`. ESP должен **не отвечать** до
тапа пользователя, держа HTTP-соединение открытым.

**Файл:** `src/wifi_server.cpp`

**Что сделать:**

1. В body handler `/permission`:
   - Распарсить JSON, извлечь `tool_name`, `tool_input`, сформировать hint.
   - Сохранить `AsyncWebServerRequest* request` в `_perm.request`.
   - Заполнить `_perm.active = true`, `_perm.decision = 0`.
   - **НЕ вызывать** `request->send()`.

2. `httpPermissionResolve(bool allow)`:
   - Проверить `_perm.active && _perm.request`.
   - Отправить ответ: `{"decision":{"behavior":"allow"}}` или `deny`.
   - Очистить `_perm`.

3. Структура:
```cpp
struct PendingPermission {
    volatile bool active;
    char tool[20];
    char hint[44];
    volatile int8_t decision;  // 0=ждём, 1=allow, -1=deny
    AsyncWebServerRequest* request;
};
static PendingPermission _perm = {};
```

4. Проверить `ASYNC_TCP_TIMEOUT` — если таймаут < 120 сек, нужен keepalive
   (периодически слать пробел).

**Связано:** F5 (actionApprove/Deny в main.cpp).

---

### F3. httpApplyToTama — должен обновлять TamaState, а не Serial

**Проблема:** Текущий `httpApplyToTama(const char* body)` просто пишет в Serial
`HTTP:<body>`. Он не парсит JSON и не заполняет `TamaState`. Дисплей ничего не
покажет от HTTP-запросов.

**Файл:** `src/wifi_server.cpp`

**Что сделать:** Переписать по плану — функция должна:

1. Поддерживать внутреннее состояние `HttpSessionState _httpState`.
2. Парсить каждый входящий JSON и обновлять `_httpState` в зависимости от
   эндпоинта (session-start → сбросить, pre-tool → running=true, post-tool →
   добавить entry, и т.д.).
3. Иметь отдельную функцию `httpApplyToTama(TamaState* out)` которая копирует
   `_httpState` → `TamaState` (как в плане, задача 3).
4. Вызываться из `loop()` в main.cpp: `httpApplyToTama(&tama)`.

**Entry formatting для /post-tool:**
```
tool_name="Bash", tool_input={"command":"git status"} → "14:32 git"
tool_name="Read", tool_input={"file_path":"/foo/bar.cpp"} → "14:32 bar.cpp"
```

---

### F5. actionApprove/Deny — hardcoded promptId и нет проверки httpPermissionPending

**Проблема:** Обе функции:
- Вызывают `httpPermissionResolve(42, true)` с хардкодом `42` вместо реального promptId.
- Не проверяют `httpPermissionPending()` — всегда идут в BLE path И HTTP path одновременно.
- Сигнатура `httpPermissionResolve(int, bool)` не совпадает с планом `httpPermissionResolve(bool)`.

**Файл:** `src/main.cpp`, строки ~404-432

**Что сделать (по плану, задача 4):**
```cpp
static void actionApprove() {
    if (httpPermissionPending()) {
        httpPermissionResolve(true);   // ответит в HTTP-соединение
    } else {
        // существующий BLE/Serial путь
        char cmd[96];
        snprintf(cmd, sizeof(cmd),
            "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
            tama.promptId);
        sendCmd(cmd);
    }
    responseSent = true;
    // ... stats, beep
}
```

Аналогично для `actionDeny()`.

---

## P1 — работает неправильно

### F4. setup() не проверяет settings().wifi

**Проблема:** В `setup()` WiFi стартует по `wifiHasCredentials()`, игнорируя
`settings().wifi`. Если пользователь выключил WiFi в настройках, после перезагрузки
WiFi всё равно включится.

**Файл:** `src/main.cpp`, строки 1257-1267

**Что сделать (по плану, задача 4):**
```cpp
wifiLoadCredentials();
if (settings().wifi) {
    wifiStart();
    // httpServerInit() вызывается внутри wifiStart() — не нужно дублировать
}
```

Также: `wifiStart()` уже вызывает `httpServerInit()` внутри себя. Текущий код
вызывает его дважды (из `wifiStart()` и из `setup()`) — убрать дубль.

---

### F6. NTP не синхронизирует RTC

**Проблема:** `ntpSyncTime()` получает время из NTP, но не записывает в RTC
(`M5.Rtc.SetTime`/`SetDate`). Часы на дисплее не обновятся.

**Файл:** `src/wifi_server.cpp`, строки 639-666

**Что сделать (по плану, задача 8):**
```cpp
bool ntpSyncTime() {
    if (_ntpSynced || wifiState() != WIFI_ST_CONNECTED) return false;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    // НЕ блокировать loop() — проверять в wifiPoll()
    return false;  // проверим позже
}

// В wifiPoll(), если !_ntpSynced && WIFI_ST_CONNECTED:
time_t now = time(nullptr);
if (now > 1700000000) {
    struct tm lt;
    localtime_r(&now, &lt);
    RTC_TimeTypeDef tm = { (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec };
    RTC_DateTypeDef dt = { ... };
    M5.Rtc.SetTime(&tm);
    M5.Rtc.SetDate(&dt);
    _ntpSynced = true;
}
```

Также: текущий `ntpSyncTime()` блокирует до 5 секунд (`delay(500)` × 10 retries)
в callback из `loop()` — это подвешивает весь UI. Нужно сделать неблокирующим.

---

### F7. NVS namespace расхождение

**Проблема:** План указывал namespace `"buddy"` с ключами `"w_ssid"`, `"w_pass"`.
Реализация использует отдельный namespace `"wificreds"` с ключами `"ssid"`, `"pass"`,
плюс дублирует в `"buddy"` как `"wifi_ssid"`, `"wifi_pass"`.

**Файл:** `src/wifi_server.cpp`, строки 93-138

**Что сделать:** Решить: один namespace или два. Рекомендация — использовать `"buddy"`
с ключами `"w_ssid"`, `"w_pass"` (как в плане). Убрать дублирование в двух
namespace. Важно: `Preferences` в `stats.h` уже использует `"buddy"` — открывать
на время чтения/записи, сразу закрывать.

---

### F8. wifiStart() блокирует на 10 секунд

**Проблема:** `wifiStart()` делает `while (WiFi.status() != WL_CONNECTED) delay(500)` —
до 10 секунд блокировки в `setup()` и при toggle в настройках. Это замораживает
экран.

**Файл:** `src/wifi_server.cpp`, строки 162-188

**Что сделать:** `wifiStart()` должен быть неблокирующим:
```cpp
void wifiStart() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    _wifiState = WIFI_ST_CONNECTING;
    // НЕ ждать — wifiPoll() обработает переход в CONNECTED
}
```

---

### F9. wifiPoll() — нет reconnect таймера

**Проблема:** По плану, при потере соединения нужен reconnect через 15 сек.
Текущий код сразу переключается в AP mode при `WL_CONNECT_FAILED`. Нормальный
кратковременный разрыв WiFi убьёт STA-режим навсегда.

**Файл:** `src/wifi_server.cpp`, строки 208-241

**Что сделать:** Добавить reconnect логику:
```cpp
static uint32_t _reconnectAt = 0;
void wifiPoll() {
    if (_wifiState == WIFI_ST_CONNECTED && WiFi.status() != WL_CONNECTED) {
        _wifiState = WIFI_ST_CONNECTING;
        _reconnectAt = millis() + 15000;
    }
    if (_wifiState == WIFI_ST_CONNECTING && millis() > _reconnectAt) {
        WiFi.begin(ssid, pass);
        _reconnectAt = millis() + 15000;
    }
}
```

---

## P2 — мелочи и polish

### F10. Порт: 80 vs 9876

**Проблема:** Сервер слушает на порту 80. План указывал 9876. Хуки config
соответственно без порта (= 80). Нужно решить: 80 проще (не надо помнить порт),
но 9876 не конфликтует с другими сервисами.

**Решение:** Оставить 80 — это проще для mDNS. Обновить план, а не код.

---

### F11. Captive portal: нет WiFi.scanNetworks()

**Проблема:** План требовал dropdown со списком сетей. Текущая форма имеет
текстовое поле для SSID.

**Файл:** `src/wifi_server.cpp`, captive portal HTML

**Что сделать:** Добавить endpoint `/scan` возвращающий JSON со списком сетей,
и JS в форме который делает fetch и строит dropdown. Низкий приоритет — текстовое
поле работает.

---

### F12. wifiSetCredentials() не реализован

**Проблема:** Функция объявлена в плане но не реализована в .cpp. Используется
`wifiSaveCredentials()` вместо неё. В xfer.h вызов идёт через `extern` на
`wifiSaveCredentials`. Нужно либо добавить `wifiSetCredentials` как обёртку,
либо убрать из плана.

**Решение:** Убрать из .h — `wifiSaveCredentials` достаточно.

---

### F13. extern DNSServer _dnsServer в main.cpp

**Проблема:** В main.cpp (строка 13) `extern DNSServer _dnsServer` — прямой
доступ к internal state wifi_server.cpp. В applySetting case 3 дублируется логика
AP-старта вместо вызова `wifiStart()`.

**Файл:** `src/main.cpp`, строки 10-13, 206-229

**Что сделать:** Убрать extern. В applySetting case 3 просто вызывать
`wifiStart()`/`wifiStop()` — они уже содержат всю логику AP/STA.

---

### F14. Timezone offset (задача 8)

**Проблема:** Не реализовано хранение timezone offset в NVS и приём через
serial-команду wifi (`"tz":10800`).

**Приоритет:** Низкий. Можно добавить позже.

---

## Порядок выполнения

```
F1 (body parsing) ──► F2 (/permission hold) ──► F3 (httpApplyToTama) ──► F5 (actionApprove/Deny)
                                                                              │
F8 (неблокирующий wifiStart) ──► F9 (reconnect)                              │
                                                                              ▼
F4 (settings().wifi) ──► F6 (NTP→RTC) ──► F7 (NVS namespace) ──► F13 (extern cleanup)
```

F1–F5 — критический путь. Без них HTTP хуки от Claude Code не работают.
F8–F9 — второй приоритет, влияют на надёжность.
Остальное — polish.
