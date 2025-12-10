// Beacon Frame Parsing Tests
// Tests 802.11 beacon frame parsing for ML feature extraction

#include <unity.h>
#include <cstring>
#include "mocks/testable_functions.h"

void setUp(void) {
    // No setup needed
}

void tearDown(void) {
    // No teardown needed
}

// ============================================================================
// Helper: Build minimal beacon frame
// Structure: FC(2) + Dur(2) + DA(6) + SA(6) + BSSID(6) + Seq(2) = 24 bytes header
//            + Timestamp(8) + BeaconInterval(2) + Capability(2) = 12 bytes fixed params
//            + IEs (variable)
// Total minimum: 36 bytes before IEs
// ============================================================================

struct BeaconBuilder {
    uint8_t buffer[512];
    uint16_t len;
    
    BeaconBuilder() {
        memset(buffer, 0, sizeof(buffer));
        len = 36;  // Minimum beacon frame
        
        // Frame control: Type=Mgmt(0), Subtype=Beacon(8)
        buffer[0] = 0x80;
        buffer[1] = 0x00;
        
        // Default beacon interval: 100 TU
        buffer[32] = 0x64;
        buffer[33] = 0x00;
        
        // Default capability: ESS + Privacy
        buffer[34] = 0x11;
        buffer[35] = 0x00;
    }
    
    void setBeaconInterval(uint16_t interval) {
        buffer[32] = interval & 0xFF;
        buffer[33] = (interval >> 8) & 0xFF;
    }
    
    void setCapability(uint16_t cap) {
        buffer[34] = cap & 0xFF;
        buffer[35] = (cap >> 8) & 0xFF;
    }
    
    void addIE(uint8_t id, const uint8_t* data, uint8_t dataLen) {
        buffer[len] = id;
        buffer[len + 1] = dataLen;
        memcpy(buffer + len + 2, data, dataLen);
        len += 2 + dataLen;
    }
    
    void addSSID(const char* ssid) {
        size_t ssidLen = strlen(ssid);
        addIE(0, (const uint8_t*)ssid, ssidLen);
    }
    
    void addHiddenSSID() {
        // Zero-length SSID
        buffer[len] = 0;  // SSID IE
        buffer[len + 1] = 0;  // Zero length
        len += 2;
    }
    
    void addNullPaddedSSID(uint8_t padLen) {
        // SSID with all null bytes
        buffer[len] = 0;  // SSID IE
        buffer[len + 1] = padLen;
        memset(buffer + len + 2, 0, padLen);
        len += 2 + padLen;
    }
    
    void addSupportedRates(uint8_t count) {
        uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
        addIE(1, rates, count > 8 ? 8 : count);
    }
    
    void addDSParameterSet(uint8_t channel) {
        addIE(3, &channel, 1);
    }
    
    void addHTCapabilities() {
        uint8_t htCap[] = {0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00};
        addIE(45, htCap, 26);
    }
    
    void addRSN() {
        // Minimal RSN IE (WPA2)
        uint8_t rsn[] = {0x01, 0x00,  // Version
                        0x00, 0x0F, 0xAC, 0x04,  // Group cipher (CCMP)
                        0x01, 0x00,  // Pairwise cipher count
                        0x00, 0x0F, 0xAC, 0x04,  // Pairwise cipher (CCMP)
                        0x01, 0x00,  // AKM count
                        0x00, 0x0F, 0xAC, 0x02,  // AKM (PSK)
                        0x00, 0x00};  // RSN capabilities
        addIE(48, rsn, 20);
    }
    
    void addVHTCapabilities() {
        uint8_t vhtCap[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00};
        addIE(191, vhtCap, 12);
    }
    
    void addWPS() {
        // WPS vendor IE: 00:50:F2:04
        uint8_t wps[] = {0x00, 0x50, 0xF2, 0x04, 0x10, 0x4A, 0x00, 0x01, 0x10};
        addIE(221, wps, 9);
    }
    
    void addWPA() {
        // WPA1 vendor IE: 00:50:F2:01
        uint8_t wpa[] = {0x00, 0x50, 0xF2, 0x01, 0x01, 0x00,
                        0x00, 0x50, 0xF2, 0x02,  // TKIP cipher
                        0x01, 0x00,
                        0x00, 0x50, 0xF2, 0x02,
                        0x01, 0x00,
                        0x00, 0x50, 0xF2, 0x02};
        addIE(221, wpa, 22);
    }
    
    void addVendorIE(const uint8_t* oui, uint8_t ouiLen) {
        addIE(221, oui, ouiLen);
    }
};

// ============================================================================
// Beacon interval parsing tests
// ============================================================================

