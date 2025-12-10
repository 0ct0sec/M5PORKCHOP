// XP Level Calculation Tests
// Tests the core XP/leveling system math

#include <unity.h>
#include "mocks/testable_functions.h"

void setUp(void) {
    // No setup needed for pure function tests
}

void tearDown(void) {
    // No teardown needed
}

// ============================================================================
// calculateLevel() tests
// ============================================================================

void test_calculateLevel_at_0_xp_is_level_1(void) {
    TEST_ASSERT_EQUAL_UINT8(1, calculateLevel(0));
}

void test_calculateLevel_at_99_xp_is_level_1(void) {
    TEST_ASSERT_EQUAL_UINT8(1, calculateLevel(99));
}

void test_calculateLevel_at_100_xp_is_level_2(void) {
    TEST_ASSERT_EQUAL_UINT8(2, calculateLevel(100));
}

void test_calculateLevel_at_101_xp_is_level_2(void) {
    TEST_ASSERT_EQUAL_UINT8(2, calculateLevel(101));
}

void test_calculateLevel_at_299_xp_is_level_2(void) {
    TEST_ASSERT_EQUAL_UINT8(2, calculateLevel(299));
}

void test_calculateLevel_at_300_xp_is_level_3(void) {
    TEST_ASSERT_EQUAL_UINT8(3, calculateLevel(300));
}

void test_calculateLevel_at_600_xp_is_level_4(void) {
    TEST_ASSERT_EQUAL_UINT8(4, calculateLevel(600));
}

void test_calculateLevel_at_1000_xp_is_level_5(void) {
    TEST_ASSERT_EQUAL_UINT8(5, calculateLevel(1000));
}

void test_calculateLevel_at_midgame_50000_xp(void) {
    // 49000 is level 20, 56000 is level 21
    TEST_ASSERT_EQUAL_UINT8(20, calculateLevel(50000));
}

void test_calculateLevel_at_599999_xp_is_level_39(void) {
    TEST_ASSERT_EQUAL_UINT8(39, calculateLevel(599999));
}

void test_calculateLevel_at_600000_xp_is_level_40(void) {
    TEST_ASSERT_EQUAL_UINT8(40, calculateLevel(600000));
}

void test_calculateLevel_at_max_uint32_is_level_40(void) {
    TEST_ASSERT_EQUAL_UINT8(40, calculateLevel(UINT32_MAX));
}

void test_calculateLevel_all_boundaries(void) {
    // Test each level boundary
    for (uint8_t level = 1; level <= MAX_LEVEL; level++) {
        uint32_t threshold = XP_THRESHOLDS[level - 1];
        TEST_ASSERT_EQUAL_UINT8(level, calculateLevel(threshold));
    }
}

// ============================================================================
// getXPForLevel() tests
// ============================================================================

void test_getXPForLevel_level_1_is_0(void) {
    TEST_ASSERT_EQUAL_UINT32(0, getXPForLevel(1));
}

void test_getXPForLevel_level_2_is_100(void) {
    TEST_ASSERT_EQUAL_UINT32(100, getXPForLevel(2));
}

void test_getXPForLevel_level_40_is_600000(void) {
    TEST_ASSERT_EQUAL_UINT32(600000, getXPForLevel(40));
}

void test_getXPForLevel_level_0_returns_0(void) {
    TEST_ASSERT_EQUAL_UINT32(0, getXPForLevel(0));
}

void test_getXPForLevel_level_41_returns_0(void) {
    TEST_ASSERT_EQUAL_UINT32(0, getXPForLevel(41));
}

void test_getXPForLevel_level_255_returns_0(void) {
    TEST_ASSERT_EQUAL_UINT32(0, getXPForLevel(255));
}

// ============================================================================
// getXPToNextLevel() tests
// ============================================================================

void test_getXPToNextLevel_at_0_xp(void) {
    // At 0 XP (level 1), need 100 XP to reach level 2
    TEST_ASSERT_EQUAL_UINT32(100, getXPToNextLevel(0));
}

