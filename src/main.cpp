#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <Preferences.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "net_link.h"
#include "sound.h"
#include "vibe_approval.h"
#include "secrets.h"
#include "vibe_hid.h"
#include "vibe_power.h"
#include "vibe_status.h"
#include "voice.h"

namespace {

// -----------------------------------------------------------------------------
// UI geometry, timing, and persistent-setting keys
// -----------------------------------------------------------------------------

// The StopWatch display is 466 x 466 pixels. All primary controls are arranged
// around the physical center so the layout visually follows the round bezel.
constexpr int kAgentCount = 6;
constexpr int kActionCount = 5;
constexpr int kOkAction = 1;
constexpr int kNgAction = 2;
constexpr int kScreenCenter = 233;
constexpr int kAgentOrbitRadius = 160;
constexpr int kAgentButtonRadius = 55;
constexpr int kMicButtonRadius = 67;
constexpr int kSettingsX = 72;
constexpr int kSettingsY = 370;
constexpr int kSettingsRadius = 22;
constexpr int kSettingsVisualRadius = 16;
constexpr int kSettingsCloseX = 365;
constexpr int kSettingsCloseY = 55;
constexpr int kSettingsCloseRadius = 25;
constexpr int kSettingsSliderLeft = 103;
constexpr int kSettingsSliderRight = 363;
constexpr std::uint32_t kLeftPhysicalButtonColor = 0xFFAC28;
constexpr std::uint32_t kRightPhysicalButtonColor = 0x2D8CFF;
constexpr std::uint32_t kButtonChordGraceMs = 90;
constexpr std::uint32_t kPhysicalMicHoldMs = 350;
constexpr std::uint32_t kUiAnimationPeriodMs = 80;
constexpr std::uint32_t kSelectionAnimationPeriodMs = 16;
constexpr std::uint32_t kSelectionAnimationBaseMs = 88;
constexpr std::uint32_t kBatteryUpdatePeriodMs = 30000;
constexpr char kPreferencesNamespace[] = "vibe-watch";
constexpr char kDeviceSlotKey[] = "device-slot";
constexpr char kSeVolumeKey[] = "se-volume";
constexpr char kVibrationStrengthKey[] = "vibe-strength";
constexpr char kAgentStateVibeKey[] = "state-vibe";

// Hit-test results share one integer space. Non-negative values below
// kAgentCount are outer-ring items; the remaining values identify fixed UI
// controls such as the microphone and settings widgets.
constexpr int kTouchMic = kAgentCount;
constexpr int kTouchSettings = kAgentCount + 1;
constexpr int kTouchSettingsBack = kAgentCount + 2;
constexpr int kTouchSlot1 = kAgentCount + 3;
constexpr int kTouchSlot2 = kAgentCount + 4;
constexpr int kTouchSlot3 = kAgentCount + 5;
constexpr int kTouchPair = kAgentCount + 6;
constexpr int kTouchVolume = kAgentCount + 7;
constexpr int kTouchVibrationStrength = kAgentCount + 8;
constexpr int kTouchAgentStateVibe = kAgentCount + 9;
constexpr int kTouchStatus = kAgentCount + 10;

struct AgentState {
    std::uint32_t color = 0;
    float brightness = 0.0f;
    int effect = 0;
    float speed = 0.0f;
};

struct AmbientState {
    std::uint32_t color = 0x304FFE;
    float brightness = 0.25f;
    int effect = 0;
    float speed = 0.4f;
};

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

// Visual state received from the host. Agent colors communicate per-chat state;
// ambient data is retained for protocol compatibility but is not drawn at the bezel.
std::array<AgentState, kAgentCount> g_agents;
AmbientState g_ambient;
String g_focusedApp;

// RPC messages arrive from the Wi-Fi/WebSocket link (net_link.cpp) through a
// FreeRTOS queue, exactly as they did from the original BLE callback.
QueueHandle_t g_rpcQueue = nullptr;

volatile bool g_connected = false;
volatile bool g_uiDirty = true;
volatile bool g_pairingSuccessPending = false;

// Input state is intentionally explicit because a physical-button press may
// become a single action, a long press, or a two-button layer-switch chord.
int g_activeTouch = -1;
std::uint32_t g_vibrationOffAt = 0;
std::uint32_t g_lastUiDraw = 0;
std::uint32_t g_lastBatteryUpdate = 0;
std::uint8_t g_batteryLevel = 100;
bool g_isCharging = false;
bool g_settingsOpen = false;
int g_deviceSlot = 1;
int g_pendingDeviceSlot = 1;
int g_selectedAgent = 0;
int g_selectedAction = 0;
bool g_planModeEnabled = false;
float g_selectionX = 0.0f;
float g_selectionY = 0.0f;
float g_selectionFromX = 0.0f;
float g_selectionFromY = 0.0f;
float g_selectionToX = 0.0f;
float g_selectionToY = 0.0f;
float g_selectionFromAngle = 0.0f;
float g_selectionToAngle = 0.0f;
std::uint32_t g_selectionAnimationStartedAt = 0;
std::uint32_t g_selectionAnimationDurationMs = 260;
bool g_selectionAnimating = false;
int g_leftAgentPressed = -1;
bool g_leftPressedActionLayer = false;
bool g_leftPressPending = false;
std::uint32_t g_leftPressedAt = 0;
bool g_rightLongTriggered = false;
std::uint32_t g_rightPhysicalPressedAt = 0;
bool g_rightActionPending = false;
bool g_rightActionPressed = false;
std::uint32_t g_rightActionPressedAt = 0;
bool g_buttonChordActive = false;
bool g_actionLayer = false;
bool g_touchActionLayer = false;
std::uint32_t g_restartAt = 0;
char g_deviceName[24] = {};
std::uint8_t g_seVolume = 128;
std::uint8_t g_vibrationStrength = 255;
bool g_agentStateVibeEnabled = true;
std::uint32_t g_lastAgentVibrationAt = 0;

// Approval pop-up. The controller owns the transaction (id, TTL, one pending
// request at a time); these flags only track what is on screen.
vibe::ApprovalController g_approvals;
std::uint32_t g_approvalShownAt = 0;       // 0 = not on screen yet
std::uint32_t g_approvalLastReminder = 0;
std::uint32_t g_approvalLastSecond = 0;
bool g_inputLockUntilRelease = false;      // swallow the press that answered
constexpr std::uint32_t kApprovalArmMs = 700;          // ignore presses right after pop-up
constexpr std::uint32_t kApprovalReminderMs = 30000;

// Power saving (idea from neilshare/vibewatch, MIT). Timeouts can be overridden
// in include/secrets.h.
#ifndef VIBE_DIM_AFTER_S
#define VIBE_DIM_AFTER_S 30
#endif
#ifndef VIBE_SLEEP_AFTER_S
#define VIBE_SLEEP_AFTER_S 90
#endif
constexpr std::uint8_t kActiveBrightness = 80;
constexpr std::uint8_t kDimBrightness = 18;
vibe::PowerController g_power({VIBE_DIM_AFTER_S * 1000UL, VIBE_SLEEP_AFTER_S * 1000UL}, 0);
vibe::PowerState g_appliedPower = vibe::PowerState::Active;

// Hermes status (gauge ring + status page). Adapted from neilshare/vibewatch's
// quota card (MIT): nothing is drawn as live data until the bridge sends it,
// and old data is shown as STALE.
vibe::HostStatus g_hostStatus;
bool g_statusOpen = false;
vibe::StatusFreshness g_lastStatusFreshness = vibe::StatusFreshness::Unavailable;
std::uint32_t g_lastStatusSecond = 0;
constexpr int kStatusRingOuter = 231;
constexpr int kStatusRingInner = 223;
constexpr float kGaugeStartDeg = 115.0f;   // 0 deg = 3 o'clock, clockwise
constexpr float kGaugeSweepDeg = 310.0f;   // gap at the bottom for the status bar

std::array<int, kAgentCount> agentX{};
std::array<int, kAgentCount> agentY{};
std::array<int, kActionCount> actionX{};
std::array<int, kActionCount> actionY{};

// -----------------------------------------------------------------------------
// Preferences, sound, color, haptics, and battery helpers
// -----------------------------------------------------------------------------

void loadPreferences() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, true);
    g_deviceSlot = preferences.getUChar(kDeviceSlotKey, 1);
    g_seVolume = preferences.getUChar(kSeVolumeKey, 128);
    g_vibrationStrength = preferences.getUChar(kVibrationStrengthKey, 255);
    g_agentStateVibeEnabled = preferences.getBool(kAgentStateVibeKey, true);
    preferences.end();
    if (g_deviceSlot < 1 || g_deviceSlot > 3) {
        g_deviceSlot = 1;
    }
    g_pendingDeviceSlot = g_deviceSlot;
    std::snprintf(g_deviceName, sizeof(g_deviceName), "%s%d", vibe::kDeviceNamePrefix, g_deviceSlot);
}

void saveDeviceSlot(int slot) {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kDeviceSlotKey, static_cast<std::uint8_t>(slot));
    preferences.end();
}

void saveSeVolume() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kSeVolumeKey, g_seVolume);
    preferences.end();
}

void saveFeedbackSettings() {
    Preferences preferences;
    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kVibrationStrengthKey, g_vibrationStrength);
    preferences.putBool(kAgentStateVibeKey, g_agentStateVibeEnabled);
    preferences.end();
}

void playSe(float frequency = 880.0f, std::uint32_t durationMs = 35) {
    sound::playSquare(frequency, durationMs, g_seVolume);
}

void playMicSe(bool pressed, std::uint8_t tempoMultiplier = 1) {
    sound::playEffect(
        pressed ? sound::Effect::Start : sound::Effect::StartReverse,
        g_seVolume, tempoMultiplier);
}

void renderUi(std::uint32_t now);

float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

std::uint16_t scaledColor(std::uint32_t packed, float brightness) {
    const float scale = clamp01(brightness);
    const auto r = static_cast<std::uint8_t>(((packed >> 16) & 0xFF) * scale);
    const auto g = static_cast<std::uint8_t>(((packed >> 8) & 0xFF) * scale);
    const auto b = static_cast<std::uint8_t>((packed & 0xFF) * scale);
    return M5.Display.color565(r, g, b);
}

float effectBrightness(int effect, float brightness, float speed, std::uint32_t now) {
    if (effect == 0 || brightness <= 0.0f) {
        return 0.0f;
    }
    if (effect == 4 || effect == 6) {
        const float hz = 0.35f + clamp01(speed) * 1.4f;
        const float phase = static_cast<float>(now % 10000) * 0.001f * hz * 2.0f * PI;
        const float low = effect == 6 ? 0.5f : 0.15f;
        return brightness * (low + (1.0f - low) * (0.5f + 0.5f * std::sin(phase)));
    }
    return brightness;
}

bool uiIsAnimated() {
    if (g_selectionAnimating || g_approvalShownAt != 0) {
        return true;
    }
    for (const auto& state : g_agents) {
        if (state.effect == 4 || state.effect == 6) {
            return true;
        }
    }
    return false;
}

void vibrate(std::uint8_t strength = 120, std::uint32_t durationMs = 25) {
    if (strength == 0 || g_vibrationStrength == 0) {
        return;
    }
    const auto scaledStrength = static_cast<std::uint8_t>(std::max(
        1U, (static_cast<unsigned>(strength) * g_vibrationStrength + 127U) / 255U));
    M5.Power.setVibration(scaledStrength);
    g_vibrationOffAt = millis() + durationMs;
}

