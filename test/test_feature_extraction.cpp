// Feature Extraction Tests
// Tests ML feature extraction helper functions

#include <unity.h>
#include <cmath>
#include "mocks/testable_functions.h"

void setUp(void) {
    // No setup needed
}

void tearDown(void) {
    // No teardown needed
}

// ============================================================================
// isRandomizedMAC() tests
// ============================================================================

void test_isRandomizedMAC_global_unicast(void) {
    // Standard global unicast MAC (bit 1 = 0)
    uint8_t mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    TEST_ASSERT_FALSE(isRandomizedMAC(mac));
}

void test_isRandomizedMAC_locally_administered(void) {
    // Locally administered (bit 1 = 1)
    uint8_t mac[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    TEST_ASSERT_TRUE(isRandomizedMAC(mac));
}

void test_isRandomizedMAC_common_random_pattern(void) {
    // Common randomized MAC patterns (x2, x6, xA, xE in first octet)
    uint8_t mac_02[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t mac_06[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t mac_0A[] = {0x0A, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t mac_0E[] = {0x0E, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    TEST_ASSERT_TRUE(isRandomizedMAC(mac_02));
    TEST_ASSERT_TRUE(isRandomizedMAC(mac_06));
    TEST_ASSERT_TRUE(isRandomizedMAC(mac_0A));
    TEST_ASSERT_TRUE(isRandomizedMAC(mac_0E));
}

void test_isRandomizedMAC_vendor_oui(void) {
    // Real vendor OUIs (globally administered)
    uint8_t apple[] = {0xF0, 0x18, 0x98, 0x00, 0x00, 0x00};      // Apple
    uint8_t intel[] = {0x00, 0x1B, 0x21, 0x00, 0x00, 0x00};      // Intel
    uint8_t cisco[] = {0x00, 0x00, 0x0C, 0x00, 0x00, 0x00};      // Cisco
    
    TEST_ASSERT_FALSE(isRandomizedMAC(apple));
    TEST_ASSERT_FALSE(isRandomizedMAC(intel));
    TEST_ASSERT_FALSE(isRandomizedMAC(cisco));
}

void test_isRandomizedMAC_android_style(void) {
    // Android random MAC (DA:xx:xx pattern)
    uint8_t android[] = {0xDA, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    TEST_ASSERT_TRUE(isRandomizedMAC(android));
}

void test_isRandomizedMAC_ios_style(void) {
    // iOS random MAC (typically x2, x6, xA, xE)
    uint8_t ios[] = {0x82, 0x12, 0x34, 0x56, 0x78, 0x9A};
    TEST_ASSERT_TRUE(isRandomizedMAC(ios));
}

// ============================================================================
// isMulticastMAC() tests
// ============================================================================

void test_isMulticastMAC_unicast(void) {
    uint8_t mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    TEST_ASSERT_FALSE(isMulticastMAC(mac));
}

void test_isMulticastMAC_broadcast(void) {
    uint8_t mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_TRUE(isMulticastMAC(mac));
}

void test_isMulticastMAC_multicast(void) {
    // IPv4 multicast: 01:00:5E:xx:xx:xx
    uint8_t mac[] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
    TEST_ASSERT_TRUE(isMulticastMAC(mac));
}

void test_isMulticastMAC_ipv6_multicast(void) {
    // IPv6 multicast: 33:33:xx:xx:xx:xx
    uint8_t mac[] = {0x33, 0x33, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_TRUE(isMulticastMAC(mac));
}

// ============================================================================
// normalizeValue() tests
// ============================================================================

void test_normalizeValue_standard_zscore(void) {
    // value=10, mean=5, std=2 -> z = (10-5)/2 = 2.5
    float z = normalizeValue(10.0f, 5.0f, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.5f, z);
}

void test_normalizeValue_negative_zscore(void) {
    // value=0, mean=5, std=2 -> z = (0-5)/2 = -2.5
    float z = normalizeValue(0.0f, 5.0f, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -2.5f, z);
}

void test_normalizeValue_at_mean_is_zero(void) {
    float z = normalizeValue(5.0f, 5.0f, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, z);
}

void test_normalizeValue_zero_std_returns_zero(void) {
    // Avoid division by zero
    float z = normalizeValue(10.0f, 5.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, z);
}

void test_normalizeValue_tiny_std_returns_zero(void) {
    // Very small std (< 0.001) should return 0
    float z = normalizeValue(10.0f, 5.0f, 0.0001f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, z);
}

void test_normalizeValue_large_values(void) {
    // Large RSSI-like values
    float z = normalizeValue(-30.0f, -60.0f, 15.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, z);  // (-30 - -60) / 15 = 2
}

// ============================================================================
// parseBeaconInterval() tests
// ============================================================================

void test_parseBeaconInterval_standard_frame(void) {
    // Construct minimal beacon frame with interval at offset 32-33
    uint8_t frame[40] = {0};
    // Standard beacon interval: 100 TU (0x0064)
    frame[32] = 0x64;
    frame[33] = 0x00;
    
    TEST_ASSERT_EQUAL_UINT16(100, parseBeaconInterval(frame, 40));
}

void test_parseBeaconInterval_custom_200ms(void) {
    uint8_t frame[40] = {0};
    // 200 TU (0x00C8)
    frame[32] = 0xC8;
    frame[33] = 0x00;
    
    TEST_ASSERT_EQUAL_UINT16(200, parseBeaconInterval(frame, 40));
}

void test_parseBeaconInterval_max_value(void) {
    uint8_t frame[40] = {0};
    // Max beacon interval (0xFFFF)
    frame[32] = 0xFF;
    frame[33] = 0xFF;
    
    TEST_ASSERT_EQUAL_UINT16(65535, parseBeaconInterval(frame, 40));
}

void test_parseBeaconInterval_too_short_frame(void) {
    uint8_t frame[30] = {0};
    // Frame too short - should return default 100
    TEST_ASSERT_EQUAL_UINT16(100, parseBeaconInterval(frame, 30));
}

void test_parseBeaconInterval_minimum_valid_length(void) {
    uint8_t frame[34] = {0};
    frame[32] = 0x50;  // 80 TU
    frame[33] = 0x00;
    
    TEST_ASSERT_EQUAL_UINT16(80, parseBeaconInterval(frame, 34));
}

// ============================================================================
// parseCapability() tests
// ============================================================================

void test_parseCapability_standard_frame(void) {
    uint8_t frame[40] = {0};
    // Capability at offset 34-35
    frame[34] = 0x11;  // ESS + Privacy
    frame[35] = 0x00;
    
    TEST_ASSERT_EQUAL_UINT16(0x0011, parseCapability(frame, 40));
}

void test_parseCapability_all_bits_set(void) {
    uint8_t frame[40] = {0};
    frame[34] = 0xFF;
    frame[35] = 0xFF;
    
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, parseCapability(frame, 40));
}

void test_parseCapability_too_short_frame(void) {
    uint8_t frame[30] = {0};
    TEST_ASSERT_EQUAL_UINT16(0, parseCapability(frame, 30));
}

void test_parseCapability_ess_bit(void) {
    uint8_t frame[40] = {0};
    frame[34] = 0x01;  // ESS (Infrastructure mode)
    frame[35] = 0x00;
    
    uint16_t cap = parseCapability(frame, 40);
    TEST_ASSERT_TRUE(cap & 0x0001);  // ESS bit set
}

void test_parseCapability_ibss_bit(void) {
    uint8_t frame[40] = {0};
    frame[34] = 0x02;  // IBSS (Ad-hoc mode)
    frame[35] = 0x00;
    
    uint16_t cap = parseCapability(frame, 40);
    TEST_ASSERT_TRUE(cap & 0x0002);  // IBSS bit set
}

void test_parseCapability_privacy_bit(void) {
    uint8_t frame[40] = {0};
    frame[34] = 0x10;  // Privacy (WEP/WPA)
    frame[35] = 0x00;
    
    uint16_t cap = parseCapability(frame, 40);
    TEST_ASSERT_TRUE(cap & 0x0010);  // Privacy bit set
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // MAC randomization tests
    RUN_TEST(test_isRandomizedMAC_global_unicast);
    RUN_TEST(test_isRandomizedMAC_locally_administered);
    RUN_TEST(test_isRandomizedMAC_common_random_pattern);
    RUN_TEST(test_isRandomizedMAC_vendor_oui);
    RUN_TEST(test_isRandomizedMAC_android_style);
    RUN_TEST(test_isRandomizedMAC_ios_style);
    
    // Multicast MAC tests
    RUN_TEST(test_isMulticastMAC_unicast);
    RUN_TEST(test_isMulticastMAC_broadcast);
    RUN_TEST(test_isMulticastMAC_multicast);
    RUN_TEST(test_isMulticastMAC_ipv6_multicast);
    
    // Normalization tests
    RUN_TEST(test_normalizeValue_standard_zscore);
    RUN_TEST(test_normalizeValue_negative_zscore);
    RUN_TEST(test_normalizeValue_at_mean_is_zero);
    RUN_TEST(test_normalizeValue_zero_std_returns_zero);
    RUN_TEST(test_normalizeValue_tiny_std_returns_zero);
    RUN_TEST(test_normalizeValue_large_values);
    
    // Beacon interval parsing tests
    RUN_TEST(test_parseBeaconInterval_standard_frame);
    RUN_TEST(test_parseBeaconInterval_custom_200ms);
    RUN_TEST(test_parseBeaconInterval_max_value);
    RUN_TEST(test_parseBeaconInterval_too_short_frame);
    RUN_TEST(test_parseBeaconInterval_minimum_valid_length);
    
    // Capability parsing tests
    RUN_TEST(test_parseCapability_standard_frame);
    RUN_TEST(test_parseCapability_all_bits_set);
    RUN_TEST(test_parseCapability_too_short_frame);
    RUN_TEST(test_parseCapability_ess_bit);
    RUN_TEST(test_parseCapability_ibss_bit);
    RUN_TEST(test_parseCapability_privacy_bit);
    
    return UNITY_END();
}
