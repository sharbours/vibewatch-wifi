#include "vibe_status.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace vibe {

namespace {

bool validCount(double v, double max) {
    return std::isfinite(v) && v >= 0 && v <= max;
}

}  // namespace

HostHealth parseHealth(const char* text) {
    if (text == nullptr) return HostHealth::Unknown;
    if (std::strcmp(text, "ok") == 0) return HostHealth::Ok;
    if (std::strcmp(text, "degraded") == 0) return HostHealth::Degraded;
    if (std::strcmp(text, "offline") == 0) return HostHealth::Offline;
    return HostHealth::Unknown;
}

const char* healthName(HostHealth health) {
    switch (health) {
        case HostHealth::Ok: return "OK";
        case HostHealth::Degraded: return "DEGRADED";
        case HostHealth::Offline: return "OFFLINE";
        case HostHealth::Unknown: break;
    }
    return "UNKNOWN";
}

bool HostStatus::apply(const StatusInput& in, std::uint32_t nowMs) {
    const HostHealth parsed = parseHealth(in.health);
    if (parsed == HostHealth::Unknown) return false;
    if (in.runs < 0 || in.runs > 1000 || in.jobs < 0 || in.jobs > 10000) return false;
    if (!validCount(in.tokensToday, 4.0e9) || !validCount(in.budget, 4.0e9)) return false;
    if (!std::isfinite(in.remainingPercent) || in.remainingPercent < 0 || in.remainingPercent > 100) {
        return false;
    }
    if (in.resetInSeconds < 0 || in.resetInSeconds > 7 * 24 * 3600) return false;
    for (double v : in.slotTokens) {
        if (!validCount(v, 4.0e9)) return false;
    }
    if (in.staleAfterSeconds < 10 || in.staleAfterSeconds > 3600) return false;

    health = parsed;
    runs = static_cast<std::uint16_t>(in.runs);
    jobs = static_cast<std::uint16_t>(in.jobs);
    tokensToday = static_cast<std::uint32_t>(in.tokensToday);
    budget = static_cast<std::uint32_t>(in.budget);
    remainingPercent = static_cast<float>(in.remainingPercent);
    resetInSeconds = static_cast<std::uint32_t>(in.resetInSeconds);
    for (int i = 0; i < kStatusSlotCount; ++i) {
        slotTokens[i] = static_cast<std::uint32_t>(in.slotTokens[i]);
    }
    staleAfterMs = in.staleAfterSeconds * 1000U;
    receivedAtMs = nowMs;
    available = true;
    return true;
}

StatusFreshness HostStatus::freshness(std::uint32_t nowMs) const {
    if (!available) return StatusFreshness::Unavailable;
    return static_cast<std::uint32_t>(nowMs - receivedAtMs) > staleAfterMs ? StatusFreshness::Stale
                                                                            : StatusFreshness::Fresh;
}

std::uint32_t HostStatus::ageSeconds(std::uint32_t nowMs) const {
    return available ? static_cast<std::uint32_t>(nowMs - receivedAtMs) / 1000U : 0;
}

std::uint32_t HostStatus::resetRemainingSeconds(std::uint32_t nowMs) const {
    const std::uint32_t age = ageSeconds(nowMs);
    return age >= resetInSeconds ? 0 : resetInSeconds - age;
}

void formatTokens(std::uint32_t tokens, char* out, std::size_t outSize) {
    if (tokens < 1000) {
        std::snprintf(out, outSize, "%u", static_cast<unsigned>(tokens));
    } else if (tokens < 1000000) {
        std::snprintf(out, outSize, "%.1fk", tokens / 1000.0);
    } else {
        std::snprintf(out, outSize, "%.1fM", tokens / 1000000.0);
    }
}

void formatDuration(std::uint32_t seconds, char* out, std::size_t outSize) {
    if (seconds >= 3600) {
        std::snprintf(out, outSize, "%uh %um", static_cast<unsigned>(seconds / 3600),
                      static_cast<unsigned>((seconds % 3600) / 60));
    } else if (seconds >= 60) {
        std::snprintf(out, outSize, "%um", static_cast<unsigned>(seconds / 60));
    } else {
        std::snprintf(out, outSize, "%us", static_cast<unsigned>(seconds));
    }
}

}  // namespace vibe