void updateBattery(bool notify) {
    const int level = M5.Power.getBatteryLevel();
    if (level >= 0 && level <= 100) {
        g_batteryLevel = static_cast<std::uint8_t>(level);
    }
    g_isCharging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    if (notify && g_connected) {
        char battery[80];
        const int n = std::snprintf(battery, sizeof(battery),
            "{\"m\":\"device.battery\",\"p\":{\"level\":%u,\"charging\":%s}}",
            g_batteryLevel, g_isCharging ? "true" : "false");
        net::sendText(battery, n);
    }
    g_lastBatteryUpdate = millis();
    g_uiDirty = true;
}

// -----------------------------------------------------------------------------
// Host communication
// -----------------------------------------------------------------------------

// JSON messages go out as WebSocket text frames. No 61-byte chunking is
// needed any more; the payload format is unchanged from the BLE version.
void sendFramedJson(String payload, bool /*appendCrlf*/) {
    if (!g_connected) {
        return;
    }
    net::sendText(payload);
}

void sendKeyEvent(const char* key, bool pressed) {
    if (!g_connected) {
        return;
    }
    // Same {"m":"v.oai.hid"} event the Codex host expected, plus the currently
    // selected agent so the bridge knows which Hermes session it applies to.
    char payload[96];
    const int written = std::snprintf(
        payload, sizeof(payload),
        "{\"m\":\"v.oai.hid\",\"p\":{\"k\":\"%s\",\"act\":%u,\"slot\":%d}}", key,
        pressed ? 1U : 0U, g_selectedAgent);
    if (written < 0 || written >= static_cast<int>(sizeof(payload))) {
        Serial.println("HID event payload overflow");
        return;
    }
    net::sendText(payload, written);
    Serial.printf("KEY %s %s\n", key, pressed ? "DOWN" : "UP");
}

void sendAgentEvent(int index, bool pressed) {
    char key[5];
    std::snprintf(key, sizeof(key), "AG%02d", index);
    sendKeyEvent(key, pressed);
}

void sendActionEvent(int index, bool pressed) {
    char key[6];
    std::snprintf(key, sizeof(key), "ACT%02d", index);
    sendKeyEvent(key, pressed);
}

void sendMicEvent(bool pressed) {
    // The original firmware only sent ACT10/ACT11 and let the Mac do the
    // dictation. Here the watch records its own microphone and streams PCM
    // to the bridge, which runs speech-to-text before calling Hermes.
    if (pressed) {
        voice::pressToTalk(g_selectedAgent);
    } else {
        voice::releaseToTalk();
    }
}

// Apply the host's compact agent-state array directly to the six ring buttons.
void applyAgentStatus(JsonVariantConst params) {
    if (!params.is<JsonArrayConst>()) {
        return;
    }
    bool anyAgentChanged = false;
    for (JsonObjectConst item : params.as<JsonArrayConst>()) {
        const int id = item["id"] | -1;
        if (id < 0 || id >= kAgentCount) {
            continue;
        }
        AgentState next;
        next.color = item["c"] | 0U;
        next.brightness = item["b"] | 0.0f;
        next.effect = item["e"] | 0;
        next.speed = item["s"] | 0.0f;
        auto& state = g_agents[id];
        const bool changed = state.color != next.color ||
                             std::abs(state.brightness - next.brightness) > 0.001f ||
                             state.effect != next.effect ||
                             std::abs(state.speed - next.speed) > 0.001f;
        anyAgentChanged = anyAgentChanged || changed;
        state = next;
    }

    // Agent-state changes use haptics only. One compact cue represents a host
    // update even when several agents change together, with rapid updates throttled.
    const std::uint32_t now = millis();
    if (anyAgentChanged && g_agentStateVibeEnabled &&
        now - g_lastAgentVibrationAt >= 120) {
        vibrate(190, 42);
        g_lastAgentVibrationAt = now;
    }
    g_uiDirty = true;
}

void applyAmbientStatus(JsonVariantConst params) {
    JsonObjectConst ambient = params["ambient"].as<JsonObjectConst>();
    if (ambient.isNull()) {
        return;
    }
    g_ambient.color = ambient["c"] | 0U;
    g_ambient.brightness = ambient["b"] | 0.0f;
    g_ambient.effect = ambient["e"] | 0;
    g_ambient.speed = ambient["s"] | 0.0f;
    g_uiDirty = true;
}

void applyFocusedApp(JsonVariantConst params) {
    const char* appName = params["appName"] | "";
    g_focusedApp = appName;
    if (g_focusedApp.length() > 18) {
        g_focusedApp = g_focusedApp.substring(0, 17) + "…";
    }
    g_uiDirty = true;
}

void sendRpcResponse(const char* method, int id) {
    JsonDocument response;
    response["id"] = id;
    response["method"] = method;

    if (std::strcmp(method, "device.status") == 0) {
        updateBattery(false);
        JsonObject result = response["result"].to<JsonObject>();
        result["version"] = vibe::kFirmwareVersion;
        result["profile_index"] = 0;
        result["layer_index"] = 1;
        result["battery"] = g_batteryLevel;
        result["is_charging"] = g_isCharging;
    } else if (std::strcmp(method, "sys.version") == 0) {
        response["result"]["version"] = vibe::kFirmwareVersion;
    } else {
        response["result"]["ok"] = 1;
    }

    String json;
    serializeJson(response, json);
    sendFramedJson(json, true);
    Serial.printf("RPC response: %s id=%d\n", method, id);
}

// -----------------------------------------------------------------------------
// Approval transport
// -----------------------------------------------------------------------------

void sendApprovalDecision(const vibe::ApprovalDecision& decision) {
    char json[160];
    const int n = std::snprintf(json, sizeof(json),
        "{\"m\":\"approval.decision\",\"p\":{\"id\":\"%s\",\"choice\":\"%s\"}}",
        decision.id, vibe::approvalChoiceName(decision.choice));
    if (n > 0 && n < static_cast<int>(sizeof(json))) {
        net::sendText(json, n);
    }
    Serial.printf("Approval %.8s -> %s\n", decision.id, vibe::approvalChoiceName(decision.choice));
}

void hideApproval() {
    g_approvalShownAt = 0;
    g_uiDirty = true;
}

void applyApprovalRequest(JsonVariantConst params) {
    vibe::ApprovalRequest request;
    vibe::copyUtf8(request.id, sizeof(request.id), params["id"] | "");
    vibe::copyUtf8(request.kind, sizeof(request.kind), params["kind"] | "EXEC");
    vibe::copyUtf8(request.title, sizeof(request.title), params["title"] | "");
    vibe::copyUtf8(request.detail, sizeof(request.detail), params["detail"] | "");
    const int slot = params["slot"] | 0;
    request.slot = static_cast<std::uint8_t>(slot >= 0 && slot < kAgentCount ? slot : 0);
    request.ttlMs = params["ttl_ms"] | 60000U;

    const auto result = g_approvals.accept(request, millis());
    if (result == vibe::ApprovalAcceptResult::Busy ||
        result == vibe::ApprovalAcceptResult::Invalid) {
        // Tell the bridge immediately; it keeps its own queue and retries.
        char json[160];
        const int n = std::snprintf(json, sizeof(json),
            "{\"m\":\"approval.decision\",\"p\":{\"id\":\"%s\",\"choice\":\"%s\"}}",
            request.id, result == vibe::ApprovalAcceptResult::Busy ? "busy" : "invalid");
        if (n > 0 && n < static_cast<int>(sizeof(json))) {
            net::sendText(json, n);
        }
        return;
    }
    if (result == vibe::ApprovalAcceptResult::Accepted) {
        Serial.printf("Approval request %.8s slot=%d: %s\n", request.id, request.slot + 1,
                      request.title);
    }
    g_uiDirty = true;  // shown by approvalLoop() once push-to-talk is idle
}

void applyApprovalCancel(JsonVariantConst params) {
    if (g_approvals.withdraw(params["id"] | "")) {
        hideApproval();
        Serial.println("Approval withdrawn by host");
    }
}

void applyHostStatus(JsonVariantConst params) {
    vibe::StatusInput in;
    in.health = params["health"] | "";
    in.runs = params["runs"] | 0;
    in.jobs = params["jobs"] | 0;
    in.tokensToday = params["tokens_today"] | 0.0;
    in.budget = params["budget"] | 0.0;
    in.remainingPercent = params["remaining_pct"] | 100.0;
    in.resetInSeconds = params["reset_s"] | static_cast<std::int64_t>(0);
    in.staleAfterSeconds = params["ttl_s"] | 90U;
    JsonArrayConst slots = params["slot_tokens"].as<JsonArrayConst>();
    int i = 0;
    for (JsonVariantConst v : slots) {
        if (i >= vibe::kStatusSlotCount) break;
        in.slotTokens[i++] = v | 0.0;
    }
    if (!g_hostStatus.apply(in, millis())) {
        Serial.println("host.status rejected (out of range)");
        return;
    }
    g_uiDirty = true;
}

void processRpc(const char* json) {
    JsonDocument request;
    const DeserializationError error = deserializeJson(request, json);
    if (error) {
        Serial.printf("RPC parse failed: %s\n", error.c_str());
        return;
    }

    const char* method = request["method"] | request["m"] | "";
    int id = request["id"] | request["i"] | -1;
    JsonVariantConst params = request["params"];
    if (params.isNull()) {
        params = request["p"];
    }

    if (std::strcmp(method, "v.oai.thstatus") == 0) {
        applyAgentStatus(params);
    } else if (std::strcmp(method, "v.oai.rgbcfg") == 0) {
        applyAmbientStatus(params);
    } else if (std::strcmp(method, "host.focused_app") == 0) {
        applyFocusedApp(params);
    } else if (std::strcmp(method, "host.status") == 0) {
        applyHostStatus(params);
    } else if (std::strcmp(method, "approval.request") == 0) {
        applyApprovalRequest(params);
    } else if (std::strcmp(method, "approval.cancel") == 0) {
        applyApprovalCancel(params);
    }

    if (id >= 0 && method[0] != '\0') {
        sendRpcResponse(method, id);
    }
}

void initializeNetwork() {
    // The device slot (#1-#3) now just names this watch on the network so
    // the bridge can tell several watches apart.
    char deviceId[24];
    std::snprintf(deviceId, sizeof(deviceId), "vibe-watch-%d", g_deviceSlot);
    voice::begin();
    voice::setVolume(g_seVolume);
    net::begin(deviceId, g_rpcQueue);
}

void beginPairing() {
    // "CONNECT" in Settings: save the chosen slot and restart so the watch
    // rejoins Wi-Fi and the bridge under the new device id.
    saveDeviceSlot(g_pendingDeviceSlot);
    g_connected = false;
    g_deviceSlot = g_pendingDeviceSlot;
    std::snprintf(g_deviceName, sizeof(g_deviceName), "%s%d", vibe::kDeviceNamePrefix, g_deviceSlot);
    g_restartAt = millis() + 900;
    g_uiDirty = true;
    Serial.printf("Reconnect requested for %s; restarting\n", g_deviceName);
}

// -----------------------------------------------------------------------------
// Circular layout and agent-selection animation
// -----------------------------------------------------------------------------

