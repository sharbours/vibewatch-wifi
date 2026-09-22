// Host-side unit tests: `pio test -e native`
#include <unity.h>

#include <cstring>

#include "vibe_approval.h"

using namespace vibe;

namespace {

ApprovalRequest makeRequest(const char* id, std::uint32_t ttl = 30000) {
    ApprovalRequest r{};
    copyUtf8(r.id, sizeof(r.id), id);
    copyUtf8(r.kind, sizeof(r.kind), "EXEC");
    copyUtf8(r.title, sizeof(r.title), "recursive delete");
    copyUtf8(r.detail, sizeof(r.detail), "rm -rf build/");
    r.slot = 2;
    r.ttlMs = ttl;
    return r;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_accept_and_approve_keeps_request_id() {
    ApprovalController c;
    TEST_ASSERT_EQUAL(ApprovalAcceptResult::Accepted, c.accept(makeRequest("abc123"), 1000));
    auto d = c.decide(ApprovalChoice::Approve, 2000);
    TEST_ASSERT_TRUE(d.hasValue);
    TEST_ASSERT_EQUAL_STRING("abc123", d.value.id);
    TEST_ASSERT_EQUAL_STRING("approve", approvalChoiceName(d.value.choice));
    TEST_ASSERT_FALSE(c.pending());
}

void test_second_request_is_busy_and_duplicate_is_idempotent() {
    ApprovalController c;
    c.accept(makeRequest("one"), 0);
    TEST_ASSERT_EQUAL(ApprovalAcceptResult::Duplicate, c.accept(makeRequest("one"), 10));
    TEST_ASSERT_EQUAL(ApprovalAcceptResult::Busy, c.accept(makeRequest("two"), 10));
    TEST_ASSERT_EQUAL_STRING("one", c.current()->id);
}

void test_invalid_ids_rejected() {
    ApprovalController c;
    TEST_ASSERT_EQUAL(ApprovalAcceptResult::Invalid, c.accept(makeRequest(""), 0));
    TEST_ASSERT_EQUAL(ApprovalAcceptResult::Invalid, c.accept(makeRequest("bad\"id"), 0));
    TEST_ASSERT_FALSE(c.pending());
}

void test_expiry_fails_closed_and_handles_millis_wrap() {
    ApprovalController c;
    const std::uint32_t start = 0xFFFFF000u;  // millis() about to wrap
    c.accept(makeRequest("wrap", 10000), start);
    TEST_ASSERT_FALSE(c.expireIfNeeded(start + 9999).hasValue);
    auto d = c.expireIfNeeded(start + 10000);
    TEST_ASSERT_TRUE(d.hasValue);
    TEST_ASSERT_EQUAL(ApprovalChoice::Expired, d.value.choice);
}

void test_ttl_is_clamped() {
    ApprovalController c;
    c.accept(makeRequest("short", 10), 0);
    TEST_ASSERT_EQUAL_UINT32(kMinApprovalTtlMs, c.remainingMs(0));
}

void test_withdraw_only_matches_current() {
    ApprovalController c;
    c.accept(makeRequest("keep"), 0);
    TEST_ASSERT_FALSE(c.withdraw("other"));
    TEST_ASSERT_TRUE(c.pending());
    TEST_ASSERT_TRUE(c.withdraw("keep"));
    TEST_ASSERT_FALSE(c.pending());
    TEST_ASSERT_FALSE(c.decide(ApprovalChoice::Approve, 1).hasValue);
}

void test_utf8_helpers_never_split_sequences() {
    char buf[6];
    copyUtf8(buf, sizeof(buf), "ab\xC3\xA9\xC3\xA9");  // "abéé" = 6 bytes
    TEST_ASSERT_EQUAL_STRING("ab\xC3\xA9", buf);
    // Breaks after the "/" so the first line reads "rm -rf /tmp/".
    TEST_ASSERT_EQUAL_UINT32(12, utf8WrapIndex("rm -rf /tmp/some/long/path", 12));
    TEST_ASSERT_EQUAL_UINT32(4, utf8WrapIndex("abcd\xC3\xA9" "fgh", 5));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_accept_and_approve_keeps_request_id);
    RUN_TEST(test_second_request_is_busy_and_duplicate_is_idempotent);
    RUN_TEST(test_invalid_ids_rejected);
    RUN_TEST(test_expiry_fails_closed_and_handles_millis_wrap);
    RUN_TEST(test_ttl_is_clamped);
    RUN_TEST(test_withdraw_only_matches_current);
    RUN_TEST(test_utf8_helpers_never_split_sequences);
    return UNITY_END();
}
