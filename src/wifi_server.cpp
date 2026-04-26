#include "wifi_server.h"
#include "data.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <Update.h>
#include <time.h>
#include <sys/time.h>

// NTP configuration
static const char* NTP_SERVER = "pool.ntp.org";
static bool _ntpSynced = false;

// F14: timezone offset — динамический, хранится в NVS
static int32_t _timezoneOffset = 0;  // по умолчанию UTC (секунды)

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static AsyncWebServer _httpServer(80);
static AsyncEventSource _events("/events");  // SSE for session notifications

static WifiState _wifiState = WIFI_ST_OFF;
static bool _wifiStarted = false;
static bool _credentialsLoaded = false;
static bool _waitingForSta = false;  // true after /save in captive portal, until STA connects or fails
static bool _httpServerRunning = false;  // tracks whether httpServerInit() has been called

// DNS server — non-static so main.cpp can access it via extern
DNSServer _dnsServer;
static IPAddress _apIp(192, 168, 4, 1);
static const char* _apSSID = "claude-buddy";
static constexpr uint32_t AP_CHANNEL = 1;
static constexpr bool AP_PAGENAME = false;

// Captive portal HTML form for initial WiFi config
static const char CAPTIVE_HTML[] =
    "<!DOCTYPE html>"
    "<html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Claude Buddy Setup</title>"
    "<style>body{font-family:sans-serif;text-align:center;padding:40px;}"
    "form{display:inline-block;text-align:left;}"
    "input,select{padding:8px;margin:6px 0;width:220px;font-size:14px;}"
    "button{padding:10px 24px;font-size:15px;cursor:pointer;}"
    "h2{color:#333;}p{color:#666;}</style></head>"
    "<body><h2>Claude Buddy — WiFi Setup</h2>"
    "<p>Connect your device to a WiFi network</p>"
    "<form method=\"get\" action=\"/save\">"
    "<label>Network:<br><select id=\"net\" name=\"ssid\"><option>Loading...</option></select></label><br>"
    "<label>Password:<br><input id=\"pw\" name=\"pass\" type=\"password\" placeholder=\"Password\"></label><br>"
    "<br><button type=\"submit\">Save &amp; Connect</button></form>"
    "<p id=\"status\" style=\"margin-top:20px;\"></p>"
    "<script>if(location.search){document.getElementById('status').textContent='Saving...';setTimeout(()=>location.href='/health',3000);}"
    "fetch('/scan').then(r=>r.json()).then(d=>"
    "{var s=document.getElementById('net');s.innerHTML='';"
    "(d.networks||[]).forEach(function(n){var o=document.createElement('option');"
    "o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';s.appendChild(o);});"
    "if(!s.options.length)s.innerHTML='<option>(none)</option>';}"
    ").catch(()=>{document.getElementById('net').innerHTML='<option>(scan failed)</option>';})</script>"
    "</body></html>";

// WiFi info page (served when connected to STA)
static const char WIFI_INFO_HTML[] =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Claude Buddy</title>"
    "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#f5f5f5;}"
    ".card{background:#fff;border-radius:12px;padding:24px;max-width:360px;margin:auto;box-shadow:0 2px 8px rgba(0,0,0,.1);}"
    "h2{margin-top:0;color:#333;}p{color:#555;line-height:1.6;}</style></head>"
    "<body><div class=\"card\"><h2>Claude Buddy</h2>"
    "<p id=\"info\">Loading...</p>"
    "<a href=\"/config\" style=\"color:#4a90d9;\">Configure WiFi</a>"
    "</div></body></html>";

// ---------------------------------------------------------------------------
// Forward declarations (functions defined later in this file)
// ---------------------------------------------------------------------------

static void _handleSessionStart(AsyncWebServerRequest* req);
static void _handleSessionEnd(AsyncWebServerRequest* req);
static void _handlePreTool(AsyncWebServerRequest* req);
static void _handlePostTool(AsyncWebServerRequest* req);
static void _handlePermissionGet(AsyncWebServerRequest* req);
static void _handlePermissionBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total);
static void _handleNotification(AsyncWebServerRequest* req);
static void _handleStop(AsyncWebServerRequest* req);
static void _handleHealth(AsyncWebServerRequest* req);
static void _handleWifiInfo(AsyncWebServerRequest* req);

// ---------------------------------------------------------------------------
// Saved credentials for captive portal reconnection
// ---------------------------------------------------------------------------

static String _savedSsid;
static String _savedPass;

// ---------------------------------------------------------------------------
// F7: NVS namespace — единый "buddy" с ключами w_ssid / w_pass
// ---------------------------------------------------------------------------

bool wifiHasCredentials(void) {
    if (_credentialsLoaded) return true;
    Preferences prefs;
    bool ok = prefs.begin("buddy", true);  // read-only
    _credentialsLoaded = ok;
    if (!ok) return false;
    bool has = prefs.getString("w_ssid").length() > 0;
    prefs.end();
    return has;
}

