# WiFi Direct Hooks — план реализации

ESP32 запускает WiFi + HTTP-сервер напрямую. Claude Code хуки отправляют
запросы на `http://claude-buddy.local:9876/...`. Python-бридж не нужен.

```
Claude Code hooks ──HTTP──► ESP32 (http://claude-buddy.local:9876)
                                ├─ /session-start   → обновить состояние
                                ├─ /session-end      → сбросить состояние
                                ├─ /pre-tool         → running=true
                                ├─ /post-tool        → добавить entry
                                ├─ /permission       → показать промпт, ждать тач
                                ├─ /notification     → waiting=true
                                ├─ /stop             → running=false
                                └─ /health           → статус JSON
```

## Граф зависимостей

```
Задача 1 (platformio.ini)
  │
  └──► Задача 2 (wifi_server.h — WiFi менеджмент)
         │
         ├──► Задача 3 (wifi_server.cpp — HTTP-сервер + эндпоинты)
         │      │
         │      └──► Задача 4 (main.cpp — интеграция в setup/loop)
         │             │
         │             ├──► Задача 5 (settings menu toggle)
         │             ├──► Задача 7 (WiFi info page)
         │             └──► Задача 9 (hooks config)
         │
         ├──► Задача 6 (Serial-команда для WiFi creds)
         ├──► Задача 8 (NTP time sync)
         └──► Задача 10 (Captive portal)
```

Задачи 5–10 независимы друг от друга после задачи 4.

---

## Задача 1: Добавить библиотеки в platformio.ini

**Файл:** `platformio.ini`

**Что сделать:** Добавить в `lib_deps`:
```ini
me-no-dev/ESPAsyncWebServer @ ^1.2.3
me-no-dev/AsyncTCP @ ^1.1.1
```

Добавить build flag:
```ini
build_flags = ... -DCONFIG_ASYNC_TCP_RUNNING_CORE=1
```

AsyncTCP должен работать на core 1 (тот же, что и Arduino loop), BLE работает на core 0.

**Критерий:** `pio run` компилируется без ошибок.

---

## Задача 2: Создать wifi_server.h — управление WiFi-соединением

**Создать файл:** `src/wifi_server.h`

**Интерфейс:**
```cpp
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>

enum WifiState { WIFI_ST_OFF, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED, WIFI_ST_AP };

void wifiLoadCredentials();           // загрузить SSID/pass из NVS (namespace "buddy", ключи "w_ssid", "w_pass")
void wifiSaveCredentials();           // сохранить в NVS
void wifiStart();                     // WiFi.begin() в STA-режиме, или softAP если нет creds
void wifiStop();                      // WiFi.disconnect(true); WiFi.mode(WIFI_OFF)
void wifiPoll();                      // вызывать из loop(); управляет реконнектом
WifiState wifiState();
const char* wifiIpAddr();             // текущий IP строкой, или ""
bool wifiHasCredentials();
void wifiSetCredentials(const char* ssid, const char* pass);
```

**Детали реализации:**
- NVS: тот же namespace `"buddy"` что в `stats.h`, ключи `"w_ssid"` и `"w_pass"`
- `wifiStart()`: если есть credentials → `WiFi.begin(ssid, pass)`, state = `CONNECTING`. Если нет → пока просто state = `OFF` (AP будет в задаче 10)
- `wifiPoll()`: проверять `WiFi.status()`. При `WL_CONNECTED` → state = `CONNECTED`, запустить `MDNS.begin("claude-buddy")`. Если отвалился → reconnect через 15 сек
- Хранить IP в `_wifiIpStr` через `WiFi.localIP().toString()`
- **Важно**: `Preferences` уже открыт в stats.h. Нельзя открывать тот же namespace параллельно. Использовать отдельный экземпляр Preferences и открывать/закрывать его только на время чтения/записи wifi-ключей

**Критерий:** ESP32 подключается к WiFi, `claude-buddy.local` резолвится по mDNS с другой машины в сети.

---

## Задача 3: Создать wifi_server.cpp — HTTP-сервер и эндпоинты

**Создать файл:** `src/wifi_server.cpp`

