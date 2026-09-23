#include "ota.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

#include <cstring>

#include "secrets.h"

#ifndef VIBE_OTA_PASSWORD
#define VIBE_OTA_PASSWORD ""
#endif

namespace ota {
namespace {

constexpr std::uint16_t kOtaPort = 3232;
Hooks s_hooks{};
char s_hostname[32] = {};
bool s_enabled = false;
bool s_started = false;
std::uint8_t s_lastPct = 255;

const char* errorText(ota_error_t error) {
    switch (error) {
        case OTA_AUTH_ERROR: return "wrong password";
        case OTA_BEGIN_ERROR: return "not enough space";
        case OTA_CONNECT_ERROR: return "connect failed";
        case OTA_RECEIVE_ERROR: return "transfer interrupted";
        case OTA_END_ERROR: return "image check failed";
    }
    return "unknown error";
}

void start() {
    ArduinoOTA.setHostname(s_hostname);  // also the mDNS name: <hostname>.local
    ArduinoOTA.setPort(kOtaPort);
    ArduinoOTA.setPassword(VIBE_OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        s_lastPct = 255;
        Serial.println("OTA: update starting");
        if (s_hooks.onStart) s_hooks.onStart();
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        const auto pct = static_cast<std::uint8_t>(total ? (100ULL * done / total) : 0);
        if (pct != s_lastPct) {
            s_lastPct = pct;
            if (s_hooks.onProgress) s_hooks.onProgress(pct);
        }
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("OTA: complete, rebooting");
        if (s_hooks.onEnd) s_hooks.onEnd();
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA: failed (%s)\n", errorText(error));
        if (s_hooks.onError) s_hooks.onError(errorText(error));
    });
    ArduinoOTA.begin();
    s_started = true;
    Serial.printf("OTA ready: %s.local / %s port %u\n", s_hostname,
                  WiFi.localIP().toString().c_str(), kOtaPort);
}

}  // namespace

void begin(const char* hostname, const Hooks& hooks) {
    s_hooks = hooks;
    std::strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    s_enabled = std::strlen(VIBE_OTA_PASSWORD) >= 8;
    if (!s_enabled) {
        Serial.println("OTA disabled: set VIBE_OTA_PASSWORD (8+ chars) in include/secrets.h");
    }
}

void loop() {
    if (!s_enabled || WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (!s_started) {
        start();  // needs the network up (UDP listener + mDNS)
    }
    ArduinoOTA.handle();
}

bool enabled() {
    return s_enabled;
}

}  // namespace ota