bool wifiLoadCredentials(void) {
    wifi_creds_t creds;
    return wifiLoadCredentials(&creds);
}

bool wifiLoadCredentials(wifi_creds_t* out) {
    Preferences prefs;
    bool ok = prefs.begin("buddy", true);  // read-only
    if (!ok) return false;
    out->ssid = prefs.getString("w_ssid", "");
    out->pass = prefs.getString("w_pass", "");
    bool has = out->ssid.length() > 0;
    prefs.end();
    _credentialsLoaded = has;
    return has;
}

bool wifiSaveCredentials(const char* ssid, const char* pass) {
    Preferences prefs;
    bool ok = prefs.begin("buddy", false);  // read-write
    if (!ok) return false;
    prefs.putString("w_ssid", ssid);
    prefs.putString("w_pass", pass);
    prefs.end();
    _credentialsLoaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// F2: Pending permission — держать HTTP-соединение открытым до решения
// ---------------------------------------------------------------------------

struct PendingPermission {
    volatile bool active;
    char tool[32];
    char hint[128];
    volatile int8_t decision;  // 0=ждём, 1=allow, -1=deny
    AsyncWebServerRequest* request;
};
static PendingPermission _perm = {};

// ---------------------------------------------------------------------------
// F3: HTTP session state — агрегация из входящих JSON
// ---------------------------------------------------------------------------

static HttpSessionState _httpState = {};
static char _lastPayload[512];  // last received payload for actionApprove/Deny

static void _parseJsonString(const char* body, const char* key, char* out, size_t outLen) {
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\":\"", key);
    const char* p = strstr(body, searchKey);
    if (!p) return;
    p += strlen(searchKey);
    const char* end = strchr(p, '"');
    if (!end) { size_t n = min((size_t)(strlen(p)), outLen - 1); memcpy(out, p, n); out[n] = '\0'; return; }
    size_t n = (size_t)(end - p);
    if (n >= outLen) n = outLen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static int _parseJsonInt(const char* body, const char* key) {
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\":", key);
    const char* p = strstr(body, searchKey);
    if (!p) return 0;
    p += strlen(searchKey);
    return atoi(p);
}

static void _copyEntry(const char* label) {
    // Append entry to _httpState.entries (ring buffer of 8)
    if (_httpState.nEntries >= 8) {
        // Shift entries down by one
        memmove(_httpState.entries[0], _httpState.entries[1], 91);
        _httpState.nEntries--;
    }
    size_t n = strlen(label);
    if (n > 91) n = 91;
    memcpy(_httpState.entries[_httpState.nEntries], label, n);
    _httpState.entries[_httpState.nEntries][n] = '\0';
    _httpState.nEntries++;
}

static void _formatEntry(char* out, size_t len) {
    // Format current httpState as a display entry: "HH:MM tool_name" or "HH:MM file_path"
    struct tm lt;
    time_t now = time(nullptr);
    if (now <= 0 || localtime_r(&now, &lt) == nullptr) {
        snprintf(out, len, "--:-- ?");
        return;
    }
    snprintf(out, len, "%02u:%02u %s", lt.tm_hour, lt.tm_min,
             _httpState.running && strlen(_httpState.sessionId) > 0 ? "..." : "");
}

// ---------------------------------------------------------------------------
// WiFi management — F8 неблокирующий + F9 reconnect timer
// ---------------------------------------------------------------------------

static uint32_t _reconnectAt = 0;

