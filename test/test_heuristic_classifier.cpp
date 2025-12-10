// Heuristic Classifier Tests
// Tests the anomaly detection and classification logic

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
// RSSI anomaly scoring tests
// ============================================================================

void test_anomalyScoreRSSI_normal_signal(void) {
    // Normal home router signal levels
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreRSSI(-50));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreRSSI(-60));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreRSSI(-70));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreRSSI(-80));
}

void test_anomalyScoreRSSI_weak_signal(void) {
    // Weak signals are not suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreRSSI(-90));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreRSSI(-100));
}

void test_anomalyScoreRSSI_boundary(void) {
    // Boundary at -30 dBm
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreRSSI(-30));   // Not suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, anomalyScoreRSSI(-29));   // Suspicious
}

void test_anomalyScoreRSSI_very_strong(void) {
    // Very strong signal is suspicious (possible rogue AP)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, anomalyScoreRSSI(-20));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, anomalyScoreRSSI(-10));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, anomalyScoreRSSI(0));
}

// ============================================================================
// Beacon interval anomaly scoring tests
// ============================================================================

void test_anomalyScoreBeaconInterval_standard(void) {
    // Standard intervals are not suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconInterval(100));  // Default
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconInterval(102));  // Common
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconInterval(50));   // Low boundary
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconInterval(200));  // High boundary
}

void test_anomalyScoreBeaconInterval_too_fast(void) {
    // Very fast beacons are suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreBeaconInterval(49));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreBeaconInterval(25));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreBeaconInterval(10));
}

void test_anomalyScoreBeaconInterval_too_slow(void) {
    // Very slow beacons are suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreBeaconInterval(201));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreBeaconInterval(500));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreBeaconInterval(1000));
}

// ============================================================================
// Open network anomaly scoring tests
// ============================================================================

void test_anomalyScoreOpenNetwork_encrypted(void) {
    // Encrypted networks are not suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreOpenNetwork(true, false, false));   // WPA
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreOpenNetwork(false, true, false));   // WPA2
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreOpenNetwork(false, false, true));   // WPA3
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreOpenNetwork(true, true, false));    // WPA+WPA2
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreOpenNetwork(false, true, true));    // WPA2+WPA3
}

void test_anomalyScoreOpenNetwork_open(void) {
    // Open networks are suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreOpenNetwork(false, false, false));
}

// ============================================================================
// WPS honeypot anomaly scoring tests
// ============================================================================

void test_anomalyScoreWPSHoneypot_no_wps(void) {
    // No WPS is not suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreWPSHoneypot(false, true, true, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreWPSHoneypot(false, false, false, false));
}

void test_anomalyScoreWPSHoneypot_wps_with_encryption(void) {
    // WPS with encryption is normal
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreWPSHoneypot(true, true, false, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreWPSHoneypot(true, false, true, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreWPSHoneypot(true, false, false, true));
}

void test_anomalyScoreWPSHoneypot_wps_on_open(void) {
    // WPS on open network is honeypot pattern
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, anomalyScoreWPSHoneypot(true, false, false, false));
}

// ============================================================================
// Inconsistent PHY anomaly scoring tests
// ============================================================================

void test_anomalyScoreInconsistentPHY_normal(void) {
    // Normal capability combinations
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreInconsistentPHY(false, false));  // Legacy
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreInconsistentPHY(false, true));   // HT only
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreInconsistentPHY(true, true));    // VHT+HT
}

void test_anomalyScoreInconsistentPHY_vht_without_ht(void) {
    // VHT without HT is inconsistent (shouldn't happen on real APs)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, anomalyScoreInconsistentPHY(true, false));
}

// ============================================================================
// Beacon jitter anomaly scoring tests
// ============================================================================

void test_anomalyScoreBeaconJitter_low(void) {
    // Hardware APs have low jitter
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconJitter(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconJitter(2.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconJitter(5.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreBeaconJitter(10.0f));  // Boundary
}

void test_anomalyScoreBeaconJitter_high(void) {
    // Software APs have high jitter
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.15f, anomalyScoreBeaconJitter(10.1f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.15f, anomalyScoreBeaconJitter(20.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.15f, anomalyScoreBeaconJitter(50.0f));
}

// ============================================================================
// Missing vendor IEs anomaly scoring tests
// ============================================================================

void test_anomalyScoreMissingVendorIEs_many(void) {
    // Real routers have many vendor IEs
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreMissingVendorIEs(5));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreMissingVendorIEs(10));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, anomalyScoreMissingVendorIEs(2));  // Boundary
}

void test_anomalyScoreMissingVendorIEs_few(void) {
    // Few vendor IEs is suspicious
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, anomalyScoreMissingVendorIEs(1));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, anomalyScoreMissingVendorIEs(0));
}

// ============================================================================
// Combined anomaly score scenarios
// ============================================================================

void test_combined_score_legitimate_router(void) {
    // Typical home router - should have low anomaly score
    float score = 0.0f;
    score += anomalyScoreRSSI(-55);                           // Normal signal
    score += anomalyScoreBeaconInterval(100);                 // Standard interval
    score += anomalyScoreOpenNetwork(false, true, false);     // WPA2 encrypted
    score += anomalyScoreWPSHoneypot(true, false, true, false); // WPS with WPA2
    score += anomalyScoreInconsistentPHY(false, true);        // HT only
    score += anomalyScoreBeaconJitter(2.0f);                  // Low jitter
    score += anomalyScoreMissingVendorIEs(5);                 // Normal vendor IEs
    
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, score);
}