void test_beacon_parse_standard_100ms_interval(void) {
    BeaconBuilder b;
    b.setBeaconInterval(100);
    TEST_ASSERT_EQUAL_UINT16(100, parseBeaconInterval(b.buffer, b.len));
}

void test_beacon_parse_fast_50ms_interval(void) {
    BeaconBuilder b;
    b.setBeaconInterval(50);
    TEST_ASSERT_EQUAL_UINT16(50, parseBeaconInterval(b.buffer, b.len));
}

void test_beacon_parse_slow_1000ms_interval(void) {
    BeaconBuilder b;
    b.setBeaconInterval(1000);
    TEST_ASSERT_EQUAL_UINT16(1000, parseBeaconInterval(b.buffer, b.len));
}

// ============================================================================
// Capability parsing tests
// ============================================================================

void test_beacon_parse_ess_capability(void) {
    BeaconBuilder b;
    b.setCapability(0x0001);  // ESS only
    
    uint16_t cap = parseCapability(b.buffer, b.len);
    TEST_ASSERT_TRUE(cap & 0x0001);
    TEST_ASSERT_FALSE(cap & 0x0002);  // No IBSS
}

void test_beacon_parse_ibss_capability(void) {
    BeaconBuilder b;
    b.setCapability(0x0002);  // IBSS only
    
    uint16_t cap = parseCapability(b.buffer, b.len);
    TEST_ASSERT_FALSE(cap & 0x0001);  // No ESS
    TEST_ASSERT_TRUE(cap & 0x0002);   // IBSS
}

void test_beacon_parse_privacy_capability(void) {
    BeaconBuilder b;
    b.setCapability(0x0010);  // Privacy
    
    uint16_t cap = parseCapability(b.buffer, b.len);
    TEST_ASSERT_TRUE(cap & 0x0010);
}

void test_beacon_parse_combined_capabilities(void) {
    BeaconBuilder b;
    b.setCapability(0x0431);  // ESS + Privacy + Short Preamble + Short Slot
    
    uint16_t cap = parseCapability(b.buffer, b.len);
    TEST_ASSERT_EQUAL_UINT16(0x0431, cap);
}

// ============================================================================
// Frame length edge cases
// ============================================================================

void test_beacon_parse_minimum_valid_frame(void) {
    BeaconBuilder b;
    // Just the fixed params, no IEs
    TEST_ASSERT_EQUAL_UINT16(100, parseBeaconInterval(b.buffer, 36));
    TEST_ASSERT_EQUAL_UINT16(0x0011, parseCapability(b.buffer, 36));
}

void test_beacon_parse_truncated_at_beacon_interval(void) {
    BeaconBuilder b;
    // Truncated before beacon interval field
    TEST_ASSERT_EQUAL_UINT16(100, parseBeaconInterval(b.buffer, 32));  // Default
}

void test_beacon_parse_truncated_at_capability(void) {
    BeaconBuilder b;
    // Truncated before capability field
    TEST_ASSERT_EQUAL_UINT16(0, parseCapability(b.buffer, 34));  // Default 0
}

void test_beacon_parse_zero_length(void) {
    uint8_t empty[1] = {0};
    TEST_ASSERT_EQUAL_UINT16(100, parseBeaconInterval(empty, 0));
    TEST_ASSERT_EQUAL_UINT16(0, parseCapability(empty, 0));
}

// ============================================================================
// Complex beacon with multiple IEs
// ============================================================================

void test_beacon_with_all_common_ies(void) {
    BeaconBuilder b;
    b.setBeaconInterval(102);
    b.setCapability(0x0431);
    b.addSSID("TestNetwork");
    b.addSupportedRates(8);
    b.addDSParameterSet(6);
    b.addHTCapabilities();
    b.addRSN();
    b.addVHTCapabilities();
    b.addWPS();
    
    // Verify frame is valid and parseable
    TEST_ASSERT_EQUAL_UINT16(102, parseBeaconInterval(b.buffer, b.len));
    TEST_ASSERT_EQUAL_UINT16(0x0431, parseCapability(b.buffer, b.len));
    TEST_ASSERT_TRUE(b.len > 100);  // Should have substantial IEs
}

void test_beacon_wpa_wpa2_mixed_mode(void) {
    BeaconBuilder b;
    b.addSSID("MixedMode");
    b.addWPA();   // WPA1 vendor IE
    b.addRSN();   // WPA2 RSN IE
    
    TEST_ASSERT_TRUE(b.len > 80);  // Has both IEs
}