**Контекст:** Этот файл реализует все HTTP-эндпоинты. Он поддерживает
собственное агрегированное состояние сессии (аналог того, что делал Python-бридж)
и копирует его в TamaState для отображения на экране.

**Структуры данных:**
```cpp
// Агрегированное состояние (заменяет SessionState из Python-бриджа)
struct HttpSessionState {
    char sessionId[32];
    bool running;
    bool waiting;
    uint16_t toolCount;
    char entries[8][92];  // последние 8 записей (как TamaState.lines)
    uint8_t nEntries;
    char msg[24];
    uint32_t lastUpdateMs;
};

// Ожидающее решение по permission
struct PendingPermission {
    volatile bool active;
    char promptId[40];
    char tool[20];
    char hint[44];
    volatile int8_t decision;  // 0=ждём, 1=allow, -1=deny
    AsyncWebServerRequest* request;  // HTTP соединение держится открытым
};
```

**Публичный интерфейс (добавить в wifi_server.h):**
```cpp
void httpServerInit();                  // зарегистрировать роуты, запустить сервер
void httpServerStop();
void httpApplyToTama(TamaState* out);   // скопировать HTTP-состояние в TamaState
void httpPermissionResolve(bool allow); // вызывается из main.cpp при тапе
bool httpPermissionPending();           // true если ждём решение
const char* httpPermissionPromptId();   // текущий promptId
```

**Эндпоинты:**

| Метод | Путь | Логика | Ответ |
|-------|------|--------|-------|
| POST | /session-start | Парсить `session_id`, сбросить `_httpState` | `{}` |
| POST | /session-end | Очистить `_httpState` | `{}` |
| POST | /pre-tool | `tool_name` → `running=true`, `msg="→ Bash"` | `{}` |
| POST | /post-tool | `tool_name`+`tool_input` → entry строка, `running=false` | `{}` |
| POST | /permission | **Синхронный!** Заполнить `_perm`, НЕ отвечать сразу. Ответ отправляется позже из `httpPermissionResolve()` | `{"decision":{"behavior":"allow"}}` или `deny` |
| POST | /notification | `waiting=true`, `msg="ожидает ввода"` | `{}` |
| POST | /stop | `running=false`, `waiting=false`, `msg="готово"` | `{}` |
| GET | /health | Статус JSON | `{"status":"ok","wifi":true,...}` |

**Критический момент — /permission:**

ESPAsyncWebServer позволяет не отвечать на запрос сразу. Паттерн:

```cpp
// body handler — вызывается когда тело запроса получено
void onPermissionBody(AsyncWebServerRequest *request, uint8_t *data,
                      size_t len, size_t index, size_t total) {
    // парсить JSON из data
    // заполнить _perm.tool, _perm.hint, _perm.promptId
    // _perm.active = true;  _perm.decision = 0;
    // _perm.request = request;  // сохранить указатель!
    // НЕ вызывать request->send() — соединение остаётся открытым
}

// Потом из loop() при тапе:
void httpPermissionResolve(bool allow) {
    if (!_perm.active || !_perm.request) return;
    const char* resp = allow
        ? "{\"decision\":{\"behavior\":\"allow\"}}"
        : "{\"decision\":{\"behavior\":\"deny\"}}";
    _perm.request->send(200, "application/json", resp);
    _perm.active = false;
    _perm.request = nullptr;
}
```

**Формирование entry для /post-tool (скопировано из Python-бриджа):**
```cpp
// tool_name="Bash", tool_input={"command":"git status"} → "14:32 git"
// tool_name="Read", tool_input={"file_path":"/foo/bar.cpp"} → "14:32 bar.cpp"
// tool_name="Agent", tool_input={"description":"search"} → "14:32 Agent"
```

