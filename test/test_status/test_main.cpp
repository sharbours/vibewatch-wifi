// Host-side unit tests: `pio test -e native`
#include <unity.h>

#include <cmath>

#include "vibe_status.h"

using namespace vibe;

namespace {
StatusInput good() {
    StatusInput in;
    in.health = "ok";
    in.runs = 1;
    in.jobs = 3;
    in.tokensToday = 12345;
    in.budget = 200000;
    in.remainingPercent = 93.8;
    in.resetInSeconds = 3600;
    in.slotTokens[2] = 4000;
    in.staleAfterSeconds = 90;
    return in;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_unavailable_until_first_snapshot() {
    HostStatus s;
    TEST_ASSERT_EQUAL(StatusFreshness::Unavailable, s.freshness(123));
}

void test_fresh_then_stale() {
    HostStatus s;
    TEST_ASSERT_TRUE(s.apply(good(), 1000));
    TEST_ASSERT_EQUAL(StatusFreshness::Fresh, s.freshness(91000));
    TEST_ASSERT_EQUAL(StatusFreshness::Stale, s.freshness(91001));
    TEST_ASSERT_EQUAL_UINT32(4000, s.slotTokens[2]);
}

void test_rejects_bad_values_and_keeps_previous() {
    HostStatus s;
    s.apply(good(), 0);
    StatusInput bad = good();
    bad.remainingPercent = 140;
    TEST_ASSERT_FALSE(s.apply(bad, 5000));
    bad = good();
    bad.health = "fine";
    TEST_ASSERT_FALSE(s.apply(bad, 5000));
    bad = good();
    bad.tokensToday = NAN;
    TEST_ASSERT_FALSE(s.apply(bad, 5000));
    bad = good();
    bad.runs = -1;
    TEST_ASSERT_FALSE(s.apply(bad, 5000));
    TEST_ASSERT_EQUAL_UINT32(0, s.receivedAtMs);  // untouched
    TEST_ASSERT_EQUAL_UINT32(12345, s.tokensToday);
}

void test_reset_countdown_advances_and_floors_at_zero() {
    HostStatus s;
    s.apply(good(), 0);
    TEST_ASSERT_EQUAL_UINT32(3540, s.resetRemainingSeconds(60000));
    TEST_ASSERT_EQUAL_UINT32(0, s.resetRemainingSeconds(4000000));
}

void test_formatting() {
    char b[16];
    formatTokens(950, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("950", b);
    formatTokens(12345, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("12.3k", b);
    formatTokens(4100000, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("4.1M", b);
    formatDuration(18720, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("5h 12m", b);
    formatDuration(45, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("45s", b);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unavailable_until_first_snapshot);
    RUN_TEST(test_fresh_then_stale);
    RUN_TEST(test_rejects_bad_values_and_keeps_previous);
    RUN_TEST(test_reset_countdown_advances_and_floors_at_zero);
    RUN_TEST(test_formatting);
    return UNITY_END();
}