void test_beacon_hidden_ssid_zero_length(void) {
    BeaconBuilder b;
    b.addHiddenSSID();
    
    // Frame should have SSID IE with zero length
    TEST_ASSERT_EQUAL_UINT8(0, b.buffer[36]);   // SSID IE ID
    TEST_ASSERT_EQUAL_UINT8(0, b.buffer[37]);   // Zero length
}

void test_beacon_hidden_ssid_null_padded(void) {
    BeaconBuilder b;
    b.addNullPaddedSSID(8);  // 8 null bytes
    
    TEST_ASSERT_EQUAL_UINT8(0, b.buffer[36]);   // SSID IE ID
    TEST_ASSERT_EQUAL_UINT8(8, b.buffer[37]);   // Length 8
    TEST_ASSERT_EQUAL_UINT8(0, b.buffer[38]);   // First null byte
}

void test_beacon_multiple_vendor_ies(void) {
    BeaconBuilder b;
    b.addWPS();
    
    // Microsoft OUI
    uint8_t ms_oui[] = {0x00, 0x50, 0xF2, 0x02, 0x00, 0x00};
    b.addVendorIE(ms_oui, 6);
    
    // Broadcom OUI
    uint8_t bcm_oui[] = {0x00, 0x10, 0x18, 0x02, 0x00};
    b.addVendorIE(bcm_oui, 5);
    
    TEST_ASSERT_TRUE(b.len > 50);
}

// ============================================================================
// Real-world beacon scenarios
// ============================================================================

void test_beacon_typical_home_router(void) {
    BeaconBuilder b;
    b.setBeaconInterval(100);
    b.setCapability(0x0431);  // ESS + Privacy + Short Preamble + Short Slot
    b.addSSID("HomeNetwork");
    b.addSupportedRates(8);
    b.addDSParameterSet(11);
    b.addHTCapabilities();
    b.addRSN();
    
    TEST_ASSERT_EQUAL_UINT16(100, parseBeaconInterval(b.buffer, b.len));
    TEST_ASSERT_TRUE(parseCapability(b.buffer, b.len) & 0x0010);  // Privacy set
}

void test_beacon_open_hotspot(void) {
    BeaconBuilder b;
    b.setBeaconInterval(100);
    b.setCapability(0x0421);  // ESS + Short Preamble + Short Slot (no Privacy)
    b.addSSID("FreeWiFi");
    b.addSupportedRates(4);
    b.addDSParameterSet(1);
    
    uint16_t cap = parseCapability(b.buffer, b.len);
    TEST_ASSERT_TRUE(cap & 0x0001);   // ESS
    TEST_ASSERT_FALSE(cap & 0x0010);  // No Privacy (open)
}

void test_beacon_suspicious_rogue_ap(void) {
    BeaconBuilder b;
    b.setBeaconInterval(50);   // Non-standard interval
    b.setCapability(0x0001);   // Minimal capabilities
    b.addSSID("FreeWiFi");     // Common target name
    // No HT, no RSN, minimal IEs
    
    TEST_ASSERT_EQUAL_UINT16(50, parseBeaconInterval(b.buffer, b.len));
    TEST_ASSERT_TRUE(b.len < 50);  // Minimal frame
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Beacon interval parsing
    RUN_TEST(test_beacon_parse_standard_100ms_interval);
    RUN_TEST(test_beacon_parse_fast_50ms_interval);
    RUN_TEST(test_beacon_parse_slow_1000ms_interval);
    
    // Capability parsing
    RUN_TEST(test_beacon_parse_ess_capability);
    RUN_TEST(test_beacon_parse_ibss_capability);
    RUN_TEST(test_beacon_parse_privacy_capability);
    RUN_TEST(test_beacon_parse_combined_capabilities);
    
    // Frame length edge cases
    RUN_TEST(test_beacon_parse_minimum_valid_frame);
    RUN_TEST(test_beacon_parse_truncated_at_beacon_interval);
    RUN_TEST(test_beacon_parse_truncated_at_capability);
    RUN_TEST(test_beacon_parse_zero_length);
    
    // Complex beacons
    RUN_TEST(test_beacon_with_all_common_ies);
    RUN_TEST(test_beacon_wpa_wpa2_mixed_mode);
    RUN_TEST(test_beacon_hidden_ssid_zero_length);
    RUN_TEST(test_beacon_hidden_ssid_null_padded);
    RUN_TEST(test_beacon_multiple_vendor_ies);
    
    // Real-world scenarios
    RUN_TEST(test_beacon_typical_home_router);
    RUN_TEST(test_beacon_open_hotspot);
    RUN_TEST(test_beacon_suspicious_rogue_ap);
    
    return UNITY_END();
}