**Функция httpApplyToTama():**
Копирует агрегированное HTTP-состояние в TamaState:
```cpp
void httpApplyToTama(TamaState* out) {
    if (!_httpState.lastUpdateMs) return;  // нет данных от HTTP
    out->sessionsTotal   = _httpState.sessionId[0] ? 1 : 0;
    out->sessionsRunning = _httpState.running ? 1 : 0;
    out->sessionsWaiting = _httpState.waiting ? 1 : 0;
    strncpy(out->msg, _httpState.msg, sizeof(out->msg)-1);
    // скопировать entries → lines
    out->nLines = _httpState.nEntries;
    for (int i = 0; i < _httpState.nEntries; i++)
        strncpy(out->lines[i], _httpState.entries[i], 91);
    // скопировать pending permission
    if (_perm.active) {
        strncpy(out->promptId, _perm.promptId, sizeof(out->promptId)-1);
        strncpy(out->promptTool, _perm.tool, sizeof(out->promptTool)-1);
        strncpy(out->promptHint, _perm.hint, sizeof(out->promptHint)-1);
    } else {
        out->promptId[0] = 0;
    }
    out->connected = true;
    out->lastUpdated = millis();
}
```

**Thread safety:** AsyncTCP работает на core 1 (тот же что Arduino loop),
но в контексте коллбэка TCP-стека. Использовать `volatile` для shared флагов.
Держать критические секции короткими.

**Критерий:** Все эндпоинты отвечают на curl. `/permission` держит соединение до вызова `httpPermissionResolve()`.

---

## Задача 4: Интегрировать WiFi в main.cpp setup() и loop()

**Файл:** `src/main.cpp`

**Изменения:**

1. **Include** вверху: `#include "wifi_server.h"`

2. **В `setup()`**, после `settingsLoad()`:
```cpp
wifiLoadCredentials();
if (settings().wifi) {
    wifiStart();
    httpServerInit();
}
```

3. **В `loop()`**, после `dataPoll(&tama)`:
```cpp
if (settings().wifi) {
    wifiPoll();
    httpApplyToTama(&tama);
}
```

4. **В `actionApprove()`** — добавить HTTP-ветку:
```cpp
if (httpPermissionPending()) {
    httpPermissionResolve(true);
} else {
    // существующий BLE/Serial путь
    char cmd[96];
    snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
        tama.promptId);
    sendCmd(cmd);
}
```

5. **В `actionDeny()`** — аналогично:
```cpp
if (httpPermissionPending()) {
    httpPermissionResolve(false);
} else {
    // существующий BLE/Serial путь
    ...
}
```

6. **Не трогать** существующий BLE/Serial путь — он должен работать параллельно.

**Как найти actionApprove/actionDeny:** Искать по `"decision\":\"once\""` и `"decision\":\"deny\""` в main.cpp.

**Критерий:** HTTP хуки обновляют дисплей. Approve/Deny на тачскрине отправляют HTTP-ответ. BLE путь продолжает работать.

---

## Задача 5: WiFi toggle в меню настроек реально включает/выключает WiFi

**Файл:** `src/main.cpp`

**Что изменить:** В функции где обрабатывается `case 3:` для WiFi toggle
(строка ~199, `case 3: s.wifi = !s.wifi; break;`):

```cpp
case 3:
    s.wifi = !s.wifi;
    if (s.wifi) {
        wifiStart();
        httpServerInit();
    } else {
        httpServerStop();
        wifiStop();
    }
    break;
```

Убрать комментарий `// stored only — no WiFi stack linked`.

**Критерий:** Переключение WiFi в меню подключает/отключает WiFi в реальном времени.

---

## Задача 6: Serial-команда для WiFi credentials

**Файл:** `src/xfer.h`

**Что добавить:** В `xferCommand()`, рядом с другими командами:

```cpp
if (strcmp(cmd, "wifi") == 0) {
    const char* ssid = doc["ssid"];
    const char* pass = doc["pass"];
    if (ssid) {
        wifiSetCredentials(ssid, pass ? pass : "");
        if (settings().wifi) { wifiStop(); wifiStart(); }
    }
    _xAck("wifi", ssid != nullptr);
    return true;
}
```

Добавить `#include "wifi_server.h"` в xfer.h если ещё нет.

**Использование:** Отправить по Serial (или BLE):
```json
{"cmd":"wifi","ssid":"MyNetwork","pass":"MyPassword"}
```

**Критерий:** Команда сохраняет credentials в NVS, ESP подключается к сети.

---

## Задача 7: WiFi info page

**Файл:** `src/main.cpp`

**Что сделать:** Добавить новую страницу в info screen (между существующими
страницами и credits). Увеличить `INFO_PAGES`. Страница показывает:

