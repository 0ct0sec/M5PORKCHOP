// HOGWASH Mode Tests
// Tests Karma AP SSID queue, probe parsing, and XP integration

#include <unity.h>
#include <cstring>
#include <cstdio>
#include "../mocks/testable_functions.h"

void setUp(void) {
    // No setup needed for pure function tests
}

void tearDown(void) {
    // No teardown needed
}

// ============================================================================
// SSID Ring Buffer Tests
// ============================================================================

// Simple ring buffer struct for testing (mirrors hogwash.cpp implementation)
struct SSIDEntry {
    char ssid[33];      // Max SSID + null
    uint32_t timestamp; // When this SSID was seen
    uint8_t probeCount; // How many times probed
};

struct SSIDQueue {
    SSIDEntry entries[8];
    uint8_t head;
    uint8_t count;
};

// Initialize queue
inline void ssidQueueInit(SSIDQueue* q) {
    memset(q, 0, sizeof(SSIDQueue));
}

// Add SSID to queue (returns true if new, false if duplicate)
inline bool ssidQueueAdd(SSIDQueue* q, const char* ssid, uint32_t timestamp) {
    if (ssid == nullptr || ssid[0] == '\0') return false;
    if (strlen(ssid) > 32) return false;
    
    // Check for duplicate
    for (uint8_t i = 0; i < q->count; i++) {
        uint8_t idx = (q->head + i) % 8;
        if (strcmp(q->entries[idx].ssid, ssid) == 0) {
            // Update timestamp and increment probe count
            q->entries[idx].timestamp = timestamp;
            q->entries[idx].probeCount++;
            return false;  // Duplicate
        }
    }
    
    // Add new entry
    uint8_t newIdx;
    if (q->count < 8) {
        newIdx = (q->head + q->count) % 8;
        q->count++;
    } else {
        // Queue full, overwrite oldest (head)
        newIdx = q->head;
        q->head = (q->head + 1) % 8;
    }
    
    strncpy(q->entries[newIdx].ssid, ssid, 32);
    q->entries[newIdx].ssid[32] = '\0';
    q->entries[newIdx].timestamp = timestamp;
    q->entries[newIdx].probeCount = 1;
    
    return true;  // New entry
}

// Get most recent SSID (for broadcasting)
inline const char* ssidQueueGetLatest(const SSIDQueue* q) {
    if (q->count == 0) return nullptr;
    uint8_t latestIdx = (q->head + q->count - 1) % 8;
    return q->entries[latestIdx].ssid;
}

// Get SSID count
inline uint8_t ssidQueueCount(const SSIDQueue* q) {
    return q->count;
}

void test_ssidQueue_init_empty(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    TEST_ASSERT_EQUAL_UINT8(0, ssidQueueCount(&q));
    TEST_ASSERT_NULL(ssidQueueGetLatest(&q));
}

void test_ssidQueue_add_single(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    bool isNew = ssidQueueAdd(&q, "TestNetwork", 1000);
    TEST_ASSERT_TRUE(isNew);
    TEST_ASSERT_EQUAL_UINT8(1, ssidQueueCount(&q));
    TEST_ASSERT_EQUAL_STRING("TestNetwork", ssidQueueGetLatest(&q));
}

void test_ssidQueue_add_duplicate(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    ssidQueueAdd(&q, "TestNetwork", 1000);
    bool isNew = ssidQueueAdd(&q, "TestNetwork", 2000);
    
    TEST_ASSERT_FALSE(isNew);  // Duplicate
    TEST_ASSERT_EQUAL_UINT8(1, ssidQueueCount(&q));  // Still 1
}

void test_ssidQueue_add_multiple(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    ssidQueueAdd(&q, "Network1", 1000);
    ssidQueueAdd(&q, "Network2", 2000);
    ssidQueueAdd(&q, "Network3", 3000);
    
    TEST_ASSERT_EQUAL_UINT8(3, ssidQueueCount(&q));
    TEST_ASSERT_EQUAL_STRING("Network3", ssidQueueGetLatest(&q));
}

