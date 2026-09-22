#pragma once

// Hermes status snapshot shown by the gauge ring and status page.
//
// Adapted from neilshare/vibewatch lib/vibe_core/src/vibe_quota.{h,cpp}
// (MIT). Same rules: validate before accepting, remember when it arrived, and
// report Unavailable/Stale instead of ever showing invented numbers. The
// Codex quota fields became Hermes health, active runs, jobs, and a daily
// token budget.

#include <cstddef>
#include <cstdint>

namespace vibe {

constexpr int kStatusSlotCount = 6;

enum class HostHealth : std::uint8_t { Unknown, Ok, Degraded, Offline };
enum class StatusFreshness : std::uint8_t { Unavailable, Fresh, Stale };

struct StatusInput {
    const char* health{""};
    std::int32_t runs{0};
    std::int32_t jobs{0};
    double tokensToday{0};
    double budget{0};           // 0 = no budget configured
    double remainingPercent{100};
    std::int64_t resetInSeconds{0};
    double slotTokens[kStatusSlotCount]{};
    std::uint32_t staleAfterSeconds{90};
};

struct HostStatus {
    HostHealth health{HostHealth::Unknown};
    std::uint16_t runs{0};
    std::uint16_t jobs{0};
    std::uint32_t tokensToday{0};
    std::uint32_t budget{0};
    float remainingPercent{100};
    std::uint32_t resetInSeconds{0};
    std::uint32_t slotTokens[kStatusSlotCount]{};
    std::uint32_t staleAfterMs{90000};
    std::uint32_t receivedAtMs{0};
    bool available{false};

    // Returns false (and keeps the previous snapshot) if anything is out of range.
    bool apply(const StatusInput& in, std::uint32_t nowMs);
    StatusFreshness freshness(std::uint32_t nowMs) const;
    std::uint32_t ageSeconds(std::uint32_t nowMs) const;
    // Reset countdown advanced by the time since the snapshot arrived.
    std::uint32_t resetRemainingSeconds(std::uint32_t nowMs) const;
    bool hasBudget() const { return budget > 0; }
};

HostHealth parseHealth(const char* text);
const char* healthName(HostHealth health);

// Compact token counts for a small screen: 950, 12.3k, 4.1M.
void formatTokens(std::uint32_t tokens, char* out, std::size_t outSize);
// "5h 12m", "12m", "45s"
void formatDuration(std::uint32_t seconds, char* out, std::size_t outSize);

}  // namespace vibe