void test_getXPToNextLevel_at_50_xp(void) {
    // At 50 XP (level 1), need 50 more XP to reach level 2
    TEST_ASSERT_EQUAL_UINT32(50, getXPToNextLevel(50));
}

void test_getXPToNextLevel_at_100_xp(void) {
    // At 100 XP (level 2), need 200 more XP to reach level 3 (threshold 300)
    TEST_ASSERT_EQUAL_UINT32(200, getXPToNextLevel(100));
}

void test_getXPToNextLevel_at_max_level(void) {
    // At max level, returns 0 (no next level)
    TEST_ASSERT_EQUAL_UINT32(0, getXPToNextLevel(600000));
}

void test_getXPToNextLevel_beyond_max_level(void) {
    // Beyond max level XP, still returns 0
    TEST_ASSERT_EQUAL_UINT32(0, getXPToNextLevel(1000000));
}

// ============================================================================
// getLevelProgress() tests
// ============================================================================

void test_getLevelProgress_at_level_start_is_0(void) {
    TEST_ASSERT_EQUAL_UINT8(0, getLevelProgress(0));
    TEST_ASSERT_EQUAL_UINT8(0, getLevelProgress(100));  // Start of level 2
    TEST_ASSERT_EQUAL_UINT8(0, getLevelProgress(300));  // Start of level 3
}

void test_getLevelProgress_at_50_percent(void) {
    // Level 1 is 0-99 XP (100 XP range), 50 XP is 50%
    TEST_ASSERT_EQUAL_UINT8(50, getLevelProgress(50));
}

void test_getLevelProgress_at_level_2_midpoint(void) {
    // Level 2 is 100-299 XP (200 XP range), 200 XP is 50%
    TEST_ASSERT_EQUAL_UINT8(50, getLevelProgress(200));
}

void test_getLevelProgress_at_99_percent(void) {
    // Level 1: 99 XP should be 99%
    TEST_ASSERT_EQUAL_UINT8(99, getLevelProgress(99));
}

void test_getLevelProgress_at_max_level_is_100(void) {
    TEST_ASSERT_EQUAL_UINT8(100, getLevelProgress(600000));
    TEST_ASSERT_EQUAL_UINT8(100, getLevelProgress(999999));
}

// ============================================================================
// Achievement bitfield tests
// ============================================================================

void test_hasAchievement_empty_bitfield(void) {
    TEST_ASSERT_FALSE(hasAchievement(0, 1ULL << 0));
    TEST_ASSERT_FALSE(hasAchievement(0, 1ULL << 46));
}

void test_hasAchievement_single_unlock(void) {
    uint64_t ach = 1ULL << 5;  // ACH_APPLE_FARMER
    TEST_ASSERT_TRUE(hasAchievement(ach, 1ULL << 5));
    TEST_ASSERT_FALSE(hasAchievement(ach, 1ULL << 6));
}

void test_hasAchievement_multiple_unlocks(void) {
    uint64_t ach = (1ULL << 0) | (1ULL << 8) | (1ULL << 46);
    TEST_ASSERT_TRUE(hasAchievement(ach, 1ULL << 0));
    TEST_ASSERT_TRUE(hasAchievement(ach, 1ULL << 8));
    TEST_ASSERT_TRUE(hasAchievement(ach, 1ULL << 46));
    TEST_ASSERT_FALSE(hasAchievement(ach, 1ULL << 1));
}

void test_unlockAchievement_from_empty(void) {
    uint64_t ach = unlockAchievement(0, 1ULL << 3);
    TEST_ASSERT_EQUAL_UINT64(1ULL << 3, ach);
}

void test_unlockAchievement_preserves_existing(void) {
    uint64_t ach = 1ULL << 5;
    ach = unlockAchievement(ach, 1ULL << 10);
    TEST_ASSERT_TRUE(hasAchievement(ach, 1ULL << 5));
    TEST_ASSERT_TRUE(hasAchievement(ach, 1ULL << 10));
}

