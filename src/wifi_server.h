#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// WiFi state machine — STA with NVS credentials, AP fallback (captive portal)
// ---------------------------------------------------------------------------

enum WifiState { WIFI_ST_OFF, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED, WIFI_ST_AP };

// WiFi credentials storage (loaded from NVS)
struct wifi_creds_t {
    String ssid;
    String pass;
};

bool     wifiLoadCredentials();
bool     wifiLoadCredentials(wifi_creds_t* out);  // overload: load into struct
bool     wifiSaveCredentials(const char* ssid, const char* pass);
void     wifiStart();
void     wifiStop();
void     wifiPoll();
WifiState wifiState();
const char* wifiIpAddr();
bool     wifiHasCredentials();

// ---------------------------------------------------------------------------
// HTTP server — Claude Code hooks endpoints
// ---------------------------------------------------------------------------

struct TamaState;  // forward decl from data.h

// Агрегированное состояние (заменяет SessionState из Python-бриджа)
struct HttpSessionState {
    char     sessionId[32];
    bool     running;
    bool     waiting;
    uint16_t toolCount;
    char     entries[8][92];  // последние 8 записей (как TamaState.lines)
    uint8_t  nEntries;
    char     msg[24];
    uint32_t lastUpdateMs;
};

bool     httpServerInit();                  // зарегистрировать роуты, запустить сервер
void     httpServerStop();
void     httpApplyToTama(const char* path, const char* body);  // обновить _httpState по URL-пути
void     httpApplyToTamaBridge(TamaState* out);  // F3: копировать HttpSessionState → TamaState
int      httpPermissionPending();           // 1 если ждём решение, 0 иначе
const char* httpPermissionPromptId();       // текущий promptId
void     httpPermissionResolve(bool approved);  // F2: ответит в HTTP-соединение (без promptId)
void     httpPermissionPrompt(int promptId, const char* tool, const char* hint);
void     httpPermissionKeepalive();             // F2: keepalive для pending permission

// ---------------------------------------------------------------------------
// Captive portal (задача 10) — включается когда нет STA credentials
// ---------------------------------------------------------------------------

void captivePortalInit();
void captivePortalPoll(uint32_t now);

// ---------------------------------------------------------------------------
// F6: NTP time sync (неблокирующий)
// ---------------------------------------------------------------------------

bool ntpSyncTime();
bool ntpWasSynced();
void ntpPollRtcSync();  // вызывать из loop() для синхронизации RTC

// F14: timezone offset — чтение/запись в NVS
int32_t ntpGetTimezone(void);
void    ntpSetTimezone(int32_t offsetSeconds);