void wifiStart() {
    if (_wifiStarted) return;

    // Try STA mode first — use saved credentials
    wifi_creds_t creds;
    if (!wifiLoadCredentials(&creds) || creds.ssid.isEmpty()) {
        // No saved credentials, go straight to AP mode
        Serial.println("[wifi] No saved creds, starting AP");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(_apSSID, nullptr, AP_CHANNEL, AP_PAGENAME);
        _dnsServer.start(53, "*", _apIp);
        _wifiState = WIFI_ST_AP;
        _wifiStarted = true;
        httpServerInit();  // start captive portal HTTP server
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname("claude-buddy");
    WiFi.begin(creds.ssid.c_str(), creds.pass.c_str());
    _wifiState = WIFI_ST_CONNECTING;
    _reconnectAt = millis() + 15000;
    // НЕ ждать — wifiPoll() обработает переход в CONNECTED (F8)

    _wifiStarted = true;
}

void wifiStop(void) {
    if (!_wifiStarted) return;

    if (_wifiState == WIFI_ST_CONNECTED || _wifiState == WIFI_ST_CONNECTING) {
        WiFi.disconnect();
    } else if (_wifiState == WIFI_ST_AP) {
        _dnsServer.stop();
        WiFi.softAPdisconnect(true);
    }
    WiFi.mode(WIFI_OFF);
    _httpServer.end();
    MDNS.end();
    _httpServerRunning = false;
    _wifiStarted = false;
    _wifiState = WIFI_ST_OFF;
}

void wifiPoll() {
    // F9: reconnect logic — не переключать в AP сразу при потере соединения
    if (_wifiState == WIFI_ST_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            _wifiState = WIFI_ST_CONNECTED;
            Serial.printf("[wifi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

            // Start HTTP server on first STA connection (normal boot or captive portal flow)
            if (_waitingForSta) _httpServer.end();
            httpServerInit();
            _waitingForSta = false;
        } else if (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_CONNECTION_LOST) {
            // F9: ждём 15 сек перед fallback в AP
            if (millis() > _reconnectAt) {
                Serial.println("[wifi] Reconnect timeout, switching to AP");
                WiFi.disconnect();
                WiFi.mode(WIFI_AP);
                WiFi.softAP(_apSSID, nullptr, AP_CHANNEL, AP_PAGENAME);
                _dnsServer.start(53, "*", _apIp);

                // Stop current HTTP server (has stale STA routes) and reinit for captive portal
                _httpServer.end();
                httpServerInit();

                _wifiState = WIFI_ST_AP;
                _waitingForSta = false;
            } else {
                // Still retrying — do nothing, wifiPoll will be called again
            }
        }
    }

    // F9: reconnect attempt if we lost connection while in CONNECTING state
    if (_wifiState == WIFI_ST_CONNECTING && WiFi.status() != WL_CONNECTED) {
        if (millis() > _reconnectAt) {
            Serial.println("[wifi] Reconnecting...");
            WiFi.begin(_savedSsid.length() ? _savedSsid.c_str() : "",
                       _savedPass.length() ? _savedPass.c_str() : "");
            _reconnectAt = millis() + 15000;
        }
    }

    // DNS server needs to be polled in captive portal mode
    if (_wifiState == WIFI_ST_AP) {
        _dnsServer.processNextRequest();
    }

}

WifiState wifiState() { return _wifiState; }
const char* wifiIpAddr() {
    static char buf[16];
    if (_wifiState == WIFI_ST_CONNECTED || _wifiState == WIFI_ST_CONNECTING) {
        snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
    } else if (_wifiState == WIFI_ST_AP) {
        strcpy(buf, "192.168.4.1");
    } else {
        strcpy(buf, "---");
    }
    return buf;
}

// ---------------------------------------------------------------------------
// HTTP server — endpoint handlers
// ---------------------------------------------------------------------------

static void _handleSessionStart(AsyncWebServerRequest* req) {
    // POST /session-start → {"promptId":"...","tool":"..."}
    Serial.println("[http] session-start (body via callback)");
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

static void _handleSessionEnd(AsyncWebServerRequest* req) {
    Serial.println("[http] session-end");
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

static void _handlePreTool(AsyncWebServerRequest* req) {
    Serial.println("[http] pre-tool (body via callback)");
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

static void _handlePostTool(AsyncWebServerRequest* req) {
    Serial.println("[http] post-tool (body via callback)");
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

static void _handlePermissionGet(AsyncWebServerRequest* req) {
    // Return pending permission requests
    if (_perm.active && _perm.decision == 0) {
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"pending\":1,\"tool\":\"%s\",\"hint\":\"%s\"}",
                 _perm.tool, _perm.hint);
        req->send(200, "application/json", resp);
    } else {
        req->send(204);  // no pending
    }
}

// F1 + F2: body handler для /permission — парсит JSON и держит соединение открытым
static void _handlePermissionBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
    if (index == 0) {
        // Reset pending permission state on new request
        memset(&_perm, 0, sizeof(_perm));
    }

    // Accumulate body in _lastPayload for later processing
    if (index + len <= sizeof(_lastPayload)) {
        memcpy(_lastPayload + index, data, len);
        _lastPayload[index + len] = '\0';
    }

    if (index + len == total) {
        // Body complete — parse JSON and hold connection
        char tool[32] = "";
        char hint[128] = "";
        _parseJsonString((const char*)_lastPayload, "tool_name", tool, sizeof(tool));

        // Extract hint from tool_input if present
        const char* inputStart = strstr((const char*)_lastPayload, "\"tool_input\":");
        if (inputStart) {
            inputStart += strlen("\"tool_input\":");
            // Skip whitespace and opening brace
            while (*inputStart == ' ' || *inputStart == '{') inputStart++;
            const char* hintKey = strstr(inputStart, "\"hint\":\"");
            if (hintKey) {
                hintKey += 8;
                const char* hintEnd = strchr(hintKey, '"');
                if (hintEnd && (hintEnd - hintKey) < (int)sizeof(hint) - 1) {
                    size_t n = (size_t)(hintEnd - hintKey);
                    memcpy(hint, hintKey, n);
                    hint[n] = '\0';
                }
            }
        }

        // Store pending permission — НЕ отвечаем пока пользователь не решит (F2)
        _perm.active = true;
        _perm.decision = 0;
        strncpy(_perm.tool, tool, sizeof(_perm.tool) - 1);
        _perm.tool[sizeof(_perm.tool) - 1] = '\0';
        strncpy(_perm.hint, hint, sizeof(_perm.hint) - 1);
        _perm.hint[sizeof(_perm.hint) - 1] = '\0';

        // Сохраняем указатель на request для httpPermissionResolve()
        _perm.request = req;

        Serial.printf("[http] permission pending: tool=%s hint=%s\n", _perm.tool, _perm.hint);
        // httpApplyToTamaBridge() в loop() заберёт _perm и отобразит промпт
    }
}

static void _handleNotification(AsyncWebServerRequest* req) {
    Serial.println("[http] notification (body via callback)");
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

static void _handleStop(AsyncWebServerRequest* req) {
    Serial.println("[http] stop requested");
    req->send(200, "application/json", "{\"status\":\"stopping\"}");
}

static void _handleHealth(AsyncWebServerRequest* req) {
    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"wifi\":\"%s\",\"ip\":\"%s\"}",
        (_wifiState == WIFI_ST_CONNECTED) ? "sta" :
        (_wifiState == WIFI_ST_AP) ? "ap" : "off",
        wifiIpAddr());
    req->send(200, "application/json", resp);
}

static void _handleWifiInfo(AsyncWebServerRequest* req) {
    char resp[384];
    const char* modeStr = nullptr;
    switch (_wifiState) {
        case WIFI_ST_OFF:     modeStr = "off"; break;
        case WIFI_ST_CONNECTING: modeStr = "connecting"; break;
        case WIFI_ST_CONNECTED: modeStr = "sta"; break;
        case WIFI_ST_AP:      modeStr = "ap"; break;
        default:              modeStr = "unknown"; break;
    }

    snprintf(resp, sizeof(resp),
        "{\"mode\":\"%s\","
         "\"ip\":\"%s\","
         "\"ssid\":\"%s\","
         "\"rssi\":%d,"
         "\"mdns\":\"%s\"}",
        modeStr,
        wifiIpAddr(),
        (_wifiState == WIFI_ST_CONNECTED || _wifiState == WIFI_ST_CONNECTING) ? WiFi.SSID().c_str() : "",
        (_wifiState == WIFI_ST_CONNECTED) ? WiFi.RSSI() : -999,
        (_wifiState == WIFI_ST_CONNECTED) ? "ok" : "no");
    req->send(200, "application/json", resp);
}

// ---------------------------------------------------------------------------
// F1: Body handler wrapper — накапливает тело запроса в _lastPayload
// ---------------------------------------------------------------------------

static void _bodyHandler(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
    if (index == 0) {
        memset(_lastPayload, 0, sizeof(_lastPayload));
    }
    if (index + len <= sizeof(_lastPayload)) {
        memcpy(_lastPayload + index, data, len);
        _lastPayload[index + len] = '\0';
    }

    if (index + len == total) {
        // Body complete — call the appropriate handler with parsed JSON
        const char* path = req->url().c_str();

        // F14: обработка timezone offset из любого POST-запроса с полем "tz"
        if (strstr(_lastPayload, "\"tz\"")) {
            int32_t tz = _parseJsonInt(_lastPayload, "tz");
            ntpSetTimezone(tz);
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"tz\":%ld}", (long)tz);
            req->send(200, "application/json", resp);
            return;
        }

        if (strcmp(path, "/session-start") == 0) {
            Serial.printf("[http] session-start: %s\n", _lastPayload);
            httpApplyToTama(path, _lastPayload);
            req->send(200, "application/json", "{\"status\":\"ok\"}");
        } else if (strcmp(path, "/session-end") == 0) {
            Serial.printf("[http] session-end: %s\n", _lastPayload);
            httpApplyToTama(path, _lastPayload);
            req->send(200, "application/json", "{\"status\":\"ok\"}");
        } else if (strcmp(path, "/pre-tool") == 0) {
            Serial.printf("[http] pre-tool: %s\n", _lastPayload);
            httpApplyToTama(path, _lastPayload);
            req->send(200, "application/json", "{\"status\":\"ok\"}");
        } else if (strcmp(path, "/post-tool") == 0) {
            Serial.printf("[http] post-tool: %s\n", _lastPayload);
            httpApplyToTama(path, _lastPayload);
            req->send(200, "application/json", "{\"status\":\"ok\"}");
        } else if (strcmp(path, "/notification") == 0) {
            Serial.printf("[http] notification: %s\n", _lastPayload);
            httpApplyToTama(path, _lastPayload);
            req->send(200, "application/json", "{\"status\":\"ok\"}");
        } else if (strcmp(path, "/stop") == 0) {
            Serial.printf("[http] stop: %s\n", _lastPayload);
            httpApplyToTama(path, _lastPayload);
            req->send(200, "application/json", "{\"status\":\"stopping\"}");
        } else if (strcmp(path, "/timezone") == 0) {
            // F14: установка timezone offset — {"tz": 10800}
            int32_t tz = _parseJsonInt(_lastPayload, "tz");
            ntpSetTimezone(tz);
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"tz\":%ld}", (long)tz);
            req->send(200, "application/json", resp);
        } else {
            // Unknown POST endpoint — just acknowledge
            Serial.printf("[http] unknown POST: %s\n", _lastPayload);
            req->send(404, "application/json", "{\"error\":\"not found\"}");
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP server init / stop
// ---------------------------------------------------------------------------

bool httpServerInit(void) {
    // If server is already running, tear it down before re-registering routes.
    // ESPAsyncWebServer accumulates handlers — without this, captive-portal routes
    // survive the AP→STA transition and shadow STA-mode endpoints (e.g. /health).
    if (_httpServerRunning) {
        _httpServer.end();
        MDNS.end();
    }

    if (_wifiState != WIFI_ST_CONNECTED && _wifiState != WIFI_ST_CONNECTING) {
        // Serve captive portal in AP mode
        _httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(200, "text/html", CAPTIVE_HTML);
        });
        _httpServer.on("/config", HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(200, "text/html", CAPTIVE_HTML);
        });
        _httpServer.on("/save", HTTP_GET, [](AsyncWebServerRequest* req) {
            if (req->hasParam("ssid") && req->hasParam("pass")) {
                auto& ssid = req->getParam("ssid")->value();
                auto& pass = req->getParam("pass")->value();
                wifiSaveCredentials(ssid.c_str(), pass.c_str());

                // Store for reconnect
                _savedSsid = ssid;
                _savedPass = pass;

                req->send(200, "text/html",
                    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                    "<title>Connecting...</title>"
                    "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#f5f5f5;}"
                    ".card{background:#fff;border-radius:12px;padding:32px;max-width:360px;margin:auto;box-shadow:0 2px 8px rgba(0,0,0,.1);}"
                    "h2{margin-top:0;color:#333;}p{color:#555;line-height:1.6;}"
                    ".spinner{width:40px;height:40px;margin:16px auto;border:4px solid #ddd;border-top-color:#4a90d9;"
                    "border-radius:50%;animation:spin 1s linear infinite;}@keyframes spin{to{transform:rotate(360deg);}}"
                    ".done{color:#27ae60;font-weight:bold;}.err{color:#e74c3c;}</style></head>"
                    "<body><div class=\"card\"><h2>Claude Buddy</h2>"
                    "<p id=\"status\">Saving credentials...</p>"
                    "<div class=\"spinner\" id=\"spinner\"></div>"
                    "<script>var t=0,max=30;function poll(){t++;fetch('/wifi').then(r=>r.json()).then(d=>"
                    "{if(d.mode==='sta'){document.getElementById('status').innerHTML='<span class=\"done\">Connected!</span><br>"
                    "IP: '+d.ip+'<br>SSID: '+d.ssid+'<br>RSSI: '+d.rssi+' dBm';"
                    "document.getElementById('spinner').style.display='none';"
                    "setTimeout(()=>location.href='/health',3000);}else if(d.mode==='connecting'){"
                    "document.getElementById('status').textContent='Connecting... ('+t+'/'+max+')';}"
                    "else if(t>=max){"
                    "document.getElementById('status').innerHTML='<span class=\"err\">Connection timed out.<br>"
                    "<a href=\"/config\">Try again</a></span>';document.getElementById('spinner').style.display='none';}"
                    "}).catch(()=>{if(t<max)setTimeout(poll,2000);}"
                    "setTimeout(poll,1500);}</script>"
                    "</div></body></html>");

                // Give the response time to reach the browser before disconnecting AP
                delay(800);

                // Disconnect AP and switch to STA mode
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);

                // Set state so wifiPoll() knows we're waiting for connection
                _wifiState = WIFI_ST_CONNECTING;
                _waitingForSta = true;

                WiFi.setHostname("claude-buddy");
                Serial.printf("[save] connecting to '%s'...\n", ssid.c_str());
                WiFi.begin(ssid.c_str(), pass.c_str());
            } else {
                req->send(400, "application/json", "{\"error\":\"missing ssid/pass\"}");
            }
        });

        _httpServer.on("/wifi", HTTP_GET, _handleWifiInfo);

        // F11: scan nearby WiFi networks for captive portal dropdown
        _httpServer.on("/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
            int n = WiFi.scanNetworks();
            if (n <= 0) {
                req->send(200, "application/json", "{\"networks\":[]}");
                return;
            }
            // Deduplicate by SSID — keep best RSSI (repeaters share the same name)
            // seen[i] = index of kept entry for SSID i, or -1 if duplicate
            int kept[32];
            for (int i = 0; i < n && i < 32; i++) kept[i] = i;
            for (int i = 0; i < n && i < 32; i++) {
                if (kept[i] < 0) continue;
                String si = WiFi.SSID(i);
                for (int j = i + 1; j < n && j < 32; j++) {
                    if (kept[j] < 0) continue;
                    if (si == WiFi.SSID(j)) {
                        // Keep the one with stronger signal
                        if (WiFi.RSSI(i) >= WiFi.RSSI(j)) kept[j] = -1;
                        else { kept[i] = -1; break; }
                    }
                }
            }

            // Build compact JSON: {"networks":[{"ssid":"X","rssi":-50},...]}
            char buf[768];
            int off = 0;
            bool first = true;
            off += snprintf(buf + off, sizeof(buf) - off, "{\"networks\":[");
            for (int i = 0; i < n && i < 32 && off < (int)sizeof(buf) - 32; i++) {
                if (kept[i] < 0) continue;
                if (!first) off += snprintf(buf + off, sizeof(buf) - off, ",");
                first = false;
                // Escape SSID: replace \ and " for safety
                String ssidStr = WiFi.SSID(i);  // store before c_str() — temporary would dangle
                const char* ssid = ssidStr.c_str();
                off += snprintf(buf + off, sizeof(buf) - off, "{\"ssid\":\"");
                for (const char* c = ssid; *c && off < (int)sizeof(buf) - 16; c++) {
                    if (*c == '\\') { buf[off++] = '\\'; buf[off++] = '\\'; }
                    else if (*c == '"') { buf[off++] = '\\'; buf[off++] = '\"'; }
                    else buf[off++] = *c;
                }
                off += snprintf(buf + off, sizeof(buf) - off, "\",\"rssi\":%d}", WiFi.RSSI(i));
            }
            off += snprintf(buf + off, sizeof(buf) - off, "]}");
            req->send(200, "application/json", buf);

            // Free scan resources
            WiFi.scanDelete();
        });

        _httpServer.on("/health", HTTP_GET, [](AsyncWebServerRequest* req) {
            char resp[128];
            snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"mode\":\"captive\"}");
            req->send(200, "application/json", resp);
        });

        _httpServer.begin();
        return true;
    }

    // STA mode — register all hooks endpoints

    // Advertise hostname via mDNS so claude-buddy.local resolves on the LAN
    if (MDNS.begin("claude-buddy")) {
        MDNS.addService("_http", "_tcp", 80);
        Serial.println("[mdns] advertised as claude-buddy.local");
    } else {
        Serial.println("[mdns] failed to advertise");
    }

    // F1: POST endpoints with body handler for JSON parsing
    _httpServer.on("/session-start", HTTP_POST, _handleSessionStart, nullptr, _bodyHandler);
    _httpServer.on("/session-end",   HTTP_POST, _handleSessionEnd,   nullptr, _bodyHandler);
    _httpServer.on("/pre-tool",      HTTP_POST, _handlePreTool,      nullptr, _bodyHandler);
    _httpServer.on("/post-tool",     HTTP_POST, _handlePostTool,     nullptr, _bodyHandler);

    // F2: /permission GET — вернуть pending permission
    _httpServer.on("/permission", HTTP_GET, _handlePermissionGet);
    // F1 + F2: /permission POST — держим соединение открытым до решения пользователя.
    // Request-коллбэк вызывается после body handler — НЕ отвечаем здесь.
    // Ответ отправит httpPermissionResolve() из actionApprove/actionDeny в main.cpp.
    _httpServer.on("/permission", HTTP_POST,
        [](AsyncWebServerRequest* req) { /* no-op — соединение держит _perm.request */ },
        nullptr,   // no upload handler
        _handlePermissionBody);

    _httpServer.on("/notification", HTTP_POST, _handleNotification, nullptr, _bodyHandler);
    _httpServer.on("/stop",         HTTP_POST, _handleStop,        nullptr, _bodyHandler);
    _httpServer.on("/health",       HTTP_GET,  _handleHealth);
    _httpServer.on("/wifi",         HTTP_GET,  _handleWifiInfo);

    // Root page — WiFi info
    _httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        char html[512];
        snprintf(html, sizeof(html), WIFI_INFO_HTML);
        req->send(200, "text/html", html);
    });

    // SSE for real-time updates
    _events.onConnect([](AsyncEventSourceClient* client) {
        client->send("hello!", nullptr, millis(), 1000);
    });
    _httpServer.addHandler(&_events);

    _httpServer.begin();
    Serial.println("[http] server started");
    _httpServerRunning = true;
    return true;
}

