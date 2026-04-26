# Протокол взаимодействия Hardware Buddy с Claude Desktop — подробное описание

## Обзор

Claude Desktop (macOS/Windows) общается с устройством-компаньоном по **Bluetooth Low Energy (BLE)** через **Nordic UART Service (NUS)**. Протокол представляет собой обмен **однострочными JSON-объектами**, разделёнными символом `\n`. Устройство также может принимать данные по USB Serial — парсинг идентичен.

Протокол двунаправленный:
- **Desktop → Device**: heartbeat-снапшоты состояния сессий, команды (`cmd`), синхронизация времени, передача файлов
- **Device → Desktop**: ack-ответы на команды, решения по permission-запросам

---

## Транспортный уровень: BLE Nordic UART Service

### UUID характеристик

| Характеристика | UUID | Направление |
|---|---|---|
| **Service** | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | — |
| **RX** (запись в устройство) | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | Desktop → Device |
| **TX** (уведомления от устройства) | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | Device → Desktop |

### Advertising

Устройство объявляет имя формата `Claude-XXXX`, где `XXXX` — два последних байта BT MAC-адреса в hex. Это позволяет различать несколько устройств в одном помещении. Имя формируется в `main.cpp`:

```c
snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
```

### Параметры соединения

- **MTU**: устройство запрашивает до 517 байт (`BLEDevice::setMTU(517)`). macOS обычно согласовывает 185.
- **TX chunking**: данные отправляются фрагментами размером `min(mtu - 3, 180)` байт с задержкой `delay(4)` между фрагментами для сброса BLE-стека.
- **RX буфер**: кольцевой буфер на 2048 байт. При переполнении байты отбрасываются.
- **Рекомендуемые интервалы подключения**: `minPreferred=0x06`, `maxPreferred=0x12` (совместимость с iOS/macOS).

### Фрейминг

Все данные на проводе — UTF-8 JSON, один объект на строку, терминатор `\n`. Обе стороны должны накапливать байты до `\n` или `\r`, затем парсить. Строки, не начинающиеся с `{`, игнорируются. Линейный буфер: 1024 байта для каждого канала (USB и BT отдельно).

---

## Безопасность и сопряжение

### LE Secure Connections

Устройство требует **LE Secure Connections с MITM-защитой и bonding**:

```c
BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
sec->setCapability(ESP_IO_CAP_OUT);  // DisplayOnly
sec->setKeySize(16);
```

- Устройство выступает как **DisplayOnly** — показывает 6-значный passkey на экране
- Desktop выступает как **KeyboardOnly** — пользователь вводит passkey
- Все NUS-характеристики и CCCD-дескриптор помечены как `ESP_GATT_PERM_*_ENCRYPTED`
- Первый доступ к GATT триггерит OS-уровневый процесс pairing
- Повторные подключения используют сохранённый LTK без повторного ввода passkey
- Шифрование: AES-CCM

### Поведение при ошибке аутентификации

Если аутентификация не удалась, устройство разрывает соединение:
```c
if (!cmpl.success && server) server->disconnect(server->getConnId());
```

### API безопасности на уровне протокола

- `bleSecure()` — возвращает `true` после успешного bonding текущей сессии
- `blePasskey()` — ненулевое значение, пока passkey должен отображаться на экране
- `bleClearBonds()` — стирает все LTK из NVS (вызывается по команде `unpair` и при factory reset)

---

## Входящие данные: Desktop → Device

### 1. Heartbeat-снапшот (основной поток данных)

Desktop отправляет снапшот при каждом изменении состояния + keepalive каждые ~10 секунд.

```json
{
  "total": 3,
  "running": 1,
  "waiting": 1,
  "msg": "approve: Bash",
  "entries": ["10:42 git push", "10:41 yarn test", "10:39 reading file..."],
  "tokens": 184502,
  "tokens_today": 31200,
  "prompt": {
    "id": "req_abc123",
    "tool": "Bash",
    "hint": "rm -rf /tmp/foo"
  }
}
```

#### Поля

| Поле | Тип | Описание | Лимит буфера на устройстве |
|---|---|---|---|
| `total` | `uint8_t` | Общее количество сессий | — |
| `running` | `uint8_t` | Сессий, активно генерирующих ответ | — |
| `waiting` | `uint8_t` | Сессий, ждущих permission-решения | — |
| `completed` | `bool` | Сессия только что завершилась | — |
| `msg` | `string` | Однострочное сообщение для маленького дисплея | 23 символа (`char[24]`) |
| `entries` | `string[]` | Последние строки транскрипта, новые первыми | до 8 строк × 91 символ |
| `tokens` | `uint32_t` | Кумулятивные output-токены с момента запуска desktop-приложения | — |
| `tokens_today` | `uint32_t` | Output-токены с полуночи (переживает рестарт) | — |
| `prompt` | `object\|null` | Присутствует только когда нужно permission-решение | — |
| `prompt.id` | `string` | Уникальный ID запроса (нужен для ответа) | 39 символов (`char[40]`) |
| `prompt.tool` | `string` | Название инструмента (Bash, Read и т.д.) | 19 символов (`char[20]`) |
| `prompt.hint` | `string` | Подсказка — что именно хочет сделать инструмент | 43 символа (`char[44]`) |

