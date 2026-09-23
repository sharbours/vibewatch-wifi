#include "voice.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "net_link.h"

namespace voice {
namespace {

// Uplink: 16 kHz, 16-bit, mono PCM. 512 samples = 32 ms per mic buffer.
constexpr std::uint32_t kMicRate = 16000;
constexpr std::size_t kMicChunkSamples = 512;
constexpr std::size_t kMicChunkBytes = kMicChunkSamples * sizeof(std::int16_t);
// ~1 s of slack so a slow UI frame never drops audio.
constexpr std::size_t kStreamBytes = kMicRate * sizeof(std::int16_t);
constexpr std::size_t kSendBytes = 2048;  // 64 ms per WebSocket frame
constexpr std::uint32_t kMaxUtteranceMs = 30000;

// Downlink: bridge announces size/rate, then sends raw PCM16 mono.
constexpr std::size_t kMaxReplyBytes = 4 * 1024 * 1024;  // ~95 s @ 22.05 kHz
constexpr int kVoiceChannel = 2;  // sound.cpp uses channels 0 and 1

std::int16_t* s_micBuf[3] = {};
StreamBufferHandle_t s_stream = nullptr;
TaskHandle_t s_micTask = nullptr;
volatile bool s_micRun = false;
volatile bool s_micTaskDone = true;

bool s_pending = false;
bool s_capturing = false;
int s_slot = 0;
int s_profile = 0;
std::uint32_t s_captureStartedAt = 0;
std::uint8_t s_volume = 180;

std::uint8_t* s_reply = nullptr;
std::size_t s_replyExpected = 0;
std::size_t s_replyReceived = 0;
std::uint32_t s_replyRate = 22050;
bool s_receiving = false;
bool s_playing = false;

std::uint8_t s_sendScratch[kSendBytes];

void freeReply() {
    if (s_reply != nullptr) {
        heap_caps_free(s_reply);
        s_reply = nullptr;
    }
    s_replyExpected = 0;
    s_replyReceived = 0;
    s_receiving = false;
    s_playing = false;
}

void stopPlayback() {
    if (s_playing) {
        M5.Speaker.stop(kVoiceChannel);
    }
    freeReply();
}

// Records on its own task so rendering the 466x466 AMOLED can never starve
// the I2S DMA. Completed buffers go into a stream buffer drained by loop().
void micTask(void*) {
    int index = 0;
    unsigned calls = 0;
    while (s_micRun) {
        // record() queues up to two buffers and blocks on the third, so when
        // it returns, the buffer queued two calls ago is complete.
        if (M5.Mic.record(s_micBuf[index], kMicChunkSamples, kMicRate)) {
            if (++calls >= 3) {
                const int done = (index + 1) % 3;
                xStreamBufferSend(s_stream, s_micBuf[done], kMicChunkBytes, pdMS_TO_TICKS(20));
            }
            index = (index + 1) % 3;
        } else {
            vTaskDelay(1);
        }
    }
    while (M5.Mic.isRecording()) {
        vTaskDelay(1);
    }
    // Flush the last one or two buffers that were still in flight.
    if (calls >= 2) {
        xStreamBufferSend(s_stream, s_micBuf[(index + 1) % 3], kMicChunkBytes, pdMS_TO_TICKS(20));
    }
    if (calls >= 1) {
        xStreamBufferSend(s_stream, s_micBuf[(index + 2) % 3], kMicChunkBytes, pdMS_TO_TICKS(20));
    }
    s_micTaskDone = true;
    s_micTask = nullptr;
    vTaskDelete(nullptr);
}

void drainMic() {
    std::size_t got;
    while ((got = xStreamBufferReceive(s_stream, s_sendScratch, sizeof(s_sendScratch), 0)) > 0) {
        net::sendBinary(s_sendScratch, got);
    }
}

void startCaptureNow() {
    M5.Speaker.end();
    auto cfg = M5.Mic.config();
    cfg.sample_rate = kMicRate;
    cfg.noise_filter_level = 64;
    M5.Mic.config(cfg);
    if (!M5.Mic.begin()) {
        Serial.println("Mic begin failed");
        M5.Speaker.begin();
        return;
    }
    xStreamBufferReset(s_stream);

    char start[112];
    const int n = std::snprintf(start, sizeof(start),
        "{\"m\":\"voice.start\",\"p\":{\"slot\":%d,\"profile\":%d,\"rate\":%u,\"fmt\":\"s16le\"}}",
        s_slot, s_profile, static_cast<unsigned>(kMicRate));
    net::sendText(start, n);

    s_micRun = true;
    s_micTaskDone = false;
    xTaskCreatePinnedToCore(micTask, "vw_mic", 4096, nullptr, 5, &s_micTask, 0);
    s_capturing = true;
    s_captureStartedAt = millis();
    Serial.println("PTT capture started");
}

void finishCapture() {
    s_micRun = false;
    const std::uint32_t waitStart = millis();
    while (!s_micTaskDone && millis() - waitStart < 400) {
        drainMic();
        delay(2);
    }
    drainMic();
    M5.Mic.end();
    M5.Speaker.begin();
    s_capturing = false;
    const char* end = "{\"m\":\"voice.end\"}";
    net::sendText(end, std::strlen(end));
    Serial.printf("PTT capture ended (%lu ms)\n",
                  static_cast<unsigned long>(millis() - s_captureStartedAt));
}

void startPlayback() {
    s_receiving = false;
    if (s_reply == nullptr || s_replyReceived < 2) {
        freeReply();
        return;
    }
    if (s_capturing) {
        // User is already talking again; drop the stale reply.
        freeReply();
        return;
    }
    M5.Speaker.setVolume(s_volume);
    const std::size_t samples = s_replyReceived / sizeof(std::int16_t);
    s_playing = M5.Speaker.playRaw(reinterpret_cast<const std::int16_t*>(s_reply), samples,
                                   s_replyRate, false, 1, kVoiceChannel, true);
    if (!s_playing) {
        freeReply();
    }
}

}  // namespace

void begin() {
    for (auto& buf : s_micBuf) {
        buf = static_cast<std::int16_t*>(
            heap_caps_malloc(kMicChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    s_stream = xStreamBufferCreate(kStreamBytes, 1);
}

void loop() {
    if (s_pending && !M5.Speaker.isPlaying()) {
        // Wait for the PTT chime to finish before handing I2S to the mic.
        s_pending = false;
        startCaptureNow();
    }
    if (s_capturing) {
        drainMic();
        if (millis() - s_captureStartedAt > kMaxUtteranceMs) {
            finishCapture();
        }
    }
    if (s_playing && !M5.Speaker.isPlaying(kVoiceChannel)) {
        freeReply();
    }
}

void pressToTalk(int agentSlot, int profile) {
    if (!net::connected()) {
        return;
    }
    stopPlayback();  // barge-in: talking over the reply cuts it off
    s_slot = agentSlot;
    s_profile = profile;
    s_pending = true;
}

void releaseToTalk() {
    if (s_pending) {
        s_pending = false;  // released before capture actually began
        return;
    }
    if (s_capturing) {
        finishCapture();
    }
}

void stopAll() {
    s_pending = false;
    if (s_capturing) {
        finishCapture();
    }
    stopPlayback();
}

bool isCapturing() {
    return s_capturing || s_pending;
}

bool isSpeaking() {
    return s_playing || s_receiving;
}

void setVolume(std::uint8_t volume) {
    s_volume = volume;
}

void onControl(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        return;
    }
    const char* m = doc["m"] | "";
    if (std::strcmp(m, "tts.begin") == 0) {
        freeReply();
        const std::size_t bytes = doc["p"]["bytes"] | 0U;
        s_replyRate = doc["p"]["rate"] | 22050U;
        if (bytes == 0 || bytes > kMaxReplyBytes) {
            Serial.printf("Reply size rejected: %u\n", static_cast<unsigned>(bytes));
            return;
        }
        s_reply = static_cast<std::uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        if (s_reply == nullptr) {
            Serial.println("PSRAM alloc for reply failed");
            return;
        }
        s_replyExpected = bytes;
        s_replyReceived = 0;
        s_receiving = true;
    } else if (std::strcmp(m, "tts.end") == 0) {
        if (s_receiving) {
            startPlayback();
        }
    } else if (std::strcmp(m, "tts.stop") == 0) {
        stopPlayback();
    }
}

void onAudioChunk(const std::uint8_t* data, std::size_t length) {
    if (!s_receiving || s_reply == nullptr) {
        return;
    }
    const std::size_t room = s_replyExpected - s_replyReceived;
    const std::size_t n = std::min(room, length);
    std::memcpy(s_reply + s_replyReceived, data, n);
    s_replyReceived += n;
}

void onLinkLost() {
    if (s_receiving) {
        freeReply();
    }
}

}  // namespace voice