void test_ssidQueue_overflow_wraps(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    // Fill queue with 8 entries
    for (int i = 0; i < 8; i++) {
        char ssid[20];
        snprintf(ssid, sizeof(ssid), "Net%d", i);
        ssidQueueAdd(&q, ssid, i * 1000);
    }
    
    TEST_ASSERT_EQUAL_UINT8(8, ssidQueueCount(&q));
    
    // Add 9th entry - should overwrite oldest
    ssidQueueAdd(&q, "Net8", 8000);
    
    TEST_ASSERT_EQUAL_UINT8(8, ssidQueueCount(&q));  // Still max 8
    TEST_ASSERT_EQUAL_STRING("Net8", ssidQueueGetLatest(&q));
}

void test_ssidQueue_rejects_null(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    bool isNew = ssidQueueAdd(&q, nullptr, 1000);
    TEST_ASSERT_FALSE(isNew);
    TEST_ASSERT_EQUAL_UINT8(0, ssidQueueCount(&q));
}

void test_ssidQueue_rejects_empty(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    bool isNew = ssidQueueAdd(&q, "", 1000);
    TEST_ASSERT_FALSE(isNew);
    TEST_ASSERT_EQUAL_UINT8(0, ssidQueueCount(&q));
}

void test_ssidQueue_rejects_too_long(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    // 33 character SSID (max is 32)
    bool isNew = ssidQueueAdd(&q, "123456789012345678901234567890123", 1000);
    TEST_ASSERT_FALSE(isNew);
    TEST_ASSERT_EQUAL_UINT8(0, ssidQueueCount(&q));
}

void test_ssidQueue_32_char_ssid_ok(void) {
    SSIDQueue q;
    ssidQueueInit(&q);
    
    // Exactly 32 characters (max valid)
    bool isNew = ssidQueueAdd(&q, "12345678901234567890123456789012", 1000);
    TEST_ASSERT_TRUE(isNew);
    TEST_ASSERT_EQUAL_UINT8(1, ssidQueueCount(&q));
}

// ============================================================================
// Probe Request Parsing Tests
// ============================================================================

// Parse SSID from probe request frame
// Probe request format: [24 byte header][Tagged params]
// Tag 0 = SSID: [tag=0][length][ssid bytes]
inline bool parseProbeSSID(const uint8_t* frame, uint16_t len, char* ssidOut, uint8_t maxLen) {
    if (frame == nullptr || len < 26 || ssidOut == nullptr) return false;
    
    // Skip 24 byte MAC header, start at tagged parameters
    const uint8_t* tagPtr = frame + 24;
    const uint8_t* endPtr = frame + len;
    
    while (tagPtr + 2 <= endPtr) {
        uint8_t tagNum = tagPtr[0];
        uint8_t tagLen = tagPtr[1];
        
        if (tagPtr + 2 + tagLen > endPtr) break;  // Invalid length
        
        if (tagNum == 0) {  // SSID tag
            if (tagLen == 0) {
                ssidOut[0] = '\0';  // Broadcast probe (empty SSID)
                return true;
            }
            if (tagLen > maxLen - 1) return false;  // Too long for buffer
            memcpy(ssidOut, tagPtr + 2, tagLen);
            ssidOut[tagLen] = '\0';
            return true;
        }
        
        tagPtr += 2 + tagLen;  // Move to next tag
    }
    
    return false;  // No SSID tag found
}

void test_parseProbeSSID_valid_ssid(void) {
    // Probe request with SSID "Test"
    uint8_t frame[30] = {0};
    // Tag 0 (SSID), length 4, "Test"
    frame[24] = 0x00;
    frame[25] = 0x04;
    frame[26] = 'T';
    frame[27] = 'e';
    frame[28] = 's';
    frame[29] = 't';
    
    char ssid[33] = {0};
    bool result = parseProbeSSID(frame, 30, ssid, sizeof(ssid));
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("Test", ssid);
}