#### Производные сигналы (вычисляются устройством)

| Условие | Состояние |
|---|---|
| `running > 0` | Хотя бы одна сессия активно генерирует |
| `waiting > 0` | Permission-запрос ожидает решения |
| `total == 0` | Нет открытых сессий |
| Нет снапшота >30 сек | Соединение считается мёртвым (`dataConnected()` → false) |
| Нет BT-байтов >15 сек | BT-канал считается неактивным (`dataBtActive()` → false) |

#### Обработка токенов

Поле `tokens` — кумулятивное с запуска desktop. Устройство отслеживает **дельту** между текущим и предыдущим значением и прибавляет к NVS-счётчику. Если значение уменьшилось — desktop перезапустился, устройство ресинхронизируется без потери прогресса. Первый пакет после перезагрузки устройства — только запоминание базы (без добавления).

Каждые 50 000 токенов — level up (триггерит состояние `P_CELEBRATE`).

### 2. Turn events

Одноразовое событие после завершения каждого turn'а. Содержит сырой массив content из SDK (текстовые блоки, tool calls). Событие отбрасывается если > 4 КБ в UTF-8.

```json
{
  "evt": "turn",
  "role": "assistant",
  "content": [{ "type": "text", "text": "..." }]
}
```

**Примечание**: текущая прошивка не обрабатывает turn events — они проходят через `_applyJson`, но поскольку у них нет полей `total`/`running`/`cmd`, они не влияют на состояние.

### 3. Синхронизация времени (one-shot при подключении)

```json
{ "time": [1775731234, -25200] }
```

- `time[0]`: Unix epoch seconds (UTC)
- `time[1]`: смещение часового пояса в секундах (например, -25200 = UTC-7)

Устройство вычисляет локальное время: `epoch + offset`, затем через `gmtime_r` получает компоненты (час, минута, секунда, день недели и т.д.) и записывает во внутренний RTC. Флаг `_rtcValid` устанавливается в `true`.

### 4. Имя владельца (one-shot при подключении)

```json
{ "cmd": "owner", "name": "Felix" }
```

Сохраняется в NVS, отображается на info-экранах.

---

## Исходящие данные: Device → Desktop

### 1. Permission-решения

Когда `prompt` присутствует в heartbeat, пользователь может одобрить или отклонить прямо с устройства:

**Одобрить:**
```json
{"cmd":"permission","id":"req_abc123","decision":"once"}
```

**Отклонить:**
```json
{"cmd":"permission","id":"req_abc123","decision":"deny"}
```

- `id` должен точно совпадать с `prompt.id` из heartbeat
- Решение отправляется и по Serial, и по BLE одновременно (`sendCmd` пишет в оба канала)
- Время реакции отслеживается и сохраняется в velocity ring buffer (последние 8 значений)
- Одобрение менее чем за 5 секунд триггерит состояние `P_HEART`

### 2. Ack-ответы на команды

Любая команда с полем `cmd` ожидает ack-ответ:

```json
{ "ack": "<имя_команды>", "ok": true, "n": 0 }
```

При ошибке:
```json
{ "ack": "<имя_команды>", "ok": false, "error": "описание" }
```

`n` — generic-счётчик (байты при chunk-ack, 0 в остальных случаях). Ack отправляется в оба канала (Serial и BLE).

---

## Команды (Desktop → Device)

### `status` — запрос состояния устройства

Desktop опрашивает каждые несколько секунд для панели stats в окне Hardware Buddy.

**Запрос:**
```json
{"cmd":"status"}
```

**Ответ:**
```json
{
  "ack": "status",
  "ok": true,
  "n": 0,
  "data": {
    "name": "Buddy",
    "owner": "Felix",
    "sec": true,
    "bat": { "pct": 87, "mV": 4012, "mA": -120, "usb": true },
    "sys": { "up": 8412, "heap": 84200, "fsFree": 1234567, "fsTotal": 3145728 },
    "stats": { "appr": 42, "deny": 3, "vel": 8, "nap": 12, "lvl": 5 }
  }
}
```