- Когда подключён: IP, mDNS имя, порт, список эндпоинтов
- Когда подключается: SSID и "подключение..."
- Когда нет credentials: инструкцию по настройке через Serial

**Ориентироваться на** существующие info pages (BLUETOOTH, STATS и т.д.)
для стиля отображения.

**Критерий:** Новая страница видна в ротации info screen, показывает актуальную информацию.

---

## Задача 8: NTP time sync

**Файл:** `src/wifi_server.h` (или `.cpp`)

**Что добавить:** В `wifiPoll()`, при переходе в CONNECTED:

```cpp
configTime(0, 0, "pool.ntp.org", "time.nist.gov");
```

После успешного NTP sync (`time(nullptr) > 1700000000`), синхронизировать RTC:
```cpp
time_t now = time(nullptr);
struct tm lt;
localtime_r(&now, &lt);
RTC_TimeTypeDef tm = { (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec };
RTC_DateTypeDef dt = { (uint8_t)lt.tm_wday, (uint8_t)(lt.tm_mon+1),
                       (uint8_t)lt.tm_mday, (uint16_t)(lt.tm_year+1900) };
M5.Rtc.SetTime(&tm);
M5.Rtc.SetDate(&dt);
```

Timezone offset хранить в NVS (ключ `"w_tz"`, int32). Принимать в Serial-команде wifi:
`{"cmd":"wifi","ssid":"...","pass":"...","tz":10800}` (10800 = UTC+3).

**Критерий:** Часы показывают правильное время через ~30 сек после WiFi-подключения.

---

## Задача 9: Обновить hooks config

**Файл:** `tools/claude-code-hooks.json`

**Что сделать:** Обновить все URL с `localhost:9876` на `claude-buddy.local:9876`.

```json
{
  "hooks": {
    "SessionStart": [{"hooks": [{"type": "http", "url": "http://claude-buddy.local:9876/session-start", "async": true}]}],
    "SessionEnd": [{"hooks": [{"type": "http", "url": "http://claude-buddy.local:9876/session-end", "async": true}]}],
    "PreToolUse": [{"hooks": [{"type": "http", "url": "http://claude-buddy.local:9876/pre-tool", "async": true}]}],
    "PostToolUse": [{"hooks": [{"type": "http", "url": "http://claude-buddy.local:9876/post-tool", "async": true}]}],
    "PermissionRequest": [{"hooks": [{"type": "http", "url": "http://claude-buddy.local:9876/permission", "timeout": 120}]}],
    "Notification": [{"matcher": "permission_prompt", "hooks": [{"type": "http", "url": "http://claude-buddy.local:9876/notification", "async": true}]}],
    "Stop": [{"hooks": [{"type": "http", "url": "http://claude-buddy.local:9876/stop", "async": true}]}]
  }
}
```

**Критерий:** Claude Code хуки достигают ESP32 напрямую.

---

## Задача 10: Captive portal для первоначальной настройки WiFi

**Файл:** `src/wifi_server.h` и/или `src/wifi_server.cpp`

**Что сделать:** Когда `wifiStart()` вызван без credentials:

1. `WiFi.softAP("Claude-Buddy-Setup")`
2. Запустить DNS-сервер, перенаправляющий все запросы на `192.168.4.1`
3. Сервировать минимальную HTML-форму на `http://192.168.4.1/`:
   - `WiFi.scanNetworks()` → dropdown
   - Поле пароля
   - Submit → сохранить в NVS, перезапустить WiFi в STA-режиме

HTML хранить как `const char[]` в коде (форма маленькая, LittleFS не нужен).

**Критерий:** При включении WiFi без credentials появляется AP. Подключившись к AP, открывается форма настройки.

---

## Риски

1. **Память:** WiFi + BLE + AsyncWebServer — много RAM. Мониторить `ESP.getFreeHeap()`. Если мало — сделать WiFi и BLE взаимоисключающими.
2. **Permission timeout:** ESPAsyncWebServer может иметь внутренний таймаут соединения < 120 сек. Проверить `ASYNC_TCP_TIMEOUT`. Если нужно — периодически слать пробел для keepalive.
3. **Thread safety:** AsyncTCP callbacks и `loop()` на одном ядре, но в разных контекстах. Использовать `volatile` для shared-флагов.