void initializeAgentPositions() {
    for (int i = 0; i < kAgentCount; ++i) {
        // Flat-top hexagon: leaves a readable status area at the bottom while
        // pushing all six agent buttons toward the circular bezel.
        const float angle = (-120.0f + 60.0f * i) * PI / 180.0f;
        agentX[i] = kScreenCenter + static_cast<int>(std::cos(angle) * kAgentOrbitRadius);
        agentY[i] = kScreenCenter + static_cast<int>(std::sin(angle) * kAgentOrbitRadius);
    }
    // FAST, OK, NG, PLAN, AI. OK/NG sit directly below the left/right
    // physical buttons; the other three follow the lower circular edge.
    actionX = {70, 354, 112, 233, 396};
    actionY = {250, 105, 105, 368, 250};
    g_selectionX = static_cast<float>(agentX[g_selectedAgent]);
    g_selectionY = static_cast<float>(agentY[g_selectedAgent]);
    g_selectionFromX = g_selectionToX = g_selectionX;
    g_selectionFromY = g_selectionToY = g_selectionY;
    g_selectionFromAngle = g_selectionToAngle =
        std::atan2(g_selectionY - kScreenCenter, g_selectionX - kScreenCenter);
}

float selectionProgress(std::uint32_t now) {
    if (!g_selectionAnimating) {
        return 1.0f;
    }
    return clamp01(static_cast<float>(now - g_selectionAnimationStartedAt) /
                   static_cast<float>(g_selectionAnimationDurationMs));
}

float snappySelectionProgress(float progress) {
    // Ease-out-back moves decisively, overshoots by a few pixels, then snaps
    // onto the target like a spring-loaded watch mechanism.
    if (progress >= 1.0f) {
        return 1.0f;
    }
    constexpr float kOvershoot = 1.10f;
    const float shifted = progress - 1.0f;
    return 1.0f + (kOvershoot + 1.0f) * shifted * shifted * shifted +
           kOvershoot * shifted * shifted;
}

void selectionPositionAt(std::uint32_t now, float& x, float& y) {
    const float eased = snappySelectionProgress(selectionProgress(now));
    const float angle = g_selectionFromAngle +
                        (g_selectionToAngle - g_selectionFromAngle) * eased;
    x = kScreenCenter + std::cos(angle) * kAgentOrbitRadius;
    y = kScreenCenter + std::sin(angle) * kAgentOrbitRadius;
}

void selectAgent(int index) {
    if (index < 0 || index >= kAgentCount || index == g_selectedAgent) {
        return;
    }
    const std::uint32_t now = millis();
    selectionPositionAt(now, g_selectionX, g_selectionY);
    const float currentAngle = std::atan2(g_selectionY - kScreenCenter,
                                          g_selectionX - kScreenCenter);
    const float targetAngle = std::atan2(static_cast<float>(agentY[index] - kScreenCenter),
                                         static_cast<float>(agentX[index] - kScreenCenter));
    float angleDelta = targetAngle - currentAngle;
    while (angleDelta > PI) {
        angleDelta -= 2.0f * PI;
    }
    while (angleDelta < -PI) {
        angleDelta += 2.0f * PI;
    }
    g_selectionFromX = g_selectionX;
    g_selectionFromY = g_selectionY;
    g_selectionToX = static_cast<float>(agentX[index]);
    g_selectionToY = static_cast<float>(agentY[index]);
    g_selectionFromAngle = currentAngle;
    g_selectionToAngle = currentAngle + angleDelta;
    // Nearby steps complete in about 123 ms; even a half-turn finishes under
    // 200 ms so physical-button navigation feels immediate.
    g_selectionAnimationDurationMs = kSelectionAnimationBaseMs +
        static_cast<std::uint32_t>(105.0f * std::abs(angleDelta) / PI);
    g_selectionAnimationStartedAt = now;
    g_selectionAnimating = true;
    g_selectedAgent = index;
    g_uiDirty = true;
}

// -----------------------------------------------------------------------------
// Touch and physical-button input
// -----------------------------------------------------------------------------