void test_combined_score_suspicious_hotspot(void) {
    // Suspicious open hotspot
    float score = 0.0f;
    score += anomalyScoreRSSI(-25);                           // Very strong (+0.3)
    score += anomalyScoreBeaconInterval(100);                 // Standard
    score += anomalyScoreOpenNetwork(false, false, false);    // Open (+0.2)
    score += anomalyScoreWPSHoneypot(true, false, false, false); // WPS on open (+0.25)
    score += anomalyScoreInconsistentPHY(false, false);       // No 11n
    score += anomalyScoreBeaconJitter(5.0f);                  // Low jitter
    score += anomalyScoreMissingVendorIEs(1);                 // Few IEs (+0.1)
    
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.85f, score);
}

void test_combined_score_rogue_ap(void) {
    // Classic rogue AP pattern
    float score = 0.0f;
    score += anomalyScoreRSSI(-20);                           // Very strong (+0.3)
    score += anomalyScoreBeaconInterval(40);                  // Fast beacons (+0.2)
    score += anomalyScoreOpenNetwork(false, false, false);    // Open (+0.2)
    score += anomalyScoreWPSHoneypot(false, false, false, false); // No WPS
    score += anomalyScoreInconsistentPHY(true, false);        // VHT w/o HT (+0.2)
    score += anomalyScoreBeaconJitter(25.0f);                 // High jitter (+0.15)
    score += anomalyScoreMissingVendorIEs(0);                 // No vendor IEs (+0.1)
    
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.15f, score);  // High score
}

void test_combined_score_mobile_hotspot(void) {
    // Legitimate mobile hotspot - some flags but not malicious
    float score = 0.0f;
    score += anomalyScoreRSSI(-35);                           // Strong but ok
    score += anomalyScoreBeaconInterval(100);                 // Standard
    score += anomalyScoreOpenNetwork(false, true, false);     // WPA2
    score += anomalyScoreWPSHoneypot(false, false, true, false); // No WPS
    score += anomalyScoreInconsistentPHY(false, true);        // Has HT
    score += anomalyScoreBeaconJitter(15.0f);                 // Some jitter (+0.15)
    score += anomalyScoreMissingVendorIEs(2);                 // Minimal IEs
    
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.15f, score);  // Low-ish score
}

void test_combined_score_enterprise_ap(void) {
    // Enterprise AP with WPA3
    float score = 0.0f;
    score += anomalyScoreRSSI(-45);                           // Good signal
    score += anomalyScoreBeaconInterval(100);                 // Standard
    score += anomalyScoreOpenNetwork(false, true, true);      // WPA2+WPA3
    score += anomalyScoreWPSHoneypot(false, false, true, true); // No WPS
    score += anomalyScoreInconsistentPHY(true, true);         // VHT+HT
    score += anomalyScoreBeaconJitter(1.0f);                  // Low jitter
    score += anomalyScoreMissingVendorIEs(8);                 // Many vendor IEs
    
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, score);  // Perfect score
}

// ============================================================================
// Threshold tests
// ============================================================================

void test_anomaly_threshold_normal(void) {
    // Score < 0.5 is typically normal
    float score = 0.3f;
    TEST_ASSERT_TRUE(score < 0.5f);
}

void test_anomaly_threshold_suspicious(void) {
    // Score 0.5-0.8 is suspicious
    float score = 0.6f;
    TEST_ASSERT_TRUE(score >= 0.5f && score < 0.8f);
}

void test_anomaly_threshold_rogue(void) {
    // Score >= 0.8 is likely rogue
    float score = 0.9f;
    TEST_ASSERT_TRUE(score >= 0.8f);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // RSSI tests
    RUN_TEST(test_anomalyScoreRSSI_normal_signal);
    RUN_TEST(test_anomalyScoreRSSI_weak_signal);
    RUN_TEST(test_anomalyScoreRSSI_boundary);
    RUN_TEST(test_anomalyScoreRSSI_very_strong);
    
    // Beacon interval tests
    RUN_TEST(test_anomalyScoreBeaconInterval_standard);
    RUN_TEST(test_anomalyScoreBeaconInterval_too_fast);
    RUN_TEST(test_anomalyScoreBeaconInterval_too_slow);
    
    // Open network tests
    RUN_TEST(test_anomalyScoreOpenNetwork_encrypted);
    RUN_TEST(test_anomalyScoreOpenNetwork_open);
    
    // WPS honeypot tests
    RUN_TEST(test_anomalyScoreWPSHoneypot_no_wps);
    RUN_TEST(test_anomalyScoreWPSHoneypot_wps_with_encryption);
    RUN_TEST(test_anomalyScoreWPSHoneypot_wps_on_open);
    
    // Inconsistent PHY tests
    RUN_TEST(test_anomalyScoreInconsistentPHY_normal);
    RUN_TEST(test_anomalyScoreInconsistentPHY_vht_without_ht);
    
    // Beacon jitter tests
    RUN_TEST(test_anomalyScoreBeaconJitter_low);
    RUN_TEST(test_anomalyScoreBeaconJitter_high);
    
    // Missing vendor IEs tests
    RUN_TEST(test_anomalyScoreMissingVendorIEs_many);
    RUN_TEST(test_anomalyScoreMissingVendorIEs_few);
    
    // Combined scenarios
    RUN_TEST(test_combined_score_legitimate_router);
    RUN_TEST(test_combined_score_suspicious_hotspot);
    RUN_TEST(test_combined_score_rogue_ap);
    RUN_TEST(test_combined_score_mobile_hotspot);
    RUN_TEST(test_combined_score_enterprise_ap);
    
    // Threshold tests
    RUN_TEST(test_anomaly_threshold_normal);
    RUN_TEST(test_anomaly_threshold_suspicious);
    RUN_TEST(test_anomaly_threshold_rogue);
    
    return UNITY_END();
}