void httpServerStop() {
    MDNS.end();
    _httpServer.end();
    _httpServerRunning = false;
    Serial.println("[http] server stopped");
}

// ---------------------------------------------------------------------------
// F3: TAMA state bridge — обновляет _httpState по URL-пути и JSON-телу
// ---------------------------------------------------------------------------

// Извлечь "первое слово" команды из tool_input для отображения в entries.
// Bash {"command":"git status ..."} → "git"
// Read {"file_path":"/foo/bar.cpp"} → "bar.cpp"
// Agent {"description":"..."} → "" (вернём пустую строку — будет использован tool_name)
static void _extractEntryLabel(const char* toolName, const char* body, char* out, size_t outLen) {
    if (strcmp(toolName, "Bash") == 0) {
        char cmd[64] = "";
        _parseJsonString(body, "command", cmd, sizeof(cmd));
        if (!cmd[0]) _parseJsonString(body, "cmd", cmd, sizeof(cmd));
        // Берём первое слово команды
        const char* sp = strchr(cmd, ' ');
        size_t n = sp ? (size_t)(sp - cmd) : strlen(cmd);
        if (n >= outLen) n = outLen - 1;
        memcpy(out, cmd, n); out[n] = '\0';
        return;
    }
    if (strcmp(toolName, "Read") == 0 || strcmp(toolName, "Write") == 0 || strcmp(toolName, "Edit") == 0) {
        char path[64] = "";
        _parseJsonString(body, "file_path", path, sizeof(path));
        if (!path[0]) _parseJsonString(body, "path", path, sizeof(path));
        // Берём basename
        const char* slash = strrchr(path, '/');
        const char* base = slash ? slash + 1 : path;
        strncpy(out, base, outLen - 1); out[outLen - 1] = '\0';
        return;
    }
    // Для остальных инструментов — пустая строка (вызывающий подставит toolName)
    out[0] = '\0';
}