| Поле | Описание |
|---|---|
| `data.name` | Имя устройства (пользовательское) |
| `data.owner` | Имя владельца |
| `data.sec` | `true` если текущее соединение зашифровано (bonded) |
| `data.bat.pct` | Процент заряда (линейная аппроксимация: `(mV - 3200) / 10`, clamp 0..100) |
| `data.bat.mV` | Напряжение батареи в мВ |
| `data.bat.mA` | Ток батареи в мА (отрицательный = зарядка) |
| `data.bat.usb` | USB подключён (VBUS > 4V) |
| `data.sys.up` | Uptime в секундах |
| `data.sys.heap` | Свободная heap-память в байтах |
| `data.sys.fsFree` | Свободное место в LittleFS (байт) |
| `data.sys.fsTotal` | Общий объём LittleFS (байт) |
| `data.stats.appr` | Общее количество approvals |
| `data.stats.deny` | Общее количество denials |
| `data.stats.vel` | Медиана времени реакции (секунды, последние 8 решений) |
| `data.stats.nap` | Кумулятивные секунды сна (face-down) |
| `data.stats.lvl` | Текущий уровень (= tokens / 50000) |

**Примечание**: ответ формируется через `snprintf`, а не через ArduinoJson — меньше heap-нагрузки, фиксированная форма.

### `name` — задать имя устройства

```json
{"cmd":"name","name":"Clawd"}
```
```json
{"ack":"name","ok":true}
```

