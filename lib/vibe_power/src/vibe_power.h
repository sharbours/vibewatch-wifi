#pragma once

// Inactivity-driven power states for the watch.
//
// Idea from neilshare/vibewatch (MIT): noteActivity() plus a timeout check in
// the main loop, stepping 240 -> 160 -> 80 MHz while dimming and then turning
// the screen off. Rewritten as a small state machine without Arduino
// dependencies so it can be unit-tested with `pio test -e native`.

#include <cstdint>

namespace vibe {

enum class PowerState : std::uint8_t { Active, Dimmed, Asleep };

struct PowerConfig {
    std::uint32_t dimAfterMs;
    std::uint32_t sleepAfterMs;  // measured from the last activity, not from dimming
};

class PowerController {
  public:
    PowerController(PowerConfig config, std::uint32_t nowMs);

    // User input (touch/button). Returns true if this input woke the screen
    // from Asleep, meaning the caller should swallow it rather than act on it.
    bool noteUserActivity(std::uint32_t nowMs);

    // Something that needs the screen on (approval pop-up, recording, a reply
    // playing). Wakes fully but never asks the caller to swallow input.
    void noteSystemActivity(std::uint32_t nowMs);

    // Advance timeouts. Returns the (possibly new) state.
    PowerState update(std::uint32_t nowMs);

    PowerState state() const { return state_; }
    const PowerConfig& config() const { return config_; }

  private:
    PowerConfig config_;
    PowerState state_{PowerState::Active};
    std::uint32_t lastActivityMs_;
};

const char* powerStateName(PowerState state);

}  // namespace vibe