static void _addEntry(const char* toolName, const char* body) {
    char detail[32] = "";
    _extractEntryLabel(toolName, body, detail, sizeof(detail));

    struct tm lt;
    time_t now = time(nullptr);
    char label[92];
    if (now > 1000000000L && localtime_r(&now, &lt)) {
        snprintf(label, sizeof(label), "%02u:%02u %s",
                 lt.tm_hour, lt.tm_min,
                 detail[0] ? detail : toolName);
    } else {
        snprintf(label, sizeof(label), "%s", detail[0] ? detail : toolName);
    }
    _copyEntry(label);
}

void httpApplyToTama(const char* path, const char* body) {
    if (!path || !body) return;

    if (strcmp(path, "/session-start") == 0) {
        memset(&_httpState, 0, sizeof(_httpState));
        _parseJsonString(body, "session_id", _httpState.sessionId, sizeof(_httpState.sessionId));
        _httpState.lastUpdateMs = millis();
        return;
    }

    if (strcmp(path, "/session-end") == 0) {
        memset(&_httpState, 0, sizeof(_httpState));
        _perm.active = false;
        _perm.decision = 0;
        _perm.request = nullptr;
        _httpState.lastUpdateMs = millis();
        return;
    }

    if (strcmp(path, "/pre-tool") == 0) {
        _httpState.running = true;
        _httpState.waiting = false;
        char toolName[32] = "";
        _parseJsonString(body, "tool_name", toolName, sizeof(toolName));
        if (toolName[0]) _addEntry(toolName, body);
        _httpState.lastUpdateMs = millis();
        return;
    }

    if (strcmp(path, "/post-tool") == 0) {
        _httpState.running = false;
        char toolName[32] = "";
        _parseJsonString(body, "tool_name", toolName, sizeof(toolName));
        if (toolName[0]) _addEntry(toolName, body);
        _httpState.lastUpdateMs = millis();
        return;
    }

    if (strcmp(path, "/notification") == 0) {
        _httpState.waiting = true;
        snprintf(_httpState.msg, sizeof(_httpState.msg), "ожидает ввода");
        _httpState.lastUpdateMs = millis();
        return;
    }

    if (strcmp(path, "/stop") == 0) {
        _httpState.running = false;
        _httpState.waiting = false;
        snprintf(_httpState.msg, sizeof(_httpState.msg), "готово");
        _httpState.lastUpdateMs = millis();
        return;
    }
}

