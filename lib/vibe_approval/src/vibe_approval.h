#pragma once

// Transactional approval state machine.
//
// Adapted from neilshare/vibewatch lib/vibe_core/src/vibe_approval.{h,cpp}
// (MIT License, Copyright (c) 2026 GOROman and contributors). Changes: the
// Codex/Workbuddy/Antigravity card id became an agent slot, ids are sized for
// Hermes Agent's 32-hex request ids, the TTL ceiling follows Hermes'
// approvals.timeout, and requests carry a title plus detail line.
//
// This file has no Arduino/M5 dependencies so it builds in `pio test -e native`.

#include <cstddef>
#include <cstdint>

namespace vibe {

constexpr std::size_t kApprovalIdLength = 65;      // up to 64 chars + NUL
constexpr std::size_t kApprovalKindLength = 16;    // e.g. "EXEC"
constexpr std::size_t kApprovalTitleLength = 64;   // Hermes "description"
constexpr std::size_t kApprovalDetailLength = 160; // redacted command text
constexpr std::uint32_t kMinApprovalTtlMs = 5000;
constexpr std::uint32_t kMaxApprovalTtlMs = 600000;

enum class ApprovalChoice : std::uint8_t { Approve, Reject, Expired, Cancelled };
enum class ApprovalAcceptResult : std::uint8_t { Accepted, Duplicate, Busy, Invalid };

struct ApprovalRequest {
    char id[kApprovalIdLength]{};
    std::uint8_t slot{0};
    char kind[kApprovalKindLength]{};
    char title[kApprovalTitleLength]{};
    char detail[kApprovalDetailLength]{};
    std::uint32_t ttlMs{0};
};

struct ApprovalDecision {
    char id[kApprovalIdLength]{};
    ApprovalChoice choice{ApprovalChoice::Cancelled};
    std::uint32_t decidedAtMs{0};
};

template <typename T> struct OptionalValue {
    bool hasValue{false};
    T value{};
};

// Copies src into a fixed buffer, truncating on a UTF-8 boundary.
void copyUtf8(char* dst, std::size_t dstSize, const char* src);

// Largest byte count <= maxBytes that does not split a UTF-8 sequence,
// preferring to break after a space in the last third of the range.
std::size_t utf8WrapIndex(const char* str, std::size_t maxBytes);

const char* approvalChoiceName(ApprovalChoice choice);

class ApprovalController {
  public:
    ApprovalAcceptResult accept(const ApprovalRequest& request, std::uint32_t nowMs);
    OptionalValue<ApprovalDecision> decide(ApprovalChoice choice, std::uint32_t nowMs);
    OptionalValue<ApprovalDecision> expireIfNeeded(std::uint32_t nowMs);
    // Host withdrew the request (run ended, answered elsewhere). Returns true
    // if it matched the pending request; no decision is reported back.
    bool withdraw(const char* id);
    // Local teardown (link lost). Produces a Cancelled decision if pending.
    OptionalValue<ApprovalDecision> cancel(std::uint32_t nowMs);

    bool pending() const;
    const ApprovalRequest* current() const;
    std::uint32_t remainingMs(std::uint32_t nowMs) const;

  private:
    bool pending_{false};
    ApprovalRequest current_{};
    std::uint32_t receivedAtMs_{0};
};

}  // namespace vibe
