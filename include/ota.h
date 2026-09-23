#pragma once

// Firmware updates over Wi-Fi (ArduinoOTA / espota). Disabled unless
// VIBE_OTA_PASSWORD (8+ characters) is defined in include/secrets.h.
//
// ArduinoOTA receives the whole image inside ota::loop(), so the main loop
// doesn't run during an update; the hooks below are how main.cpp keeps the
// screen and hardware in a sane state meanwhile.

#include <cstdint>

namespace ota {

struct Hooks {
    void (*onStart)();                     // stop audio, wake screen
    void (*onProgress)(std::uint8_t pct);  // redraw progress
    void (*onEnd)();                       // about to reboot into the new image
    void (*onError)(const char* reason);   // old firmware keeps running
};

void begin(const char* hostname, const Hooks& hooks);
void loop();  // call every main-loop iteration
bool enabled();

}  // namespace ota