void test_parseProbeSSID_broadcast_probe(void) {
    // Broadcast probe (empty SSID)
    uint8_t frame[26] = {0};
    frame[24] = 0x00;  // SSID tag
    frame[25] = 0x00;  // Length 0
    
    char ssid[33] = "garbage";
    bool result = parseProbeSSID(frame, 26, ssid, sizeof(ssid));
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_parseProbeSSID_frame_too_short(void) {
    uint8_t frame[20] = {0};
    char ssid[33];
    
    bool result = parseProbeSSID(frame, 20, ssid, sizeof(ssid));
    TEST_ASSERT_FALSE(result);
}

void test_parseProbeSSID_null_frame(void) {
    char ssid[33];
    bool result = parseProbeSSID(nullptr, 30, ssid, sizeof(ssid));
    TEST_ASSERT_FALSE(result);
}

void test_parseProbeSSID_long_ssid(void) {
    // 32 byte SSID (maximum)
    uint8_t frame[58] = {0};
    frame[24] = 0x00;  // SSID tag
    frame[25] = 32;    // Length 32
    for (int i = 0; i < 32; i++) {
        frame[26 + i] = 'A' + (i % 26);
    }
    
    char ssid[33] = {0};
    bool result = parseProbeSSID(frame, 58, ssid, sizeof(ssid));
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_UINT8(32, strlen(ssid));
}

// ============================================================================
// Achievement2 Bitfield Tests (second uint64_t for HOGWASH achievements)
// ============================================================================

// Check if achievement is unlocked in second bitfield
inline bool hasAchievement2(uint64_t achievements2, uint64_t achievementBit) {
    return (achievements2 & achievementBit) != 0;
}

// Unlock achievement in second bitfield
inline uint64_t unlockAchievement2(uint64_t achievements2, uint64_t achievementBit) {
    return achievements2 | achievementBit;
}

// HOGWASH achievement bits (in achievements2 field)
#define ACH2_FIRST_HOOK     (1ULL << 0)   // First device hooked via Karma
#define ACH2_KARMA_KING     (1ULL << 1)   // 50 devices hooked lifetime
#define ACH2_HONEY_POT      (1ULL << 2)   // 5 devices connected simultaneously
#define ACH2_TRAP_MASTER    (1ULL << 3)   // 100 unique SSIDs captured
#define ACH2_APPLE_PICKER   (1ULL << 4)   // Hook 10 Apple devices
#define ACH2_TRAFFIC_WARDEN (1ULL << 5)   // 30 minutes continuous HOGWASH

void test_hasAchievement2_empty(void) {
    TEST_ASSERT_FALSE(hasAchievement2(0, ACH2_FIRST_HOOK));
    TEST_ASSERT_FALSE(hasAchievement2(0, ACH2_KARMA_KING));
}

void test_hasAchievement2_single(void) {
    uint64_t ach2 = ACH2_FIRST_HOOK;
    TEST_ASSERT_TRUE(hasAchievement2(ach2, ACH2_FIRST_HOOK));
    TEST_ASSERT_FALSE(hasAchievement2(ach2, ACH2_KARMA_KING));
}

void test_unlockAchievement2_first(void) {
    uint64_t ach2 = 0;
    ach2 = unlockAchievement2(ach2, ACH2_FIRST_HOOK);
    TEST_ASSERT_EQUAL_UINT64(ACH2_FIRST_HOOK, ach2);
}

void test_unlockAchievement2_preserves_existing(void) {
    uint64_t ach2 = ACH2_FIRST_HOOK | ACH2_HONEY_POT;
    ach2 = unlockAchievement2(ach2, ACH2_KARMA_KING);
    
    TEST_ASSERT_TRUE(hasAchievement2(ach2, ACH2_FIRST_HOOK));
    TEST_ASSERT_TRUE(hasAchievement2(ach2, ACH2_HONEY_POT));
    TEST_ASSERT_TRUE(hasAchievement2(ach2, ACH2_KARMA_KING));
}

void test_unlockAchievement2_idempotent(void) {
    uint64_t ach2 = ACH2_FIRST_HOOK;
    uint64_t ach2_again = unlockAchievement2(ach2, ACH2_FIRST_HOOK);
    TEST_ASSERT_EQUAL_UINT64(ach2, ach2_again);
}

// ============================================================================
// XP Anti-Farm Cap Tests
// ============================================================================

struct HogwashSessionStats {
    uint16_t probeXP;
    uint16_t hookXP;
    bool capWarned;
};

const uint16_t HOGWASH_PROBE_XP_CAP = 200;  // Max XP from probes per session

inline uint16_t applyHogwashXPCap(HogwashSessionStats* stats, uint16_t xpToAdd, bool isProbeEvent) {
    if (!isProbeEvent) {
        stats->hookXP += xpToAdd;
        return xpToAdd;  // Hook XP not capped
    }
    
    if (stats->probeXP >= HOGWASH_PROBE_XP_CAP) {
        if (!stats->capWarned) {
            stats->capWarned = true;
        }
        return 0;  // Capped
    }
    
    uint16_t remaining = HOGWASH_PROBE_XP_CAP - stats->probeXP;
    uint16_t awarded = (xpToAdd <= remaining) ? xpToAdd : remaining;
    stats->probeXP += awarded;
    return awarded;
}

void test_hogwashXPCap_under_limit(void) {
    HogwashSessionStats stats = {0};
    uint16_t awarded = applyHogwashXPCap(&stats, 10, true);
    TEST_ASSERT_EQUAL_UINT16(10, awarded);
    TEST_ASSERT_EQUAL_UINT16(10, stats.probeXP);
}

void test_hogwashXPCap_at_limit(void) {
    HogwashSessionStats stats = {.probeXP = 195, .hookXP = 0, .capWarned = false};
    uint16_t awarded = applyHogwashXPCap(&stats, 10, true);
    TEST_ASSERT_EQUAL_UINT16(5, awarded);  // Only 5 remaining
    TEST_ASSERT_EQUAL_UINT16(200, stats.probeXP);
}

void test_hogwashXPCap_over_limit(void) {
    HogwashSessionStats stats = {.probeXP = 200, .hookXP = 0, .capWarned = false};
    uint16_t awarded = applyHogwashXPCap(&stats, 10, true);
    TEST_ASSERT_EQUAL_UINT16(0, awarded);
    TEST_ASSERT_TRUE(stats.capWarned);
}

void test_hogwashXPCap_hook_not_capped(void) {
    HogwashSessionStats stats = {.probeXP = 200, .hookXP = 0, .capWarned = true};
    uint16_t awarded = applyHogwashXPCap(&stats, 25, false);  // Hook event
    TEST_ASSERT_EQUAL_UINT16(25, awarded);  // Hooks not capped
    TEST_ASSERT_EQUAL_UINT16(25, stats.hookXP);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // SSID Queue tests
    RUN_TEST(test_ssidQueue_init_empty);
    RUN_TEST(test_ssidQueue_add_single);
    RUN_TEST(test_ssidQueue_add_duplicate);
    RUN_TEST(test_ssidQueue_add_multiple);
    RUN_TEST(test_ssidQueue_overflow_wraps);
    RUN_TEST(test_ssidQueue_rejects_null);
    RUN_TEST(test_ssidQueue_rejects_empty);
    RUN_TEST(test_ssidQueue_rejects_too_long);
    RUN_TEST(test_ssidQueue_32_char_ssid_ok);
    
    // Probe parsing tests
    RUN_TEST(test_parseProbeSSID_valid_ssid);
    RUN_TEST(test_parseProbeSSID_broadcast_probe);
    RUN_TEST(test_parseProbeSSID_frame_too_short);
    RUN_TEST(test_parseProbeSSID_null_frame);
    RUN_TEST(test_parseProbeSSID_long_ssid);
    
    // Achievement2 tests
    RUN_TEST(test_hasAchievement2_empty);
    RUN_TEST(test_hasAchievement2_single);
    RUN_TEST(test_unlockAchievement2_first);
    RUN_TEST(test_unlockAchievement2_preserves_existing);
    RUN_TEST(test_unlockAchievement2_idempotent);
    
    // XP cap tests
    RUN_TEST(test_hogwashXPCap_under_limit);
    RUN_TEST(test_hogwashXPCap_at_limit);
    RUN_TEST(test_hogwashXPCap_over_limit);
    RUN_TEST(test_hogwashXPCap_hook_not_capped);
    
    return UNITY_END();
}