bool pointInRect(int x, int y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

int hitTestSettings(int x, int y) {
    const int backDx = x - kSettingsCloseX;
    const int backDy = y - kSettingsCloseY;
    if (backDx * backDx + backDy * backDy <= kSettingsCloseRadius * kSettingsCloseRadius) {
        return kTouchSettingsBack;
    }
    for (int i = 0; i < 3; ++i) {
        const int dx = x - (122 + i * 111);
        const int dy = y - 112;
        if (dx * dx + dy * dy <= 34 * 34) {
            return kTouchSlot1 + i;
        }
    }
    if (pointInRect(x, y, 153, 151, 160, 45)) {
        return kTouchPair;
    }
    if (pointInRect(x, y, 80, 205, 306, 55)) {
        return kTouchVolume;
    }
    if (pointInRect(x, y, 80, 267, 306, 55)) {
        return kTouchVibrationStrength;
    }
    if (pointInRect(x, y, 145, 352, 180, 55)) {
        return kTouchAgentStateVibe;
    }
    return -1;
}

int hitTestMain(int x, int y) {
    if (!g_actionLayer && y >= 418 && x >= 143 && x <= 323) {
        return kTouchStatus;  // the status pill at the bottom
    }
    const int settingsDx = x - kSettingsX;
    const int settingsDy = y - kSettingsY;
    if (!g_actionLayer &&
        settingsDx * settingsDx + settingsDy * settingsDy <= kSettingsRadius * kSettingsRadius) {
        return kTouchSettings;
    }
    const int outerCount = g_actionLayer ? kActionCount : kAgentCount;
    for (int i = 0; i < outerCount; ++i) {
        const int dx = x - (g_actionLayer ? actionX[i] : agentX[i]);
        const int dy = y - (g_actionLayer ? actionY[i] : agentY[i]);
        if (dx * dx + dy * dy <= kAgentButtonRadius * kAgentButtonRadius) {
            return i;
        }
    }
    const int dx = x - kScreenCenter;
    const int dy = y - kScreenCenter;
    if (dx * dx + dy * dy <= kMicButtonRadius * kMicButtonRadius) {
        return kTouchMic;
    }
    return -1;
}

void updateVolumeFromTouch(int x) {
    const int boundedX = std::max(kSettingsSliderLeft, std::min(kSettingsSliderRight, x));
    g_seVolume = static_cast<std::uint8_t>(
        (boundedX - kSettingsSliderLeft) * 255 /
        (kSettingsSliderRight - kSettingsSliderLeft));
    M5.Speaker.setVolume(g_seVolume);
    g_uiDirty = true;
}

void updateVibrationStrengthFromTouch(int x) {
    const int boundedX = std::max(kSettingsSliderLeft, std::min(kSettingsSliderRight, x));
    g_vibrationStrength = static_cast<std::uint8_t>(
        (boundedX - kSettingsSliderLeft) * 255 /
        (kSettingsSliderRight - kSettingsSliderLeft));
    g_uiDirty = true;
}

void sendOuterActionEvent(int index, bool pressed) {
    if (index == 3 && pressed) {
        g_planModeEnabled = !g_planModeEnabled;
        g_uiDirty = true;
    }
    sendActionEvent(index == 4 ? 12 : 6 + index, pressed);
}

void playOuterActionPressSe(int index) {
    if (index == kOkAction) {
        playSe(659.25f, 34);
        delay(42);
        playSe(987.77f, 86);
        return;
    }
    if (index == kNgAction) {
        playSe(392.00f, 42);
        delay(50);
        playSe(293.66f, 105);
        return;
    }
    playSe(900.0f + index * 75.0f, 40);
}

void handleSettingsTouch(const m5::Touch_Class::touch_detail_t& touch) {
    if (touch.wasPressed()) {
        g_activeTouch = hitTestSettings(touch.x, touch.y);
        if (g_activeTouch == kTouchSettingsBack) {
            g_settingsOpen = false;
            playSe(540.0f);
            vibrate(80, 20);
        } else if (g_activeTouch >= kTouchSlot1 && g_activeTouch <= kTouchSlot3) {
            g_pendingDeviceSlot = 1 + g_activeTouch - kTouchSlot1;
            playSe(760.0f + g_pendingDeviceSlot * 90.0f);
            vibrate(80, 20);
        } else if (g_activeTouch == kTouchPair) {
            playSe(1100.0f, 55);
            vibrate(150, 35);
        } else if (g_activeTouch == kTouchVolume) {
            updateVolumeFromTouch(touch.x);
        } else if (g_activeTouch == kTouchVibrationStrength) {
            updateVibrationStrengthFromTouch(touch.x);
        } else if (g_activeTouch == kTouchAgentStateVibe) {
            g_agentStateVibeEnabled = !g_agentStateVibeEnabled;
            saveFeedbackSettings();
            playSe(g_agentStateVibeEnabled ? 1040.0f : 620.0f, 45);
            vibrate(130, 28);
        }
        g_uiDirty = true;
    }
    if (touch.isPressed()) {
        if (g_activeTouch == kTouchVolume) {
            updateVolumeFromTouch(touch.x);
        } else if (g_activeTouch == kTouchVibrationStrength) {
            updateVibrationStrengthFromTouch(touch.x);
        }
    }
    if (touch.wasReleased() && g_activeTouch >= 0) {
        if (g_activeTouch == kTouchPair) {
            beginPairing();
        } else if (g_activeTouch == kTouchVolume) {
            saveSeVolume();
            voice::setVolume(g_seVolume);
            playSe(980.0f, 70);
        } else if (g_activeTouch == kTouchVibrationStrength) {
            saveFeedbackSettings();
            vibrate(255, 70);
        }
        g_activeTouch = -1;
        g_uiDirty = true;
    }
}

void handleTouch() {
    const auto touch = M5.Touch.getDetail();
    if (g_settingsOpen) {
        handleSettingsTouch(touch);
        return;
    }

    if (touch.wasPressed()) {
        g_activeTouch = hitTestMain(touch.x, touch.y);
        // Remember the layer from touch-down through touch-up. A layer change
        // during the gesture must not release a different host-side control.
        g_touchActionLayer = g_actionLayer;
        if (g_activeTouch >= 0 && g_activeTouch < kAgentCount) {
            if (g_touchActionLayer) {
                g_selectedAction = g_activeTouch;
                sendOuterActionEvent(g_activeTouch, true);
                playOuterActionPressSe(g_activeTouch);
            } else {
                selectAgent(g_activeTouch);
                sendAgentEvent(g_activeTouch, true);
                playSe(820.0f + g_activeTouch * 55.0f);
            }
            vibrate();
        } else if (g_activeTouch == kTouchMic) {
            // The center is always a dedicated PTT button. Send DOWN at the
            // touch edge so AI assistant starts listening immediately.
            sendMicEvent(true);
            playMicSe(true);
            vibrate(150, 35);
        } else if (g_activeTouch == kTouchStatus) {
            g_statusOpen = true;
            playSe(700.0f);
            vibrate(80, 20);
        } else if (g_activeTouch == kTouchSettings) {
            g_settingsOpen = true;
            g_pendingDeviceSlot = g_deviceSlot;
            playSe(760.0f);
            vibrate(80, 20);
        }
        g_uiDirty = true;
    }

    if (touch.wasReleased() && g_activeTouch >= 0) {
        if (g_activeTouch < kAgentCount) {
            if (g_touchActionLayer) {
                sendOuterActionEvent(g_activeTouch, false);
            } else {
                sendAgentEvent(g_activeTouch, false);
            }
        } else if (g_activeTouch == kTouchMic) {
            sendMicEvent(false);
            playMicSe(false);
        }
        g_activeTouch = -1;
        g_uiDirty = true;
    }
}

void handlePhysicalButtons() {
    // Treat the two physical buttons as a chord before dispatching either
    // single-button action. The short grace period prevents an Agent/OK/NG
    // event from leaking out when the user's intention is to switch layers.
    if (!g_buttonChordActive && M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
        if (g_leftAgentPressed >= 0) {
            if (g_leftPressedActionLayer) {
                sendOuterActionEvent(g_leftAgentPressed, false);
            } else {
                sendAgentEvent(g_leftAgentPressed, false);
            }
            g_leftAgentPressed = -1;
        }
        if (g_rightLongTriggered) {
            sendMicEvent(false);
            playMicSe(false);
            g_rightLongTriggered = false;
        }
        g_rightPhysicalPressedAt = 0;
        if (g_rightActionPressed) {
            sendOuterActionEvent(kOkAction, false);
            g_rightActionPressed = false;
        }
        g_leftPressPending = false;
        g_rightActionPending = false;
        g_actionLayer = !g_actionLayer;
        g_selectionAnimating = false;
        g_selectionX = g_selectionToX = static_cast<float>(agentX[g_selectedAgent]);
        g_selectionY = g_selectionToY = static_cast<float>(agentY[g_selectedAgent]);
        g_selectionFromX = g_selectionX;
        g_selectionFromY = g_selectionY;
        g_selectionFromAngle = g_selectionToAngle =
            std::atan2(g_selectionY - kScreenCenter, g_selectionX - kScreenCenter);
        g_buttonChordActive = true;
        playSe(g_actionLayer ? 1120.0f : 680.0f, 48);
        vibrate(170, 42);
        g_uiDirty = true;
        renderUi(millis());
        return;
    }
    if (g_buttonChordActive) {
        if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
            g_buttonChordActive = false;
        }
        return;
    }

    if (M5.BtnA.wasPressed()) {
        // Wait briefly so a near-simultaneous right press can become a chord
        // without emitting an unwanted Agent or action click first.
        g_leftPressPending = true;
        g_leftPressedAt = millis();
    }
    if (g_leftPressPending && M5.BtnA.isPressed() &&
        millis() - g_leftPressedAt >= kButtonChordGraceMs) {
        g_leftPressPending = false;
        g_leftPressedActionLayer = g_actionLayer;
        if (g_actionLayer) {
            g_selectedAction = kNgAction;
            g_leftAgentPressed = kNgAction;
            sendOuterActionEvent(g_leftAgentPressed, true);
            playOuterActionPressSe(g_leftAgentPressed);
        } else {
            selectAgent((g_selectedAgent + 1) % kAgentCount);
            g_leftAgentPressed = g_selectedAgent;
            sendAgentEvent(g_leftAgentPressed, true);
            playSe(820.0f + g_selectedAgent * 55.0f, 33);
        }
        vibrate();
        g_uiDirty = true;
    }
    if (M5.BtnA.wasReleased()) {
        if (g_leftPressPending) {
            // Preserve very quick single clicks that end inside the chord
            // grace window.
            g_leftPressPending = false;
            g_leftPressedActionLayer = g_actionLayer;
            if (g_actionLayer) {
                g_selectedAction = kNgAction;
                g_leftAgentPressed = kNgAction;
                sendOuterActionEvent(g_leftAgentPressed, true);
                playOuterActionPressSe(g_leftAgentPressed);
            } else {
                selectAgent((g_selectedAgent + 1) % kAgentCount);
                g_leftAgentPressed = g_selectedAgent;
                sendAgentEvent(g_leftAgentPressed, true);
                playSe(820.0f + g_selectedAgent * 55.0f, 33);
            }
            delay(12);
        }
        if (g_leftAgentPressed >= 0) {
            if (g_leftPressedActionLayer) {
                sendOuterActionEvent(g_leftAgentPressed, false);
            } else {
                sendAgentEvent(g_leftAgentPressed, false);
            }
            g_leftAgentPressed = -1;
        }
        g_leftPressedActionLayer = false;
        g_uiDirty = true;
    }

    if (g_actionLayer && M5.BtnB.wasPressed()) {
        // In Action mode the physical buttons map spatially to OK and NG.
        // Delay NG briefly for the same chord-detection reason as the left key.
        g_rightActionPending = true;
        g_rightActionPressedAt = millis();
    }
    if (g_actionLayer && g_rightActionPending && M5.BtnB.isPressed() &&
        millis() - g_rightActionPressedAt >= kButtonChordGraceMs) {
        g_rightActionPending = false;
        g_rightActionPressed = true;
        g_selectedAction = kOkAction;
        sendOuterActionEvent(kOkAction, true);
        playOuterActionPressSe(kOkAction);
        vibrate();
        g_uiDirty = true;
    }
    if (g_actionLayer && M5.BtnB.wasReleased()) {
        if (g_rightActionPending) {
            g_rightActionPending = false;
            g_selectedAction = kOkAction;
            sendOuterActionEvent(kOkAction, true);
            playOuterActionPressSe(kOkAction);
            delay(12);
            sendOuterActionEvent(kOkAction, false);
            vibrate();
        } else if (g_rightActionPressed) {
            sendOuterActionEvent(kOkAction, false);
            g_rightActionPressed = false;
        }
        g_uiDirty = true;
        return;
    }
    if (g_actionLayer) {
        return;
    }

    if (M5.BtnB.wasPressed()) {
        // Outside Action mode, a right-button tap invokes the assistant and a
        // hold becomes push-to-talk. The threshold keeps both gestures quick.
        g_rightLongTriggered = false;
        g_rightPhysicalPressedAt = millis();
        g_uiDirty = true;
        renderUi(millis());
    }
    if (M5.BtnB.isPressed() && !g_rightLongTriggered &&
        millis() - g_rightPhysicalPressedAt >= kPhysicalMicHoldMs) {
        g_rightLongTriggered = true;
        sendMicEvent(true);
        vibrate(150, 35);
        g_uiDirty = true;
        renderUi(millis());
        playMicSe(true);
    }
    if (M5.BtnB.wasReleased()) {
        if (g_rightLongTriggered) {
            sendMicEvent(false);
            playMicSe(false);
        } else {
            // A short click is the AI assistant key (ACT12).
            sendActionEvent(12, true);
            delay(12);
            sendActionEvent(12, false);
            playSe(1180.0f, 38);
            vibrate(100, 24);
        }
        g_rightLongTriggered = false;
        g_rightPhysicalPressedAt = 0;
        g_uiDirty = true;
        renderUi(millis());
    }
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

void drawThickCircle(int x, int y, int radius, int thickness, std::uint16_t color) {
    for (int i = 0; i < thickness; ++i) {
        M5.Display.drawCircle(x, y, radius - i, color);
    }
}

void drawThickRoundRect(int x, int y, int width, int height, int radius, int thickness,
                        std::uint16_t color) {
    for (int i = 0; i < thickness; ++i) {
        M5.Display.drawRoundRect(x + i, y + i, width - i * 2, height - i * 2,
                                 std::max(1, radius - i), color);
    }
}

void drawSelectionIndicator(std::uint32_t now) {
    const float rawProgress = selectionProgress(now);
    if (g_selectionAnimating) {
        // Paint oldest samples first. The dim, narrowing rings form a short
        // motion-blur tail without requiring an alpha framebuffer.
        static constexpr std::uint32_t kTrailColors[] = {
            0x30234D, 0x49346F, 0x684A9A, 0x8964C5,
        };
        for (int trail = 4; trail >= 1; --trail) {
            const float sample = std::max(0.0f, rawProgress - trail * 0.11f);
            const float eased = snappySelectionProgress(sample);
            const float angle = g_selectionFromAngle +
                                (g_selectionToAngle - g_selectionFromAngle) * eased;
            const int x = kScreenCenter + static_cast<int>(std::lround(
                std::cos(angle) * kAgentOrbitRadius));
            const int y = kScreenCenter + static_cast<int>(std::lround(
                std::sin(angle) * kAgentOrbitRadius));
            drawThickCircle(x, y, kAgentButtonRadius + 2, std::max(1, 5 - trail),
                            scaledColor(kTrailColors[4 - trail], 1.0f));
        }
    }

    selectionPositionAt(now, g_selectionX, g_selectionY);
    const int x = static_cast<int>(std::lround(g_selectionX));
    const int y = static_cast<int>(std::lround(g_selectionY));
    const float landingProgress = clamp01((rawProgress - 0.62f) / 0.38f);
    const int landingPulse = g_selectionAnimating
        ? static_cast<int>(std::lround(std::sin(landingProgress * PI) * 5.0f))
        : 0;
    const int indicatorRadius = kAgentButtonRadius + landingPulse;
    drawThickCircle(x, y, indicatorRadius + 4, 3, M5.Display.color565(74, 56, 128));
    drawThickCircle(x, y, indicatorRadius, 7, M5.Display.color565(163, 132, 255));
    drawThickCircle(x, y, indicatorRadius - 9, 2, TFT_WHITE);

    if (g_selectionAnimating && rawProgress >= 1.0f) {
        g_selectionAnimating = false;
        g_selectionX = g_selectionFromX = g_selectionToX;
        g_selectionY = g_selectionFromY = g_selectionToY;
        g_selectionFromAngle = g_selectionToAngle;
    }
}

void drawSettingsGlyph(int x, int y, std::uint16_t color) {
    drawThickCircle(x, y, 8, 2, color);
    M5.Display.fillCircle(x, y, 3, color);
    for (int i = 0; i < 8; ++i) {
        const float angle = i * PI / 4.0f;
        const int x1 = x + static_cast<int>(std::cos(angle) * 10);
        const int y1 = y + static_cast<int>(std::sin(angle) * 10);
        const int x2 = x + static_cast<int>(std::cos(angle) * 15);
        const int y2 = y + static_cast<int>(std::sin(angle) * 15);
        M5.Display.drawWideLine(x1, y1, x2, y2, 1.7f, color);
    }
}

void drawMicGlyph(int x, int y, std::uint16_t color) {
    drawThickRoundRect(x - 13, y - 27, 26, 40, 12, 3, color);
    M5.Display.drawWideLine(x - 21, y - 3, x - 21, y + 7, 2.0f, color);
    M5.Display.drawWideLine(x + 21, y - 3, x + 21, y + 7, 2.0f, color);
    M5.Display.drawWideLine(x - 21, y + 7, x - 13, y + 17, 2.0f, color);
    M5.Display.drawWideLine(x + 21, y + 7, x + 13, y + 17, 2.0f, color);
    M5.Display.drawWideLine(x - 13, y + 17, x + 13, y + 17, 2.0f, color);
    M5.Display.drawWideLine(x, y + 17, x, y + 29, 2.0f, color);
    M5.Display.drawWideLine(x - 11, y + 29, x + 11, y + 29, 2.0f, color);
}

void drawLargeMicGlyph(int x, int y, std::uint16_t color) {
    drawThickRoundRect(x - 18, y - 36, 36, 54, 17, 4, color);
    M5.Display.drawWideLine(x - 29, y - 5, x - 29, y + 9, 3.5f, color);
    M5.Display.drawWideLine(x + 29, y - 5, x + 29, y + 9, 3.5f, color);
    M5.Display.drawWideLine(x - 29, y + 9, x - 18, y + 23, 3.5f, color);
    M5.Display.drawWideLine(x + 29, y + 9, x + 18, y + 23, 3.5f, color);
    M5.Display.drawWideLine(x - 18, y + 23, x + 18, y + 23, 3.5f, color);
    M5.Display.drawWideLine(x, y + 23, x, y + 39, 3.5f, color);
    M5.Display.drawWideLine(x - 15, y + 39, x + 15, y + 39, 3.5f, color);
}

void drawAssistantGlyph(int x, int y, std::uint16_t color) {
    drawThickCircle(x, y, 24, 3, color);
    M5.Display.drawWideLine(x - 11, y, x - 3, y - 8, 3.0f, color);
    M5.Display.drawWideLine(x - 3, y - 8, x + 8, y - 5, 3.0f, color);
    M5.Display.drawWideLine(x + 8, y - 5, x + 13, y + 6, 3.0f, color);
    M5.Display.fillCircle(x - 6, y + 7, 2, color);
    M5.Display.fillCircle(x + 7, y + 7, 2, color);
}

void drawPhysicalActionLinks() {
    // Colored rails enter from the real button positions at the top edge and
    // terminate under the OK/NG circles. Pressing either side lights its rail.
    const bool leftActive = g_leftAgentPressed == kNgAction || g_activeTouch == kNgAction;
    const bool rightActive = g_rightActionPressed || g_activeTouch == kOkAction;
    const auto leftColor = scaledColor(kLeftPhysicalButtonColor, 1.0f);
    const auto rightColor = scaledColor(kRightPhysicalButtonColor, 1.0f);
    const auto leftGlow = scaledColor(kLeftPhysicalButtonColor, leftActive ? 0.58f : 0.24f);
    const auto rightGlow = scaledColor(kRightPhysicalButtonColor, rightActive ? 0.58f : 0.24f);

    // Two short segments make each link follow the curved case rather than
    // looking like a generic straight divider.
    M5.Display.drawWideLine(67, 0, 73, 28, leftActive ? 14.0f : 11.0f, leftGlow);
    M5.Display.drawWideLine(73, 28, 85, 58, leftActive ? 14.0f : 11.0f, leftGlow);
    M5.Display.drawWideLine(67, 0, 73, 28, 5.0f, leftActive ? TFT_WHITE : leftColor);
    M5.Display.drawWideLine(73, 28, 85, 58, 5.0f, leftActive ? TFT_WHITE : leftColor);

    M5.Display.drawWideLine(399, 0, 393, 28, rightActive ? 14.0f : 11.0f, rightGlow);
    M5.Display.drawWideLine(393, 28, 381, 58, rightActive ? 14.0f : 11.0f, rightGlow);
    M5.Display.drawWideLine(399, 0, 393, 28, 5.0f, rightActive ? TFT_WHITE : rightColor);
    M5.Display.drawWideLine(393, 28, 381, 58, 5.0f, rightActive ? TFT_WHITE : rightColor);
}

void drawFastGlyph(int x, int y, std::uint16_t color) {
    M5.Display.drawWideLine(x + 10, y - 28, x - 13, y + 1, 7.0f, color);
    M5.Display.drawWideLine(x - 13, y + 1, x + 3, y + 1, 7.0f, color);
    M5.Display.drawWideLine(x + 3, y + 1, x - 8, y + 29, 7.0f, color);
    M5.Display.drawWideLine(x - 8, y + 29, x + 18, y - 7, 7.0f, color);
}

void drawApproveGlyph(int x, int y, std::uint16_t color) {
    M5.Display.drawWideLine(x - 19, y, x - 6, y + 15, 6.0f, color);
    M5.Display.drawWideLine(x - 6, y + 15, x + 21, y - 18, 6.0f, color);
}

void drawRejectGlyph(int x, int y, std::uint16_t color) {
    M5.Display.drawWideLine(x - 18, y - 18, x + 18, y + 18, 6.0f, color);
    M5.Display.drawWideLine(x + 18, y - 18, x - 18, y + 18, 6.0f, color);
}

void drawPlanGlyph(int x, int y, std::uint16_t color, bool enabled) {
    drawThickRoundRect(x - 29, y - 15, 58, 30, 15, 3, color);
    M5.Display.fillCircle(x + (enabled ? 14 : -14), y, 9, color);
}

void drawSwipeChevron(int x, int y, bool pointsRight, std::uint16_t color) {
    const int direction = pointsRight ? 1 : -1;
    M5.Display.drawWideLine(x - direction * 4, y - 7, x + direction * 3, y, 2.0f, color);
    M5.Display.drawWideLine(x + direction * 3, y, x - direction * 4, y + 7, 2.0f, color);
}

void drawCenterActionGlyph(int action, int x, int y, std::uint16_t color) {
    switch (action) {
        case 0:
            drawMicGlyph(x, y - 4, color);
            break;
        case 1:
            drawFastGlyph(x, y - 5, color);
            break;
        case 2:
            drawApproveGlyph(x, y - 5, color);
            break;
        case 3:
            drawRejectGlyph(x, y - 5, color);
            break;
        default:
            drawPlanGlyph(x, y - 5, color, g_planModeEnabled);
            break;
    }
}

void drawStatusBar() {
    const auto panel = M5.Display.color565(21, 24, 31);
    const auto stateColor = g_connected ? M5.Display.color565(66, 232, 139)
                                        : M5.Display.color565(255, 174, 54);
    M5.Display.fillRoundRect(151, 426, 164, 26, 13, panel);
    drawThickRoundRect(151, 426, 164, 26, 13, 2, stateColor);

    char status[40];
    if (g_restartAt != 0) {
        std::snprintf(status, sizeof(status), "RESTART  #%d", g_deviceSlot);
    } else {
        const char* link = g_connected ? "ON" : (net::wifiUp() ? "HOST" : "WIFI");
        if (voice::isCapturing()) {
            link = "REC";
        } else if (voice::isSpeaking()) {
            link = "SAY";
        }
        std::snprintf(status, sizeof(status), "%s  #%d  %u%%%s", link,
                      g_deviceSlot, g_batteryLevel, g_isCharging ? "+" : "");
    }
    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(0.75f);
    M5.Display.setTextColor(TFT_WHITE, panel);
    M5.Display.drawString(status, kScreenCenter, 439);
}

void renderSettingsUi() {
    const auto panel = M5.Display.color565(27, 30, 38);
    const auto panelBorder = M5.Display.color565(118, 124, 142);
    const auto purple = M5.Display.color565(145, 120, 255);
    const auto muted = M5.Display.color565(205, 210, 222);

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_32);
    M5.Display.setTextSize(0.82f);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("SETTINGS", 218, 38);
    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(0.78f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    M5.Display.drawString("WATCH ID", kScreenCenter, 72);

    M5.Display.fillCircle(kSettingsCloseX, kSettingsCloseY, kSettingsCloseRadius, panel);
    drawThickCircle(kSettingsCloseX, kSettingsCloseY, kSettingsCloseRadius, 3, panelBorder);
    M5.Display.drawWideLine(kSettingsCloseX - 10, kSettingsCloseY - 10, kSettingsCloseX + 10,
                            kSettingsCloseY + 10, 2.5f, TFT_WHITE);
    M5.Display.drawWideLine(kSettingsCloseX + 10, kSettingsCloseY - 10, kSettingsCloseX - 10,
                            kSettingsCloseY + 10, 2.5f, TFT_WHITE);

    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(1);
    for (int i = 0; i < 3; ++i) {
        const int slot = i + 1;
        const int x = 122 + i * 111;
        const bool selected = slot == g_pendingDeviceSlot;
        M5.Display.fillCircle(x, 112, 30, selected ? purple : panel);
        drawThickCircle(x, 112, 30, selected ? 4 : 2, selected ? TFT_WHITE : panelBorder);
        M5.Display.setTextColor(TFT_WHITE, selected ? purple : panel);
        char label[4];
        std::snprintf(label, sizeof(label), "#%d", slot);
        M5.Display.drawString(label, x, 112);
    }

    const bool pairPressed = g_activeTouch == kTouchPair;
    const auto pairFill = pairPressed ? M5.Display.color565(103, 81, 220) : purple;
    M5.Display.fillRoundRect(153, 151, 160, 45, 22, pairFill);
    drawThickRoundRect(153, 151, 160, 45, 22, pairPressed ? 5 : 3,
                       pairPressed ? TFT_WHITE : panelBorder);
    M5.Display.setTextColor(TFT_WHITE, pairFill);
    M5.Display.drawString("CONNECT", kScreenCenter, 173);

    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(0.82f);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    char volumeLabel[24];
    std::snprintf(volumeLabel, sizeof(volumeLabel), "SE VOLUME  %u%%",
                  static_cast<unsigned>(g_seVolume) * 100 / 255);
    M5.Display.drawString(volumeLabel, kScreenCenter, 216);

    const int volumeX = kSettingsSliderLeft + static_cast<int>(g_seVolume) *
                        (kSettingsSliderRight - kSettingsSliderLeft) / 255;
    M5.Display.drawWideLine(kSettingsSliderLeft, 242, kSettingsSliderRight, 242,
                            6.0f, panelBorder);
    if (volumeX > kSettingsSliderLeft) {
        M5.Display.drawWideLine(kSettingsSliderLeft, 242, volumeX, 242, 6.0f, purple);
    }
    M5.Display.fillCircle(volumeX, 242, 12, TFT_WHITE);
    drawThickCircle(volumeX, 242, 12, 2, purple);

    char vibrationLabel[32];
    std::snprintf(vibrationLabel, sizeof(vibrationLabel), "VIBE STRENGTH  %u%%",
                  static_cast<unsigned>(g_vibrationStrength) * 100 / 255);
    M5.Display.drawString(vibrationLabel, kScreenCenter, 278);

    const int vibrationX = kSettingsSliderLeft + static_cast<int>(g_vibrationStrength) *
                           (kSettingsSliderRight - kSettingsSliderLeft) / 255;
    M5.Display.drawWideLine(kSettingsSliderLeft, 304, kSettingsSliderRight, 304,
                            6.0f, panelBorder);
    if (vibrationX > kSettingsSliderLeft) {
        M5.Display.drawWideLine(kSettingsSliderLeft, 304, vibrationX, 304, 6.0f, purple);
    }
    M5.Display.fillCircle(vibrationX, 304, 12, TFT_WHITE);
    drawThickCircle(vibrationX, 304, 12, 2, purple);

    M5.Display.setTextSize(0.72f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    M5.Display.drawString("AGENT 1-6 STATE CHANGE", kScreenCenter, 341);

    const auto drawCheckbox = [&](int x, const char* label, bool checked) {
        const auto fill = checked ? purple : panel;
        M5.Display.fillRoundRect(x, 367, 28, 28, 6, fill);
        drawThickRoundRect(x, 367, 28, 28, 6, 2, checked ? TFT_WHITE : panelBorder);
        if (checked) {
            M5.Display.drawWideLine(x + 6, 381, x + 12, 387, 3.0f, TFT_WHITE);
            M5.Display.drawWideLine(x + 12, 387, x + 23, 375, 3.0f, TFT_WHITE);
        }
        M5.Display.setTextDatum(middle_left);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawString(label, x + 38, 381);
        M5.Display.setTextDatum(middle_center);
    };
    drawCheckbox(178, "VIBE", g_agentStateVibeEnabled);

    drawStatusBar();
}

// -----------------------------------------------------------------------------
// Approval pop-up: layout adapted from neilshare/vibewatch drawApprovalOverlay()
// (MIT), reworked as a full-screen card for the round display.
// -----------------------------------------------------------------------------

constexpr int kApprovalButtonY = 326;
constexpr int kApprovalButtonH = 54;
constexpr int kApprovalButtonW = 150;
constexpr int kApprovalNgX = 70;
constexpr int kApprovalOkX = 246;

// Draws up to maxLines of wrapped text centred on the screen; returns lines used.
int drawWrapped(const char* text, int y, int lineHeight, std::size_t maxBytes, int maxLines) {
    char line[64];
    int lines = 0;
    while (text != nullptr && *text != '\0' && lines < maxLines) {
        while (*text == ' ') {
            ++text;
        }
        std::size_t n = vibe::utf8WrapIndex(text, std::min(maxBytes, sizeof(line) - 1));
        if (n == 0) {
            break;
        }
        const bool last = lines == maxLines - 1 && text[n] != '\0';
        std::memcpy(line, text, n);
        line[n] = '\0';
        if (last && n >= 3) {
            std::strcpy(line + vibe::utf8WrapIndex(line, n - 3), "...");
        }
        M5.Display.drawString(line, kScreenCenter, y + lines * lineHeight);
        text += n;
        ++lines;
    }
    return lines;
}

void drawApprovalOverlay(std::uint32_t now) {
    const vibe::ApprovalRequest* request = g_approvals.current();
    if (request == nullptr) {
        return;
    }
    const auto amber = M5.Display.color565(255, 172, 54);
    const auto greenOk = M5.Display.color565(43, 201, 110);
    const auto redNg = M5.Display.color565(245, 90, 104);
    const auto muted = M5.Display.color565(180, 188, 205);
    const bool armed = now - g_approvalShownAt >= kApprovalArmMs;

    // Pulsing amber bezel so the state is readable at a glance.
    const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(now % 1400) / 1400.0f * 2.0f * PI);
    drawThickCircle(kScreenCenter, kScreenCenter, 228, 6, scaledColor(0xFFAC28, pulse));

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(0.8f);
    M5.Display.setTextColor(amber, TFT_BLACK);
    M5.Display.drawString("APPROVAL", kScreenCenter, 66);

    char header[40];
    std::snprintf(header, sizeof(header), "AGENT %d  [ %s ]", request->slot + 1, request->kind);
    M5.Display.setTextSize(0.55f);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(header, kScreenCenter, 104);

    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    const int titleLines = drawWrapped(
        request->title[0] != '\0' ? request->title : "Run this command?", 146, 24, 24, 2);

    M5.Display.setTextSize(0.82f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    drawWrapped(request->detail, 146 + titleLines * 24 + 14, 21, 28, 4 - titleLines + 1);

    char countdown[16];
    std::snprintf(countdown, sizeof(countdown), "%lus",
                  static_cast<unsigned long>((g_approvals.remainingMs(now) + 999) / 1000));
    M5.Display.setTextColor(amber, TFT_BLACK);
    M5.Display.drawString(countdown, kScreenCenter, 300);

    const auto drawButton = [&](int x, std::uint16_t fill, const char* label) {
        const auto face = armed ? fill : scaledColor(0x3A3F4C, 1.0f);
        M5.Display.fillRoundRect(x, kApprovalButtonY, kApprovalButtonW, kApprovalButtonH, 16, face);
        drawThickRoundRect(x, kApprovalButtonY, kApprovalButtonW, kApprovalButtonH, 16, 2, TFT_WHITE);
        M5.Display.setFont(&fonts::Orbitron_Light_24);
        M5.Display.setTextSize(0.62f);
        M5.Display.setTextColor(TFT_WHITE, face);
        M5.Display.drawString(label, x + kApprovalButtonW / 2, kApprovalButtonY + kApprovalButtonH / 2);
    };
    // Left physical button (orange) = NG, right (blue) = OK, as elsewhere.
    drawButton(kApprovalNgX, redNg, "NG");
    drawButton(kApprovalOkX, greenOk, "OK");

    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(0.7f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    M5.Display.drawString("L button          R button", kScreenCenter, 400);
}

void resetInputStateForModal() {
    // Anything half-pressed when the pop-up appeared must not complete later
    // as an agent/OK/NG action on the normal layer.
    g_activeTouch = -1;
    g_leftAgentPressed = -1;
    g_leftPressPending = false;
    g_rightActionPending = false;
    g_rightActionPressed = false;
    g_rightPhysicalPressedAt = 0;
    g_buttonChordActive = false;
    g_inputLockUntilRelease = true;
}

void answerApproval(vibe::ApprovalChoice choice) {
    const auto decision = g_approvals.decide(choice, millis());
    if (!decision.hasValue) {
        return;
    }
    sendApprovalDecision(decision.value);
    if (choice == vibe::ApprovalChoice::Approve) {
        playOuterActionPressSe(kOkAction);
        vibrate(180, 50);
    } else {
        playOuterActionPressSe(kNgAction);
        vibrate(120, 35);
    }
    g_inputLockUntilRelease = true;
    hideApproval();
}

// Returns true when the approval pop-up (or its release lock) owns input this
// frame, so the normal touch/button handlers must not run.
bool handleApprovalInput() {
    const auto touch = M5.Touch.getDetail();
    if (g_inputLockUntilRelease) {
        if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed() && !touch.isPressed()) {
            g_inputLockUntilRelease = false;
        }
        return true;
    }
    if (g_approvalShownAt == 0) {
        return false;
    }
    if (millis() - g_approvalShownAt < kApprovalArmMs) {
        return true;  // arming delay: a stray press can't approve
    }
    if (M5.BtnA.wasPressed()) {
        answerApproval(vibe::ApprovalChoice::Reject);
    } else if (M5.BtnB.wasPressed()) {
        answerApproval(vibe::ApprovalChoice::Approve);
    } else if (touch.wasPressed() && touch.y >= kApprovalButtonY - 12 &&
               touch.y <= kApprovalButtonY + kApprovalButtonH + 12) {
        if (touch.x >= kApprovalNgX && touch.x < kApprovalNgX + kApprovalButtonW) {
            answerApproval(vibe::ApprovalChoice::Reject);
        } else if (touch.x >= kApprovalOkX && touch.x < kApprovalOkX + kApprovalButtonW) {
            answerApproval(vibe::ApprovalChoice::Approve);
        }
    }
    return true;
}

void approvalLoop(std::uint32_t now) {
    const auto expired = g_approvals.expireIfNeeded(now);
    if (expired.hasValue) {
        sendApprovalDecision(expired.value);  // silence is not consent
        playOuterActionPressSe(kNgAction);
        hideApproval();
        return;
    }
    const vibe::ApprovalRequest* request = g_approvals.current();
    if (request == nullptr) {
        return;
    }
    if (g_approvalShownAt == 0) {
        // Never interrupt someone mid-sentence; show once push-to-talk ends.
        if (voice::isCapturing()) {
            return;
        }
        g_settingsOpen = false;
        g_statusOpen = false;
        g_actionLayer = false;
        selectAgent(request->slot);
        resetInputStateForModal();
        g_inputLockUntilRelease = M5.BtnA.isPressed() || M5.BtnB.isPressed() ||
                                  M5.Touch.getDetail().isPressed();
        g_approvalShownAt = now == 0 ? 1 : now;
        g_approvalLastReminder = now;
        vibrate(250, 90);
        playSe(1250.0f, 75);
        g_uiDirty = true;
        return;
    }
    if (now - g_approvalLastReminder >= kApprovalReminderMs) {
        g_approvalLastReminder = now;
        vibrate(200, 60);
    }
    const std::uint32_t second = g_approvals.remainingMs(now) / 1000;
    if (second != g_approvalLastSecond) {
        g_approvalLastSecond = second;
        g_uiDirty = true;
    }
}

// -----------------------------------------------------------------------------
// Power saving: Active (240 MHz, full brightness) -> Dimmed (160 MHz, dim) ->
// Asleep (80 MHz, panel off, Wi-Fi modem sleep). 80 MHz is the lowest clock
// the ESP32-S3 Wi-Fi driver supports.
// -----------------------------------------------------------------------------

void applyPowerState(vibe::PowerState next) {
    if (next == g_appliedPower) {
        return;
    }
    const bool wasAsleep = g_appliedPower == vibe::PowerState::Asleep;
    switch (next) {
        case vibe::PowerState::Active:
            setCpuFrequencyMhz(240);
            if (wasAsleep) {
                M5.Display.wakeup();
                net::setPowerSave(false);
            }
            M5.Display.setBrightness(kActiveBrightness);
            break;
        case vibe::PowerState::Dimmed:
            setCpuFrequencyMhz(160);
            if (wasAsleep) {
                M5.Display.wakeup();
                net::setPowerSave(false);
            }
            M5.Display.setBrightness(kDimBrightness);
            break;
        case vibe::PowerState::Asleep:
            M5.Display.setBrightness(0);
            M5.Display.sleep();
            setCpuFrequencyMhz(80);
            net::setPowerSave(true);
            break;
    }
    Serial.printf("Power: %s -> %s\n", vibe::powerStateName(g_appliedPower),
                  vibe::powerStateName(next));
    g_appliedPower = next;
    g_uiDirty = true;  // repaint whatever changed while the panel was off
}

void updatePower() {
    const std::uint32_t now = millis();
    const auto touch = M5.Touch.getDetail();
    const bool userInput = touch.isPressed() || touch.wasPressed() || touch.wasReleased() ||
                           M5.BtnA.isPressed() || M5.BtnB.isPressed() ||
                           M5.BtnA.wasReleased() || M5.BtnB.wasReleased();
    if (userInput && g_power.noteUserActivity(now)) {
        // The tap/press that wakes a dark screen must not also trigger an
        // agent, OK/NG, or push-to-talk the user couldn't see.
        g_inputLockUntilRelease = true;
        vibrate(60, 15);
    }
    // Keep the screen on while something needs it: recording, a spoken reply,
    // or an approval (which also wakes a sleeping watch).
    if (voice::isCapturing() || voice::isSpeaking() || g_approvals.pending() ||
        g_settingsOpen || g_restartAt != 0) {
        g_power.noteSystemActivity(now);
    }
    applyPowerState(g_power.update(now));
}

// -----------------------------------------------------------------------------
// Hermes status: gauge ring on the agent layer, full page from the status pill
// -----------------------------------------------------------------------------

std::uint16_t healthColor(vibe::StatusFreshness freshness) {
    if (freshness != vibe::StatusFreshness::Fresh) {
        return M5.Display.color565(96, 102, 116);  // grey: no trustworthy data
    }
    switch (g_hostStatus.health) {
        case vibe::HostHealth::Ok: return M5.Display.color565(66, 232, 139);
        case vibe::HostHealth::Degraded: return M5.Display.color565(255, 172, 54);
        case vibe::HostHealth::Offline: return M5.Display.color565(245, 90, 104);
        default: return M5.Display.color565(96, 102, 116);
    }
}

// Arc helper that copes with sweeps crossing 0 degrees.
void fillGaugeArc(int outer, int inner, float fromDeg, float sweepDeg, std::uint16_t color) {
    if (sweepDeg <= 0.5f) return;
    const float end = fromDeg + sweepDeg;
    if (end <= 360.0f) {
        M5.Display.fillArc(kScreenCenter, kScreenCenter, outer, inner, fromDeg, end, color);
    } else {
        M5.Display.fillArc(kScreenCenter, kScreenCenter, outer, inner, fromDeg, 360.0f, color);
        M5.Display.fillArc(kScreenCenter, kScreenCenter, outer, inner, 0.0f, end - 360.0f, color);
    }
}

void drawGauge(int outer, int inner, std::uint32_t now) {
    const auto freshness = g_hostStatus.freshness(now);
    fillGaugeArc(outer, inner, kGaugeStartDeg, kGaugeSweepDeg, M5.Display.color565(34, 38, 48));
    if (freshness == vibe::StatusFreshness::Unavailable) return;
    const float fraction = g_hostStatus.hasBudget() ? g_hostStatus.remainingPercent / 100.0f : 1.0f;
    fillGaugeArc(outer, inner, kGaugeStartDeg, kGaugeSweepDeg * fraction, healthColor(freshness));
}

void drawStatusRing(std::uint32_t now) {
    drawGauge(kStatusRingOuter, kStatusRingInner, now);
}

void renderStatusPage(std::uint32_t now) {
    const auto freshness = g_hostStatus.freshness(now);
    const auto muted = M5.Display.color565(180, 188, 205);
    const auto accent = healthColor(freshness);
    drawGauge(226, 208, now);

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(0.8f);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("HERMES", kScreenCenter, 70);

    if (freshness == vibe::StatusFreshness::Unavailable) {
        M5.Display.setFont(&fonts::DejaVu18);
        M5.Display.setTextSize(1.0f);
        M5.Display.setTextColor(muted, TFT_BLACK);
        M5.Display.drawString(g_connected ? "waiting for bridge..." : "not connected",
                              kScreenCenter, kScreenCenter);
        M5.Display.setTextSize(0.7f);
        M5.Display.drawString("tap to close", kScreenCenter, 400);
        return;
    }

    M5.Display.setTextSize(0.6f);
    M5.Display.setTextColor(accent, TFT_BLACK);
    M5.Display.drawString(freshness == vibe::StatusFreshness::Stale
                              ? "STALE" : vibe::healthName(g_hostStatus.health),
                          kScreenCenter, 104);

    char big[16];
    vibe::formatTokens(g_hostStatus.tokensToday, big, sizeof(big));
    M5.Display.setFont(&fonts::Orbitron_Light_32);
    M5.Display.setTextSize(1.3f);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(big, kScreenCenter, 160);

    char line[48];
    M5.Display.setFont(&fonts::DejaVu18);
    M5.Display.setTextSize(0.85f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    if (g_hostStatus.hasBudget()) {
        char budget[16];
        vibe::formatTokens(g_hostStatus.budget, budget, sizeof(budget));
        std::snprintf(line, sizeof(line), "tokens today  %d%% of %s left",
                      static_cast<int>(g_hostStatus.remainingPercent + 0.5f), budget);
    } else {
        std::snprintf(line, sizeof(line), "tokens today");
    }
    M5.Display.drawString(line, kScreenCenter, 204);

    char reset[16];
    vibe::formatDuration(g_hostStatus.resetRemainingSeconds(now), reset, sizeof(reset));
    std::snprintf(line, sizeof(line), "resets in %s", reset);
    M5.Display.drawString(line, kScreenCenter, 230);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    std::snprintf(line, sizeof(line), "runs %u    jobs %u",
                  static_cast<unsigned>(g_hostStatus.runs), static_cast<unsigned>(g_hostStatus.jobs));
    M5.Display.drawString(line, kScreenCenter, 266);

    // Per-agent session tokens, two rows of three.
    M5.Display.setTextSize(0.72f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    for (int row = 0; row < 2; ++row) {
        char rowText[64] = {};
        for (int col = 0; col < 3; ++col) {
            const int i = row * 3 + col;
            char t[12];
            if (g_hostStatus.slotTokens[i] == 0) {
                std::snprintf(t, sizeof(t), "-");
            } else {
                vibe::formatTokens(g_hostStatus.slotTokens[i], t, sizeof(t));
            }
            char cell[20];
            std::snprintf(cell, sizeof(cell), "%s%d:%s", col == 0 ? "" : "   ", i + 1, t);
            std::strncat(rowText, cell, sizeof(rowText) - std::strlen(rowText) - 1);
        }
        M5.Display.drawString(rowText, kScreenCenter, 302 + row * 24);
    }

    char age[16];
    vibe::formatDuration(g_hostStatus.ageSeconds(now), age, sizeof(age));
    std::snprintf(line, sizeof(line), "%s %s ago",
                  freshness == vibe::StatusFreshness::Stale ? "STALE, updated" : "updated", age);
    M5.Display.setTextColor(freshness == vibe::StatusFreshness::Stale
                                ? M5.Display.color565(255, 172, 54) : muted, TFT_BLACK);
    M5.Display.drawString(line, kScreenCenter, 368);
    M5.Display.setTextSize(0.7f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    M5.Display.drawString("tap to close", kScreenCenter, 400);
}

// Consumes input while the status page is open; any tap or button closes it.
bool handleStatusInput() {
    if (!g_statusOpen) {
        return false;
    }
    const auto touch = M5.Touch.getDetail();
    if (touch.wasPressed() || M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
        g_statusOpen = false;
        g_inputLockUntilRelease = true;
        playSe(540.0f);
        vibrate(60, 15);
        g_uiDirty = true;
    }
    return true;
}

void statusLoop(std::uint32_t now) {
    const auto freshness = g_hostStatus.freshness(now);
    if (freshness != g_lastStatusFreshness) {
        g_lastStatusFreshness = freshness;
        g_uiDirty = true;  // ring turns grey the moment data goes stale
    }
    if (g_statusOpen) {
        const std::uint32_t second = now / 1000;
        if (second != g_lastStatusSecond) {
            g_lastStatusSecond = second;
            g_uiDirty = true;  // countdown and "updated Xs ago"
        }
    }
}

void renderUi(std::uint32_t now) {
    // Redraw the small round display as one frame. The UI is simple enough that
    // full-frame painting avoids stale pixels when switching between layers.
    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);

    if (g_approvalShownAt != 0) {
        drawApprovalOverlay(now);
        M5.Display.endWrite();
        g_uiDirty = false;
        g_lastUiDraw = now;
        return;
    }

    if (g_statusOpen) {
        renderStatusPage(now);
        M5.Display.endWrite();
        g_uiDirty = false;
        g_lastUiDraw = now;
        return;
    }

    if (g_settingsOpen) {
        renderSettingsUi();
        M5.Display.endWrite();
        g_uiDirty = false;
        g_lastUiDraw = now;
        return;
    }

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_32);
    M5.Display.setTextSize(1);
    if (!g_actionLayer) {
        drawStatusRing(now);
    }
    const int outerCount = g_actionLayer ? kActionCount : kAgentCount;
    if (g_actionLayer) {
        drawPhysicalActionLinks();
    }
    for (int i = 0; i < outerCount; ++i) {
        const int outerX = g_actionLayer ? actionX[i] : agentX[i];
        const int outerY = g_actionLayer ? actionY[i] : agentY[i];
        std::uint16_t fill = M5.Display.color565(17, 22, 28);
        std::uint16_t accent = M5.Display.color565(105, 114, 132);
        bool selected = false;
        if (g_actionLayer) {
            static constexpr std::uint32_t kActionColors[kActionCount] = {
                0x9D74FF, kRightPhysicalButtonColor, kLeftPhysicalButtonColor,
                0x33C4E8, 0xE5E8EF,
            };
            accent = scaledColor(kActionColors[i], 1.0f);
            selected = i == 3 ? g_planModeEnabled : g_selectedAction == i;
        } else {
            const auto& state = g_agents[i];
            const float brightness = effectBrightness(state.effect, state.brightness, state.speed, now);
            fill = state.effect == 0 || state.color == 0
                       ? M5.Display.color565(17, 22, 28)
                       : scaledColor(state.color, brightness);
            // Agent selection is rendered by one independently animated ring
            // after all six circles have been painted.
            selected = false;
            accent = M5.Display.color565(163, 132, 255);
        }
        const bool pressed = g_activeTouch == i || g_leftAgentPressed == i ||
                             (g_actionLayer && g_rightActionPressed && i == kOkAction);
        if (g_actionLayer && pressed) {
            fill = accent;
        }
        M5.Display.fillCircle(outerX, outerY, kAgentButtonRadius, fill);
        if (selected) {
            // A three-stage edge stays visible against both dark and bright
            // agent colors: outer glow, saturated edge, then white keyline.
            drawThickCircle(outerX, outerY, kAgentButtonRadius + 4, 3,
                            g_actionLayer ? scaledColor(0x34303F, 1.0f)
                                          : M5.Display.color565(74, 56, 128));
        }
        const auto borderColor = pressed ? TFT_WHITE
                                         : selected ? accent
                                                    : g_actionLayer ? accent
                                                                    : M5.Display.color565(105, 114, 132);
        drawThickCircle(outerX, outerY, kAgentButtonRadius, pressed ? 7 : selected ? 7 : 3,
                        borderColor);
        if (pressed) {
            drawThickCircle(outerX, outerY, kAgentButtonRadius - 8, 2, TFT_WHITE);
        } else if (selected) {
            drawThickCircle(outerX, outerY, kAgentButtonRadius - 9, 2, TFT_WHITE);
        }
        if (g_actionLayer) {
            const int glyphY = outerY - 10;
            if (i == 0) {
                drawFastGlyph(outerX, glyphY, TFT_WHITE);
            } else if (i == 1) {
                drawApproveGlyph(outerX, glyphY, TFT_WHITE);
            } else if (i == 2) {
                drawRejectGlyph(outerX, glyphY, TFT_WHITE);
            } else if (i == 3) {
                drawPlanGlyph(outerX, glyphY, TFT_WHITE, g_planModeEnabled);
            } else {
                drawAssistantGlyph(outerX, glyphY, TFT_WHITE);
            }
            static constexpr const char* kOuterActionLabels[kActionCount] = {
                "FAST", "OK", "NG", "PLAN", "AI",
            };
            M5.Display.setFont(&fonts::Orbitron_Light_24);
            M5.Display.setTextSize(0.62f);
            M5.Display.setTextColor(TFT_WHITE);
            M5.Display.drawString(kOuterActionLabels[i], outerX,
                                  outerY + kAgentButtonRadius + 13);
            M5.Display.setFont(&fonts::Orbitron_Light_32);
            M5.Display.setTextSize(1);
        } else {
            const int fillRed = ((fill >> 11) & 0x1F) * 255 / 31;
            const int fillGreen = ((fill >> 5) & 0x3F) * 255 / 63;
            const int fillBlue = (fill & 0x1F) * 255 / 31;
            const int fillLuminance = (fillRed * 299 + fillGreen * 587 + fillBlue * 114) / 1000;
            const auto labelColor = fillLuminance >= 150 ? TFT_BLACK : TFT_WHITE;
            M5.Display.setTextColor(labelColor);
            char label[2];
            std::snprintf(label, sizeof(label), "%d", i + 1);
            // Slight horizontal overdraw gives the angular Orbitron glyphs
            // more weight without losing their technical character.
            M5.Display.drawString(label, outerX - 1, outerY);
            M5.Display.drawString(label, outerX + 1, outerY);
            M5.Display.drawString(label, outerX, outerY);
        }
    }

    if (!g_actionLayer) {
        drawSelectionIndicator(now);
    }

    const bool micPressed = g_rightLongTriggered || g_activeTouch == kTouchMic;
    const auto micAccent = M5.Display.color565(48, 79, 254);
    const auto micFill = micPressed ? micAccent : M5.Display.color565(25, 31, 40);
    M5.Display.fillCircle(kScreenCenter, kScreenCenter, kMicButtonRadius, micFill);
    drawThickCircle(kScreenCenter, kScreenCenter, kMicButtonRadius, micPressed ? 7 : 4,
                    micPressed ? TFT_WHITE : micAccent);
    drawLargeMicGlyph(kScreenCenter, kScreenCenter - 2, TFT_WHITE);

    if (!g_actionLayer) {
        const auto settingsFill = M5.Display.color565(30, 32, 40);
        M5.Display.fillCircle(kSettingsX, kSettingsY, kSettingsVisualRadius, settingsFill);
        drawThickCircle(kSettingsX, kSettingsY, kSettingsVisualRadius, 3,
                        M5.Display.color565(120, 126, 143));
        drawSettingsGlyph(kSettingsX, kSettingsY, TFT_WHITE);
    }

    if (!g_actionLayer) {
        drawStatusBar();
    }

    M5.Display.endWrite();
    g_uiDirty = false;
    g_lastUiDraw = now;
}

// Draw one frame of the boot animation. The six orbiting dots preview the
// Agent-layer layout before the full interface appears.
void drawSplashFrame(float progress) {
    const float eased = 1.0f - std::pow(1.0f - clamp01(progress), 3.0f);
    const float rawFade = clamp01(progress / 0.68f);
    const float textFade = rawFade * rawFade * (3.0f - 2.0f * rawFade);
    const float pulse = 0.78f + 0.22f * std::sin(progress * PI * 4.0f);
    const auto purple = scaledColor(0x9D74FF, textFade);
    const auto cyan = scaledColor(0x33C4E8, textFade);
    const auto muted = scaledColor(0xAAB4C8, textFade * 0.85f);

    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);

    // Expand the primary logo ring from the center.
    const int ringRadius = 72 + static_cast<int>(56.0f * eased);
    drawThickCircle(kScreenCenter, kScreenCenter, ringRadius + 8, 2,
                    scaledColor(0x34284F, textFade));
    drawThickCircle(kScreenCenter, kScreenCenter, ringRadius, 4,
                    scaledColor(0x9D74FF, textFade * pulse));
    // Reveal six status dots clockwise, mirroring the main Agent layer.
    for (int i = 0; i < kAgentCount; ++i) {
        const float revealAt = 0.10f + i * 0.075f;
        if (progress < revealAt) {
            continue;
        }
        const float dotFade = clamp01((progress - revealAt) / 0.18f);
        const float angle = (-120.0f + i * 60.0f) * PI / 180.0f;
        const int x = kScreenCenter + static_cast<int>(std::cos(angle) * 166.0f);
        const int y = kScreenCenter + static_cast<int>(std::sin(angle) * 166.0f);
        M5.Display.fillCircle(x, y, 5 + static_cast<int>(3.0f * dotFade),
                              scaledColor(i % 2 == 0 ? 0x9D74FF : 0x33C4E8, dotFade));
    }

    // The Orbitron face matches the technical visual language of the main UI.
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Orbitron_Light_32);
    M5.Display.setTextSize(1.24f);
    M5.Display.setTextColor(purple, TFT_BLACK);
    M5.Display.drawString("VIBEWATCH", kScreenCenter, 202);

    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(0.62f);
    M5.Display.setTextColor(cyan, TFT_BLACK);
    M5.Display.drawString("AI CONTROL SURFACE", kScreenCenter, 252);

    M5.Display.setTextSize(0.68f);
    M5.Display.setTextColor(muted, TFT_BLACK);
    M5.Display.drawString(vibe::kFirmwareVersion, kScreenCenter, 291);

    // Animate from zero to the measured charge so the battery readout also
    // acts as a compact startup progress indicator.
    const int animatedBattery = static_cast<int>(
        std::lround(static_cast<float>(g_batteryLevel) * eased));
    const std::uint32_t batteryPacked = g_batteryLevel <= 15
        ? 0xF55367
        : g_batteryLevel <= 35 ? 0xFFAC28 : 0x33C4E8;
    const auto batteryColor = scaledColor(batteryPacked, textFade);
    const auto batteryTrack = scaledColor(0x566071, textFade * 0.55f);
    constexpr int kBatteryBarX = 143;
    constexpr int kBatteryBarY = 361;
    constexpr int kBatteryBarWidth = 180;
    constexpr int kBatteryBarHeight = 10;
    const int batteryFillWidth = kBatteryBarWidth * animatedBattery / 100;

    char batteryLabel[24];
    std::snprintf(batteryLabel, sizeof(batteryLabel), "BATTERY  %d%%", animatedBattery);
    M5.Display.setTextSize(0.58f);
    M5.Display.setTextColor(batteryColor, TFT_BLACK);
    M5.Display.drawString(batteryLabel, kScreenCenter, 338);
    M5.Display.fillRoundRect(kBatteryBarX, kBatteryBarY, kBatteryBarWidth,
                             kBatteryBarHeight, kBatteryBarHeight / 2, batteryTrack);
    if (batteryFillWidth > 0) {
        M5.Display.fillRoundRect(kBatteryBarX, kBatteryBarY, batteryFillWidth,
                                 kBatteryBarHeight, kBatteryBarHeight / 2, batteryColor);
    }

    // A small center mark gives the logo a watch-dial focal point.
    M5.Display.fillCircle(kScreenCenter, 151, 4, cyan);
    M5.Display.drawWideLine(kScreenCenter - 13, 151, kScreenCenter - 6, 151, 2.0f, purple);
    M5.Display.drawWideLine(kScreenCenter + 6, 151, kScreenCenter + 13, 151, 2.0f, purple);

    M5.Display.endWrite();
}

void showSplashScreen() {
    constexpr std::uint32_t kSplashAnimationMs = 1800;
    constexpr std::uint32_t kSplashHoldMs = 1100;
    struct ChiptuneNote {
        std::uint32_t atMs;
        float frequency;
        std::uint32_t durationMs;
    };
    // Original NES-style pulse-wave arpeggio; no existing game melody is used.
    static constexpr ChiptuneNote kStartupJingle[] = {
        {70, 293.66f, 70},   // D4
        {160, 440.00f, 70},  // A4
        {250, 587.33f, 80},  // D5
        {360, 698.46f, 85},  // F5
        {480, 880.00f, 95},  // A5
        {610, 698.46f, 65},  // F5
        {700, 880.00f, 75},  // A5
        {800, 1174.66f, 240},  // D6
    };
    constexpr std::size_t kJingleNoteCount =
        sizeof(kStartupJingle) / sizeof(kStartupJingle[0]);

    drawSplashFrame(0.0f);
    const std::uint32_t startedAt = millis();
    std::size_t nextNote = 0;

    while (millis() - startedAt < kSplashAnimationMs) {
        const std::uint32_t elapsed = millis() - startedAt;
        while (nextNote < kJingleNoteCount &&
               elapsed >= kStartupJingle[nextNote].atMs) {
            const auto& note = kStartupJingle[nextNote];
            playSe(note.frequency, note.durationMs);
            ++nextNote;
        }
        const float progress = static_cast<float>(elapsed) /
                               static_cast<float>(kSplashAnimationMs);
        drawSplashFrame(progress);
        delay(24);
    }
    drawSplashFrame(1.0f);
    delay(kSplashHoldMs);
}

}  // namespace