Имя сохраняется в NVS. Спецсимволы (`"`, `\`, управляющие символы) фильтруются при записи.

### `owner` — задать имя владельца

```json
{"cmd":"owner","name":"Felix"}
```
```json
{"ack":"owner","ok":true}
```

Сохраняется в NVS, отображается на экране приветствия.

### `species` — сменить ASCII-вид (не в REFERENCE.md)

```json
{"cmd":"species","idx":5}
```
```json
{"ack":"species","ok":true}
```

`idx` = 0..17 для ASCII-видов, `0xFF` для GIF-режима. Сохраняется в NVS.

### `unpair` — стереть BLE-bonds

```json
{"cmd":"unpair"}
```
```json
{"ack":"unpair","ok":true}
```

Стирает все сохранённые LTK из NVS через `esp_ble_remove_bond_device`. Desktop отправляет при нажатии кнопки «Forget» — следующее сопряжение покажет новый passkey.

---

## Протокол передачи файлов (Folder Push)

Позволяет стримить содержимое папки (character pack) через BLE. Транспорт content-agnostic. Ограничение: суммарно < 1.8 МБ. Файлы сохраняются в LittleFS под `/characters/<name>/`.

### Последовательность

```
Desktop:  {"cmd":"char_begin","name":"bufo","total":184320}
Device:   {"ack":"char_begin","ok":true}

Desktop:  {"cmd":"file","path":"manifest.json","size":412}
Device:   {"ack":"file","ok":true}

Desktop:  {"cmd":"chunk","d":"<base64>"}
Device:   {"ack":"chunk","ok":true,"n":200}
Desktop:  {"cmd":"chunk","d":"<base64>"}
Device:   {"ack":"chunk","ok":true,"n":412}
          ...повтор chunk до конца файла...

Desktop:  {"cmd":"file_end"}
Device:   {"ack":"file_end","ok":true,"n":412}

          ...повтор file/chunk/file_end для каждого файла...

Desktop:  {"cmd":"char_end"}
Device:   {"ack":"char_end","ok":true}
```

### Детали каждого шага

#### `char_begin`

| Поле | Описание |
|---|---|
| `name` | Имя character pack (из имени папки или `manifest.json`) |
| `total` | Суммарный размер всех файлов в байтах |

**Проверки устройства:**
1. Вычисляет `available = fsFree + reclaimable` (место текущего пака под `/characters/`)
2. Если `total + 4096 > available` — отвечает `ok:false` с ошибкой `"need XK, have YK"`
3. При успехе: закрывает текущий GIF-рендерер, стирает всё под `/characters/`, создаёт новую папку

**Если устройство не хочет принимать файлы** — просто не отвечает ack. Desktop ждёт несколько секунд и показывает ошибку.

#### `file`

| Поле | Описание |
|---|---|
| `path` | Имя файла (без вложенных путей; валидацию на `..` и абсолютные пути рекомендуется делать) |
| `size` | Ожидаемый размер файла в байтах |

Открывает файл `/characters/<name>/<path>` на запись.

#### `chunk`

| Поле | Описание |
|---|---|
| `d` | Данные файла, закодированные в base64 |

- Декодирование через `mbedtls_base64_decode` в буфер 300 байт
- Запись в LittleFS, инкремент счётчика `_xWritten`
- **Каждый chunk подтверждается ack'ом** — LittleFS-запись может блокироваться на flash erase, а UART RX-буфер всего ~256 байт. Без ack sender переполнит буфер.
- `n` в ack = кумулятивные записанные байты текущего файла

#### `file_end`

Закрывает текущий файл. Проверяет: `_xWritten == _xExpected` (или `_xExpected == 0`). `n` = финальный размер.

#### `char_end`

Завершает трансфер. Вызывает `characterInit(_xCharName)` для загрузки нового пака. При успехе переключает режим с ASCII на GIF и сохраняет `species=0xFF` в NVS.

### Прогресс

`xferActive()` возвращает `true` во время активного трансфера. `xferProgress()` / `xferTotal()` — текущий/ожидаемый объём. Используется для отрисовки progress bar на экране.

---

## Режимы работы парсера данных

Три режима, проверяются по приоритету:

| Режим | Условие | Поведение |
|---|---|---|
| **Demo** | Включён через меню | Циклирует 5 фейковых сценариев каждые 8 секунд, игнорирует live-данные |
| **Live** | JSON получен за последние 30 сек | Нормальная обработка heartbeat'ов |
| **Asleep** | Нет данных | Все счётчики = 0, сообщение «Claude отключён» |

### USB и BLE — два параллельных канала

Данные могут приходить как по Serial (USB), так и по BLE. Для каждого канала — отдельный линейный буфер на 1024 байта (`_usbLine`, `_btLine`). Парсинг идентичен. Ack-ответы всегда уходят в **оба** канала — устройство не отслеживает, по какому каналу пришла команда.

Отличие: для BLE-канала дополнительно отслеживается `_lastBtByteMs` — время последнего полученного байта. Если > 15 сек нет трафика, канал считается неактивным (keepalive desktop'а ~10 сек, headroom ×1.5).

---

## Конфигурация, сохраняемая через протокол

Все значения хранятся в NVS (ESP32 Non-Volatile Storage) в namespace `"buddy"`:

| NVS-ключ | Тип | Источник | Описание |
|---|---|---|---|
| `petname` | String | cmd `name` | Имя устройства |
| `owner` | String | cmd `owner` | Имя владельца |
| `species` | UChar | cmd `species` | Индекс ASCII-вида (0xFF = GIF) |
| `appr` | UShort | permission `once` | Счётчик approvals |
| `deny` | UShort | permission `deny` | Счётчик denials |
| `tok` | UInt | heartbeat `tokens` | Кумулятивные токены |
| `lvl` | UChar | производная от `tok` | Уровень (tok / 50000) |
| `vel` | Bytes[16] | permission timing | Ring buffer: время реакции на approval (8 × uint16) |
| `vidx` | UChar | — | Текущий индекс в velocity ring buffer |
| `vcnt` | UChar | — | Заполненность velocity ring buffer |
| `nap` | UInt | IMU face-down | Кумулятивные секунды сна |

---

## Включение BLE-моста на стороне Desktop

1. **Help → Troubleshooting → Enable Developer Mode** — добавляет меню «Developer»
2. **Developer → Open Hardware Buddy…** — открывает окно сопряжения
3. Нажать **Connect** → выбрать устройство из списка сканирования
4. При первом подключении macOS запросит разрешение на Bluetooth

После сопряжения мост автоматически переподключается в фоне. Окно нужно только для первичного pairing, панели статистики и drop target для character pack'ов.

---

## Диаграмма потока данных

```
┌──────────────────┐         BLE NUS / USB Serial         ┌──────────────────┐
│  Claude Desktop  │ ──────── JSON lines (\n) ──────────► │  ESP32 Device    │
│                  │                                       │                  │
│  Heartbeats      │  {"total":3,"running":1,...}          │  dataPoll()      │
│  (every change   │  {"time":[epoch, tz_off]}             │  _applyJson()    │
│   + 10s keepalive)│  {"cmd":"status"}                    │  xferCommand()   │
│                  │  {"cmd":"owner","name":"..."}         │                  │
│  Folder push     │  {"cmd":"char_begin",...}             │                  │
│                  │  {"cmd":"chunk","d":"base64"}         │                  │
│                  │                                       │                  │
│                  │ ◄─────── JSON lines (\n) ──────────── │                  │
│                  │  {"cmd":"permission","id":"...","decision":"once"}       │
│  Session manager │  {"ack":"status","ok":true,"data":{...}}                │
│                  │  {"ack":"chunk","ok":true,"n":200}    │  sendCmd()       │
│                  │                                       │  _xAck()         │
└──────────────────┘                                       └──────────────────┘
```
