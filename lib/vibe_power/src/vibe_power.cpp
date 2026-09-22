#include "vibe_power.h"

namespace vibe {

PowerController::PowerController(PowerConfig config, std::uint32_t nowMs)
    : config_(config), lastActivityMs_(nowMs) {
    if (config_.sleepAfterMs < config_.dimAfterMs) {
        config_.sleepAfterMs = config_.dimAfterMs;
    }
}

bool PowerController::noteUserActivity(std::uint32_t nowMs) {
    const bool wasAsleep = state_ == PowerState::Asleep;
    lastActivityMs_ = nowMs;
    state_ = PowerState::Active;
    return wasAsleep;
}

void PowerController::noteSystemActivity(std::uint32_t nowMs) {
    lastActivityMs_ = nowMs;
    state_ = PowerState::Active;
}

PowerState PowerController::update(std::uint32_t nowMs) {
    // Unsigned subtraction keeps this correct across millis() wrap-around.
    const std::uint32_t idle = nowMs - lastActivityMs_;
    if (idle >= config_.sleepAfterMs) {
        state_ = PowerState::Asleep;
    } else if (idle >= config_.dimAfterMs) {
        state_ = PowerState::Dimmed;
    } else {
        state_ = PowerState::Active;
    }
    return state_;
}

const char* powerStateName(PowerState state) {
    switch (state) {
        case PowerState::Active: return "active";
        case PowerState::Dimmed: return "dimmed";
        case PowerState::Asleep: return "asleep";
    }
    return "active";
}

}  // namespace vibe