// -----------------------------------------------------------------------------
// Arduino lifecycle
// -----------------------------------------------------------------------------

void setup() {
    // Initialize hardware and local state before joining Wi-Fi. This ensures
    // the first screen and battery report are valid when the bridge connects.
    Serial.begin(115200);
    delay(200);

    auto config = M5.config();
    config.clear_display = true;
    config.internal_spk = true;
    config.internal_mic = true;
    M5.begin(config);
    M5.Display.setBrightness(kActiveBrightness);
    M5.Display.setRotation(0);

    loadPreferences();
    M5.Speaker.setVolume(g_seVolume);
    updateBattery(false);
    showSplashScreen();

    initializeAgentPositions();
    g_rpcQueue = xQueueCreate(6, sizeof(char*));
    if (g_rpcQueue == nullptr) {
        Serial.println("Failed to create RPC queue");
        while (true) {
            delay(1000);
        }
    }

    renderUi(millis());
    initializeNetwork();
    g_uiDirty = true;
}

void loop() {
    // Keep input, host messages, deferred restart, haptics, battery updates, and
    // rendering cooperative; no path should block long enough to starve Wi-Fi.
    M5.update();
    net::loop();

    // Mirror link state into the UI and reuse the pairing chime on connect.
    const bool linkUp = net::connected();
    if (linkUp != g_connected) {
        g_connected = linkUp;
        g_uiDirty = true;
        if (linkUp) {
            g_pairingSuccessPending = true;
        } else {
            g_activeTouch = -1;
            // The bridge keeps the request and re-sends it on reconnect.
            g_approvals.cancel(millis());
            hideApproval();
        }
    }

    updatePower();
    if (!handleApprovalInput() && !handleStatusInput()) {
        handleTouch();
        handlePhysicalButtons();
    }
    voice::loop();
    approvalLoop(millis());
    statusLoop(millis());
    static int lastVoiceState = 0;
    const int voiceState = voice::isCapturing() ? 1 : (voice::isSpeaking() ? 2 : 0);
    if (voiceState != lastVoiceState) {
        lastVoiceState = voiceState;
        g_uiDirty = true;
    }

    char* message = nullptr;
    while (xQueueReceive(g_rpcQueue, &message, 0) == pdTRUE) {
        processRpc(message);
        std::free(message);
        message = nullptr;
    }

    if (g_pairingSuccessPending) {
        g_pairingSuccessPending = false;
        playSe(1320.0f, 95);
        vibrate(220, 75);
    }

    const std::uint32_t now = millis();
    if (g_restartAt != 0 && static_cast<std::int32_t>(now - g_restartAt) >= 0) {
        ESP.restart();
    }
    if (g_vibrationOffAt != 0 && static_cast<std::int32_t>(now - g_vibrationOffAt) >= 0) {
        M5.Power.setVibration(0);
        g_vibrationOffAt = 0;
    }
    if (now - g_lastBatteryUpdate >= kBatteryUpdatePeriodMs) {
        updateBattery(true);
    }
    const std::uint32_t uiPeriod = g_selectionAnimating ? kSelectionAnimationPeriodMs
                                                        : kUiAnimationPeriodMs;
    const bool screenOn = g_appliedPower != vibe::PowerState::Asleep;
    if (screenOn && (g_uiDirty || uiIsAnimated()) && now - g_lastUiDraw >= uiPeriod) {
        renderUi(now);
    }

    // Slower polling while the screen is off still catches a tap or press.
    delay(screenOn ? 5 : 20);
}
