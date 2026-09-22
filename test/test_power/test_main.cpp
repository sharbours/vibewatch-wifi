// Host-side unit tests: `pio test -e native`
#include <unity.h>

#include "vibe_power.h"

using namespace vibe;

void setUp() {}
void tearDown() {}

void test_steps_through_dim_then_sleep() {
    PowerController p({30000, 90000}, 0);
    TEST_ASSERT_EQUAL(PowerState::Active, p.update(29999));
    TEST_ASSERT_EQUAL(PowerState::Dimmed, p.update(30000));
    TEST_ASSERT_EQUAL(PowerState::Dimmed, p.update(89999));
    TEST_ASSERT_EQUAL(PowerState::Asleep, p.update(90000));
}

void test_wake_from_sleep_is_swallowed_but_dim_is_not() {
    PowerController p({1000, 2000}, 0);
    p.update(1500);
    TEST_ASSERT_FALSE(p.noteUserActivity(1500));  // dimmed: screen visible, act on it
    p.update(5000);
    TEST_ASSERT_EQUAL(PowerState::Asleep, p.state());
    TEST_ASSERT_TRUE(p.noteUserActivity(5000));   // dark: this tap only wakes
    TEST_ASSERT_EQUAL(PowerState::Active, p.state());
    TEST_ASSERT_EQUAL(PowerState::Active, p.update(5500));
}

void test_system_activity_keeps_awake() {
    PowerController p({1000, 2000}, 0);
    for (std::uint32_t t = 0; t < 10000; t += 500) {
        p.noteSystemActivity(t);
        TEST_ASSERT_EQUAL(PowerState::Active, p.update(t));
    }
    TEST_ASSERT_EQUAL(PowerState::Asleep, p.update(20000));
    p.noteSystemActivity(20000);  // e.g. approval arrives while asleep
    TEST_ASSERT_EQUAL(PowerState::Active, p.state());
}

void test_millis_wraparound() {
    const std::uint32_t start = 0xFFFFFF00u;
    PowerController p({1000, 2000}, start);
    TEST_ASSERT_EQUAL(PowerState::Active, p.update(start + 999));
    TEST_ASSERT_EQUAL(PowerState::Asleep, p.update(start + 2500));  // wrapped past zero
}

void test_bad_config_is_clamped() {
    PowerController p({5000, 1000}, 0);
    TEST_ASSERT_EQUAL_UINT32(5000, p.config().sleepAfterMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_steps_through_dim_then_sleep);
    RUN_TEST(test_wake_from_sleep_is_swallowed_but_dim_is_not);
    RUN_TEST(test_system_activity_keeps_awake);
    RUN_TEST(test_millis_wraparound);
    RUN_TEST(test_bad_config_is_clamped);
    return UNITY_END();
}
