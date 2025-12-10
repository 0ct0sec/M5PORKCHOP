// GPS Distance Calculation Tests
// Tests the Haversine formula for accurate GPS distance measurement

#include <unity.h>
#include <cmath>
#include "mocks/testable_functions.h"

void setUp(void) {
    // No setup needed
}

void tearDown(void) {
    // No teardown needed
}

// Helper: compare doubles with tolerance
#define ASSERT_DOUBLE_WITHIN(delta, expected, actual) \
    TEST_ASSERT_DOUBLE_WITHIN(delta, expected, actual)

// ============================================================================
// haversineMeters() basic tests
// ============================================================================

void test_haversine_same_point_returns_zero(void) {
    double d = haversineMeters(51.5074, -0.1278, 51.5074, -0.1278);
    ASSERT_DOUBLE_WITHIN(0.001, 0.0, d);
}

void test_haversine_very_close_points(void) {
    // Two points ~11 meters apart (0.0001 degrees at equator)
    double d = haversineMeters(0.0, 0.0, 0.0, 0.0001);
    ASSERT_DOUBLE_WITHIN(1.0, 11.1, d);  // ~11.1m per 0.0001 degrees at equator
}

void test_haversine_known_distance_london_to_paris(void) {
    // London (51.5074, -0.1278) to Paris (48.8566, 2.3522)
    // Known distance: ~344 km
    double d = haversineMeters(51.5074, -0.1278, 48.8566, 2.3522);
    ASSERT_DOUBLE_WITHIN(5000.0, 344000.0, d);  // Within 5km tolerance
}

void test_haversine_known_distance_nyc_to_la(void) {
    // NYC (40.7128, -74.0060) to LA (34.0522, -118.2437)
    // Known distance: ~3940 km
    double d = haversineMeters(40.7128, -74.0060, 34.0522, -118.2437);
    ASSERT_DOUBLE_WITHIN(50000.0, 3940000.0, d);  // Within 50km tolerance
}

void test_haversine_across_equator(void) {
    // From 10N to 10S at same longitude
    // 20 degrees latitude ~= 2222 km
    double d = haversineMeters(10.0, 0.0, -10.0, 0.0);
    ASSERT_DOUBLE_WITHIN(10000.0, 2222000.0, d);  // Within 10km tolerance
}

void test_haversine_across_prime_meridian(void) {
    // From 10W to 10E at equator
    // 20 degrees longitude at equator ~= 2222 km
    double d = haversineMeters(0.0, -10.0, 0.0, 10.0);
    ASSERT_DOUBLE_WITHIN(10000.0, 2222000.0, d);
}

void test_haversine_across_international_date_line(void) {
    // This is a tricky case - crossing 180 degrees
    // From 170E to 170W (20 degree gap via date line)
    double d = haversineMeters(0.0, 170.0, 0.0, -170.0);
    // At equator, 20 degrees = ~2222 km
    ASSERT_DOUBLE_WITHIN(10000.0, 2222000.0, d);
}

void test_haversine_north_pole_region(void) {
    // Near north pole - longitude converges
    // Two points at 89N but 90 degrees apart in longitude
    double d = haversineMeters(89.0, 0.0, 89.0, 90.0);
    // At 89N, the distance should be much less than at equator
    TEST_ASSERT_LESS_THAN_DOUBLE(500000.0, d);  // Less than 500km
}

void test_haversine_south_pole_region(void) {
    // Near south pole
    double d = haversineMeters(-89.0, 0.0, -89.0, 90.0);
    TEST_ASSERT_LESS_THAN_DOUBLE(500000.0, d);
}

void test_haversine_antipodal_points(void) {
    // Opposite sides of Earth (should be ~20000 km, half circumference)
    double d = haversineMeters(0.0, 0.0, 0.0, 180.0);
    ASSERT_DOUBLE_WITHIN(100000.0, 20015000.0, d);  // Half of Earth's circumference
}

void test_haversine_symmetry(void) {
    // Distance A->B should equal B->A
    double d1 = haversineMeters(51.5074, -0.1278, 48.8566, 2.3522);
    double d2 = haversineMeters(48.8566, 2.3522, 51.5074, -0.1278);
    ASSERT_DOUBLE_WITHIN(0.001, d1, d2);
}

void test_haversine_negative_latitudes(void) {
    // Sydney (-33.8688, 151.2093) to Melbourne (-37.8136, 144.9631)
    // Known distance: ~714 km
    double d = haversineMeters(-33.8688, 151.2093, -37.8136, 144.9631);
    ASSERT_DOUBLE_WITHIN(20000.0, 714000.0, d);
}

void test_haversine_walking_distance(void) {
    // Simulate ~1km walk (0.009 degrees latitude)
    double d = haversineMeters(40.0, -74.0, 40.009, -74.0);
    ASSERT_DOUBLE_WITHIN(50.0, 1000.0, d);  // Within 50m of 1km
}

void test_haversine_wardriving_typical_update(void) {
    // Typical GPS update while walking - ~10 meters
    double d = haversineMeters(40.0, -74.0, 40.00009, -74.0);
    ASSERT_DOUBLE_WITHIN(5.0, 10.0, d);  // Within 5m of 10m
}

// ============================================================================
// Edge cases
// ============================================================================

void test_haversine_zero_coordinates(void) {
    // Point at 0,0 to 1,1
    double d = haversineMeters(0.0, 0.0, 1.0, 1.0);
    // Diagonal of ~157km
    ASSERT_DOUBLE_WITHIN(5000.0, 157000.0, d);
}

void test_haversine_max_latitude(void) {
    // Poles
    double d = haversineMeters(90.0, 0.0, -90.0, 0.0);
    // Pole to pole = half circumference through center
    ASSERT_DOUBLE_WITHIN(100000.0, 20015000.0, d);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    RUN_TEST(test_haversine_same_point_returns_zero);
    RUN_TEST(test_haversine_very_close_points);
    RUN_TEST(test_haversine_known_distance_london_to_paris);
    RUN_TEST(test_haversine_known_distance_nyc_to_la);
    RUN_TEST(test_haversine_across_equator);
    RUN_TEST(test_haversine_across_prime_meridian);
    RUN_TEST(test_haversine_across_international_date_line);
    RUN_TEST(test_haversine_north_pole_region);
    RUN_TEST(test_haversine_south_pole_region);
    RUN_TEST(test_haversine_antipodal_points);
    RUN_TEST(test_haversine_symmetry);
    RUN_TEST(test_haversine_negative_latitudes);
    RUN_TEST(test_haversine_walking_distance);
    RUN_TEST(test_haversine_wardriving_typical_update);
    RUN_TEST(test_haversine_zero_coordinates);
    RUN_TEST(test_haversine_max_latitude);
    
    return UNITY_END();
}