// F3: Copy HttpSessionState → TamaState (called from main.cpp loop)
void httpApplyToTamaBridge(TamaState* out) {
    if (!out || !_httpState.lastUpdateMs) return;  // нет данных от HTTP вообще

    out->sessionsRunning = _httpState.running ? 1 : 0;
    out->sessionsWaiting = _httpState.waiting ? 1 : 0;
    out->connected = true;
    out->lastUpdated = millis();

    // Copy entries as lines
    out->nLines = _httpState.nEntries;
    for (int i = 0; i < _httpState.nEntries && i < 8; i++) {
        strncpy(out->lines[i], _httpState.entries[i], sizeof(out->lines[i]) - 1);
        out->lines[i][sizeof(out->lines[i]) - 1] = '\0';
    }

    // Copy pending permission into TamaState prompt fields
    if (_perm.active) {
        strncpy(out->promptId,   "http",      sizeof(out->promptId)   - 1);
        strncpy(out->promptTool, _perm.tool,  sizeof(out->promptTool) - 1);
        strncpy(out->promptHint, _perm.hint,  sizeof(out->promptHint) - 1);
        out->promptId[sizeof(out->promptId) - 1]     = '\0';
        out->promptTool[sizeof(out->promptTool) - 1] = '\0';
        out->promptHint[sizeof(out->promptHint) - 1] = '\0';
    } else {
        out->promptId[0] = '\0';
    }
}

