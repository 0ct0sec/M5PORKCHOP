// Unit tests for pure OINK capture filters / gating logic.

#include <unity.h>

#include "../../src/modes/oink_capture_filters.h"

void setUp(void) {}
void tearDown(void) {}

void test_shouldMarkDeauthSuccessOnM1_blocksStationIsOurs(void) {
    TEST_ASSERT_FALSE(OinkCaptureFilters::shouldMarkDeauthSuccessOnM1(true, true));
    TEST_ASSERT_FALSE(OinkCaptureFilters::shouldMarkDeauthSuccessOnM1(true, false));
}

void test_shouldMarkDeauthSuccessOnM1_requiresUnicastStation(void) {
    TEST_ASSERT_TRUE(OinkCaptureFilters::shouldMarkDeauthSuccessOnM1(false, true));
    TEST_ASSERT_FALSE(OinkCaptureFilters::shouldMarkDeauthSuccessOnM1(false, false));
}

void test_shouldStoreHandshakeForStation_blocksStationIsOurs(void) {
    TEST_ASSERT_FALSE(OinkCaptureFilters::shouldStoreHandshakeForStation(true));
    TEST_ASSERT_TRUE(OinkCaptureFilters::shouldStoreHandshakeForStation(false));
}

void test_evaluateHandshakeCreateGate_allowsCreate_whenNoGrow_evenWarning_andLowHeap(void) {
    const auto r = OinkCaptureFilters::evaluateHandshakeCreateGate(
        /*size=*/0, /*capacity=*/5, /*maxHandshakes=*/50,
        /*pressure=*/HeapPressureLevel::Warning,
        /*freeHeap=*/40000, /*minFreeHeap=*/60000,
        /*largestBlock=*/1000, /*minAllocBlock=*/4096);

    TEST_ASSERT_TRUE(r.allowCreate);
    TEST_ASSERT_FALSE(r.allowBeaconCopy);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OinkCaptureFilters::HandshakeCreateBlockReason::None,
                            (uint8_t)r.blockReason);
}

void test_evaluateHandshakeCreateGate_blocksOnPressure_whenNeedGrow(void) {
    const auto r = OinkCaptureFilters::evaluateHandshakeCreateGate(
        /*size=*/5, /*capacity=*/5, /*maxHandshakes=*/50,
        /*pressure=*/HeapPressureLevel::Warning,
        /*freeHeap=*/100000, /*minFreeHeap=*/60000,
        /*largestBlock=*/100000, /*minAllocBlock=*/4096);

    TEST_ASSERT_FALSE(r.allowCreate);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OinkCaptureFilters::HandshakeCreateBlockReason::Pressure,
                            (uint8_t)r.blockReason);
}

void test_evaluateHandshakeCreateGate_blocksOnFreeHeap_whenNeedGrow(void) {
    const auto r = OinkCaptureFilters::evaluateHandshakeCreateGate(
        /*size=*/5, /*capacity=*/5, /*maxHandshakes=*/50,
        /*pressure=*/HeapPressureLevel::Caution,
        /*freeHeap=*/59000, /*minFreeHeap=*/60000,
        /*largestBlock=*/100000, /*minAllocBlock=*/4096);

    TEST_ASSERT_FALSE(r.allowCreate);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OinkCaptureFilters::HandshakeCreateBlockReason::FreeHeap,
                            (uint8_t)r.blockReason);
}

void test_evaluateHandshakeCreateGate_blocksOnFragmentation_whenNeedGrow(void) {
    const auto r = OinkCaptureFilters::evaluateHandshakeCreateGate(
        /*size=*/5, /*capacity=*/5, /*maxHandshakes=*/50,
        /*pressure=*/HeapPressureLevel::Caution,
        /*freeHeap=*/100000, /*minFreeHeap=*/60000,
        /*largestBlock=*/4095, /*minAllocBlock=*/4096);

    TEST_ASSERT_FALSE(r.allowCreate);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OinkCaptureFilters::HandshakeCreateBlockReason::Fragmentation,
                            (uint8_t)r.blockReason);
}

void test_evaluateHandshakeCreateGate_blocksOnMaxHandshakes(void) {
    const auto r = OinkCaptureFilters::evaluateHandshakeCreateGate(
        /*size=*/50, /*capacity=*/50, /*maxHandshakes=*/50,
        /*pressure=*/HeapPressureLevel::Normal,
        /*freeHeap=*/100000, /*minFreeHeap=*/60000,
        /*largestBlock=*/100000, /*minAllocBlock=*/4096);

    TEST_ASSERT_FALSE(r.allowCreate);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OinkCaptureFilters::HandshakeCreateBlockReason::MaxHandshakes,
                            (uint8_t)r.blockReason);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_shouldMarkDeauthSuccessOnM1_blocksStationIsOurs);
    RUN_TEST(test_shouldMarkDeauthSuccessOnM1_requiresUnicastStation);
    RUN_TEST(test_shouldStoreHandshakeForStation_blocksStationIsOurs);

    RUN_TEST(test_evaluateHandshakeCreateGate_allowsCreate_whenNoGrow_evenWarning_andLowHeap);
    RUN_TEST(test_evaluateHandshakeCreateGate_blocksOnPressure_whenNeedGrow);
    RUN_TEST(test_evaluateHandshakeCreateGate_blocksOnFreeHeap_whenNeedGrow);
    RUN_TEST(test_evaluateHandshakeCreateGate_blocksOnFragmentation_whenNeedGrow);
    RUN_TEST(test_evaluateHandshakeCreateGate_blocksOnMaxHandshakes);

    return UNITY_END();
}