void test_unlockAchievement_idempotent(void) {
    uint64_t ach = 1ULL << 5;
    uint64_t ach2 = unlockAchievement(ach, 1ULL << 5);
    TEST_ASSERT_EQUAL_UINT64(ach, ach2);
}

void test_countAchievements_empty(void) {
    TEST_ASSERT_EQUAL_UINT8(0, countAchievements(0));
}

void test_countAchievements_one(void) {
    TEST_ASSERT_EQUAL_UINT8(1, countAchievements(1ULL << 0));
    TEST_ASSERT_EQUAL_UINT8(1, countAchievements(1ULL << 46));
}

void test_countAchievements_multiple(void) {
    uint64_t ach = (1ULL << 0) | (1ULL << 5) | (1ULL << 10) | (1ULL << 46);
    TEST_ASSERT_EQUAL_UINT8(4, countAchievements(ach));
}

void test_countAchievements_all_47(void) {
    // All 47 achievements (bits 0-46)
    uint64_t ach = (1ULL << 47) - 1;
    TEST_ASSERT_EQUAL_UINT8(47, countAchievements(ach));
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // calculateLevel tests
    RUN_TEST(test_calculateLevel_at_0_xp_is_level_1);
    RUN_TEST(test_calculateLevel_at_99_xp_is_level_1);
    RUN_TEST(test_calculateLevel_at_100_xp_is_level_2);
    RUN_TEST(test_calculateLevel_at_101_xp_is_level_2);
    RUN_TEST(test_calculateLevel_at_299_xp_is_level_2);
    RUN_TEST(test_calculateLevel_at_300_xp_is_level_3);
    RUN_TEST(test_calculateLevel_at_600_xp_is_level_4);
    RUN_TEST(test_calculateLevel_at_1000_xp_is_level_5);
    RUN_TEST(test_calculateLevel_at_midgame_50000_xp);
    RUN_TEST(test_calculateLevel_at_599999_xp_is_level_39);
    RUN_TEST(test_calculateLevel_at_600000_xp_is_level_40);
    RUN_TEST(test_calculateLevel_at_max_uint32_is_level_40);
    RUN_TEST(test_calculateLevel_all_boundaries);
    
    // getXPForLevel tests
    RUN_TEST(test_getXPForLevel_level_1_is_0);
    RUN_TEST(test_getXPForLevel_level_2_is_100);
    RUN_TEST(test_getXPForLevel_level_40_is_600000);
    RUN_TEST(test_getXPForLevel_level_0_returns_0);
    RUN_TEST(test_getXPForLevel_level_41_returns_0);
    RUN_TEST(test_getXPForLevel_level_255_returns_0);
    
    // getXPToNextLevel tests
    RUN_TEST(test_getXPToNextLevel_at_0_xp);
    RUN_TEST(test_getXPToNextLevel_at_50_xp);
    RUN_TEST(test_getXPToNextLevel_at_100_xp);
    RUN_TEST(test_getXPToNextLevel_at_max_level);
    RUN_TEST(test_getXPToNextLevel_beyond_max_level);
    
    // getLevelProgress tests
    RUN_TEST(test_getLevelProgress_at_level_start_is_0);
    RUN_TEST(test_getLevelProgress_at_50_percent);
    RUN_TEST(test_getLevelProgress_at_level_2_midpoint);
    RUN_TEST(test_getLevelProgress_at_99_percent);
    RUN_TEST(test_getLevelProgress_at_max_level_is_100);
    
    // Achievement tests
    RUN_TEST(test_hasAchievement_empty_bitfield);
    RUN_TEST(test_hasAchievement_single_unlock);
    RUN_TEST(test_hasAchievement_multiple_unlocks);
    RUN_TEST(test_unlockAchievement_from_empty);
    RUN_TEST(test_unlockAchievement_preserves_existing);
    RUN_TEST(test_unlockAchievement_idempotent);
    RUN_TEST(test_countAchievements_empty);
    RUN_TEST(test_countAchievements_one);
    RUN_TEST(test_countAchievements_multiple);
    RUN_TEST(test_countAchievements_all_47);
    
    return UNITY_END();
}