int httpPermissionPending() {
    return _perm.active && _perm.decision == 0 ? 1 : 0;
}

const char* httpPermissionPromptId() {
    static char _buf[16];
    if (_perm.active) {
        snprintf(_buf, sizeof(_buf), "http");
        return _buf;
    }
    return "";
}

// F2: Keepalive для pending permission — request->send() закрывает соединение,
//     поэтому keepalive не реализован. ESPAsyncWebServer держит соединение
//     открытым пока не вызван request->send(). Таймаут задаётся ASYNC_TCP_TIMEOUT.
void httpPermissionKeepalive() {
    // no-op
}

// F2: Resolve pending permission — отправить ответ в HTTP-соединение
void httpPermissionResolve(bool approved) {
    if (!_perm.active || !_perm.request) return;

    char resp[128];
    if (approved) {
        snprintf(resp, sizeof(resp), "{\"decision\":{\"behavior\":\"allow\"}}");
    } else {
        snprintf(resp, sizeof(resp), "{\"decision\":{\"behavior\":\"deny\"}}");
    }
    _perm.request->send(200, "application/json", resp);

    // Clear pending state
    memset(&_perm, 0, sizeof(_perm));

    Serial.printf("[http] permission resolved: %s\n", approved ? "allow" : "deny");
}

