#pragma once

// Wi-Fi + WebSocket transport that replaces the original BLE HID vendor
// channel. JSON messages keep the exact same shape as the BLE version
// ({"m":"v.oai.hid",...}, {"method":"v.oai.thstatus",...}) so the UI and
// RPC code in main.cpp barely change. Binary frames carry audio.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstddef>
#include <cstdint>

namespace net {

// rpcQueue receives malloc'd, NUL-terminated JSON strings (same contract as
// the original BLE callback). The main loop owns and frees them.
void begin(const char* deviceId, QueueHandle_t rpcQueue);
void loop();

bool wifiUp();
bool connected();

bool sendText(const char* json, std::size_t length);
bool sendText(const String& json);
bool sendBinary(const std::uint8_t* data, std::size_t length);

// Wi-Fi modem sleep. Off while the screen is on (push-to-talk latency), on
// while it is off (the radio then wakes only for AP beacons).
void setPowerSave(bool enabled);

}  // namespace net
