#include "vibe_approval.h"

#include <cstring>

namespace vibe {

namespace {

bool sameId(const char* a, const char* b) {
    return std::strncmp(a, b, kApprovalIdLength) == 0;
}

ApprovalDecision makeDecision(const ApprovalRequest& request, ApprovalChoice choice,
                              std::uint32_t nowMs) {
    ApprovalDecision decision{};
    std::memcpy(decision.id, request.id, kApprovalIdLength);
    decision.choice = choice;
    decision.decidedAtMs = nowMs;
    return decision;
}

bool validId(const char* id) {
    const std::size_t len = strnlen(id, kApprovalIdLength);
    if (len == 0 || len >= kApprovalIdLength) {
        return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
        const char c = id[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::size_t utf8WrapIndex(const char* str, std::size_t maxBytes) {
    if (str == nullptr) {
        return 0;
    }
    const std::size_t len = std::strlen(str);
    if (len <= maxBytes) {
        return len;
    }
    std::size_t idx = maxBytes;
    while (idx > 0 && (static_cast<unsigned char>(str[idx]) & 0xC0) == 0x80) {
        --idx;
    }
    if (idx == 0) {
        return 0;
    }
    for (std::size_t i = idx; i > (maxBytes * 2) / 3; --i) {
        if (str[i - 1] == ' ' || str[i - 1] == '/' || str[i - 1] == '-') {
            return i;
        }
    }
    return idx;
}

void copyUtf8(char* dst, std::size_t dstSize, const char* src) {
    if (dstSize == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::size_t n = std::strlen(src);
    if (n >= dstSize) {
        n = dstSize - 1;
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) {
            --n;
        }
    }
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

const char* approvalChoiceName(ApprovalChoice choice) {
    switch (choice) {
        case ApprovalChoice::Approve: return "approve";
        case ApprovalChoice::Reject: return "reject";
        case ApprovalChoice::Expired: return "expired";
        case ApprovalChoice::Cancelled: return "cancelled";
    }
    return "cancelled";
}

ApprovalAcceptResult ApprovalController::accept(const ApprovalRequest& request,
                                                std::uint32_t nowMs) {
    if (!validId(request.id)) {
        return ApprovalAcceptResult::Invalid;
    }
    if (pending_) {
        return sameId(current_.id, request.id) ? ApprovalAcceptResult::Duplicate
                                               : ApprovalAcceptResult::Busy;
    }
    current_ = request;
    if (current_.ttlMs < kMinApprovalTtlMs) {
        current_.ttlMs = kMinApprovalTtlMs;
    } else if (current_.ttlMs > kMaxApprovalTtlMs) {
        current_.ttlMs = kMaxApprovalTtlMs;
    }
    receivedAtMs_ = nowMs;
    pending_ = true;
    return ApprovalAcceptResult::Accepted;
}

OptionalValue<ApprovalDecision> ApprovalController::decide(ApprovalChoice choice,
                                                           std::uint32_t nowMs) {
    if (!pending_) {
        return {};
    }
    OptionalValue<ApprovalDecision> result{};
    result.hasValue = true;
    result.value = makeDecision(current_, choice, nowMs);
    pending_ = false;
    return result;
}

OptionalValue<ApprovalDecision> ApprovalController::expireIfNeeded(std::uint32_t nowMs) {
    if (!pending_ || static_cast<std::uint32_t>(nowMs - receivedAtMs_) < current_.ttlMs) {
        return {};
    }
    return decide(ApprovalChoice::Expired, nowMs);
}

bool ApprovalController::withdraw(const char* id) {
    if (!pending_ || id == nullptr || !sameId(current_.id, id)) {
        return false;
    }
    pending_ = false;
    return true;
}

OptionalValue<ApprovalDecision> ApprovalController::cancel(std::uint32_t nowMs) {
    return decide(ApprovalChoice::Cancelled, nowMs);
}

bool ApprovalController::pending() const {
    return pending_;
}

const ApprovalRequest* ApprovalController::current() const {
    return pending_ ? &current_ : nullptr;
}

std::uint32_t ApprovalController::remainingMs(std::uint32_t nowMs) const {
    if (!pending_) {
        return 0;
    }
    const std::uint32_t elapsed = nowMs - receivedAtMs_;
    return elapsed >= current_.ttlMs ? 0 : current_.ttlMs - elapsed;
}

}  // namespace vibe