void httpPermissionPrompt(int promptId, const char* tool, const char* hint) {
    (void)promptId; (void)tool; (void)hint;
    // Already handled in _handlePermissionBody — no-op here
}

// ---------------------------------------------------------------------------
// Captive portal init / poll (stub)
// ---------------------------------------------------------------------------

void captivePortalInit() {
    // Already handled in wifiStart() when no STA creds exist
}

void captivePortalPoll(uint32_t now) {
    // DNS polling is done inside wifiPoll() already
    (void)now;
}

// ---------------------------------------------------------------------------
// F14: timezone offset — чтение/запись в NVS namespace "buddy"
// ---------------------------------------------------------------------------

int32_t ntpGetTimezone(void) {
    Preferences prefs;
    if (!prefs.begin("buddy", true)) return 0;
    int32_t val = (int32_t)prefs.getInt("tz_offset", 0);
    prefs.end();
    _timezoneOffset = val;
    return val;
}

void ntpSetTimezone(int32_t offsetSeconds) {
    Preferences prefs;
    if (!prefs.begin("buddy", false)) return;
    prefs.putInt("tz_offset", (long)offsetSeconds);
    prefs.end();
    _timezoneOffset = offsetSeconds;
    Serial.printf("[ntp] timezone saved: %ld s (%+.2f h)\n", (long)offsetSeconds, (double)offsetSeconds / 3600.0);
}

// ---------------------------------------------------------------------------
// F6: NTP time sync — неблокирующий, синхронизация RTC
// ---------------------------------------------------------------------------

bool ntpWasSynced() { return _ntpSynced; }

bool ntpSyncTime() {
    if (_ntpSynced || wifiState() != WIFI_ST_CONNECTED) return false;
    // F14: загружаем offset из NVS при каждом вызове
    int32_t tz = ntpGetTimezone();
    configTime(tz, 0, NTP_SERVER);
    return false;
}

// Вызывать из main.cpp loop() для неблокирующей синхронизации RTC
void ntpPollRtcSync() {
    if (_ntpSynced || wifiState() != WIFI_ST_CONNECTED) return;

    time_t now = time(nullptr);
    if (now > 1700000000) {  // valid time received
        struct tm lt;
        localtime_r(&now, &lt);

        RTC_TimeTypeDef tm = {
            (uint8_t)lt.tm_sec,
            (uint8_t)lt.tm_min,
            (uint8_t)lt.tm_hour
        };
        RTC_DateTypeDef dt = {
            (uint8_t)((2000 + lt.tm_year) % 100),
            (uint8_t)(lt.tm_mon + 1),
            (uint8_t)lt.tm_mday,
            (uint8_t)lt.tm_wday
        };

        M5.Rtc.SetTime(&tm);
        M5.Rtc.SetDate(&dt);
        _ntpSynced = true;
        Serial.printf("[ntp] RTC synced: %02u:%02u:%02u %04d-%02u-%02u\n",
                      lt.tm_hour, lt.tm_min, lt.tm_sec,
                      1900 + lt.tm_year, lt.tm_mon + 1, lt.tm_mday);
    }
}
