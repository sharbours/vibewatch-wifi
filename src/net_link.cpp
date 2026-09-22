#include "net_link.h"

#include <WiFi.h>
#include <WebSocketsClient.h>

#include <cstdlib>
#include <cstring>

#include "secrets.h"
#include "voice.h"

namespace net {
namespace {

WebSocketsClient s_ws;
QueueHandle_t s_queue = nullptr;
volatile bool s_connected = false;
bool s_wsStarted = false;
String s_deviceId;
String s_headers;
std::uint32_t s_lastWifiAttempt = 0;

// Audio control messages must be handled synchronously, in order with the
// binary frames that follow them, so they bypass the RPC queue.
bool isAudioControl(const char* json) {
    return std::strstr(json, "\"tts.") != nullptr;
}

void onEvent(WStype_t type, std::uint8_t* payload, std::size_t length) {
    switch (type) {
        case WStype_CONNECTED: {
            s_connected = true;
            Serial.printf("Bridge connected: %s\n", reinterpret_cast<char*>(payload));
            String hello = "{\"m\":\"hello\",\"p\":{\"device\":\"" + s_deviceId +
                           "\",\"mic_rate\":16000,\"fw\":\"hermes-wifi\"}}";
            s_ws.sendTXT(hello);
            break;
        }
        case WStype_DISCONNECTED:
            if (s_connected) {
                Serial.println("Bridge disconnected");
            }
            s_connected = false;
            voice::onLinkLost();
            break;
        case WStype_TEXT: {
            if (payload == nullptr || length == 0) {
                break;
            }
            auto* message = static_cast<char*>(std::malloc(length + 1));
            if (message == nullptr) {
                break;
            }
            std::memcpy(message, payload, length);
            message[length] = '\0';
            if (isAudioControl(message)) {
                voice::onControl(message);
                std::free(message);
            } else if (s_queue == nullptr || xQueueSend(s_queue, &message, 0) != pdTRUE) {
                Serial.println("RPC queue full");
                std::free(message);
            }
            break;
        }
        case WStype_BIN:
            voice::onAudioChunk(payload, length);
            break;
        default:
            break;
    }
}

}  // namespace

void begin(const char* deviceId, QueueHandle_t rpcQueue) {
    s_queue = rpcQueue;
    s_deviceId = deviceId;

    // Hostname must be set before the station interface starts.
    WiFi.setHostname(deviceId);
    WiFi.mode(WIFI_STA);
    // Modem sleep adds 100+ ms latency spikes that make push-to-talk choppy.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(VIBE_WIFI_SSID, VIBE_WIFI_PASSWORD);
    s_lastWifiAttempt = millis();

    s_headers = String("Authorization: Bearer ") + VIBE_BRIDGE_TOKEN + "\r\nX-Vibe-Device: " + deviceId;
    s_ws.onEvent(onEvent);
    s_ws.setExtraHeaders(s_headers.c_str());
    s_ws.setReconnectInterval(3000);
    s_ws.enableHeartbeat(15000, 4000, 2);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        s_connected = false;
        if (millis() - s_lastWifiAttempt > 15000) {
            Serial.println("Wi-Fi retry");
            WiFi.reconnect();
            s_lastWifiAttempt = millis();
        }
        return;
    }
    if (!s_wsStarted) {
        Serial.printf("Wi-Fi up: %s -> ws://%s:%d/watch\n", WiFi.localIP().toString().c_str(),
                      VIBE_BRIDGE_HOST, VIBE_BRIDGE_PORT);
        s_ws.begin(VIBE_BRIDGE_HOST, VIBE_BRIDGE_PORT, "/watch");
        s_wsStarted = true;
    }
    s_ws.loop();
}

bool wifiUp() {
    return WiFi.status() == WL_CONNECTED;
}

bool connected() {
    return s_connected;
}

bool sendText(const char* json, std::size_t length) {
    if (!s_connected) {
        return false;
    }
    return s_ws.sendTXT(reinterpret_cast<const std::uint8_t*>(json), length);
}

bool sendText(const String& json) {
    return sendText(json.c_str(), json.length());
}

void setPowerSave(bool enabled) {
    WiFi.setSleep(enabled);
    Serial.printf("Wi-Fi power save %s\n", enabled ? "on" : "off");
}

bool sendBinary(const std::uint8_t* data, std::size_t length) {
    if (!s_connected) {
        return false;
    }
    return s_ws.sendBIN(data, length);
}

}  // namespace net
