// Test C5 UART marker detection for JanusHog handshake lifecycle.
// Keeps host parsing aligned with external/projectZero firmware output.

#include <unity.h>

#include "../../src/core/c5_uart_markers.h"

void setUp(void) {}
void tearDown(void) {}

void test_isHandshakeCapturedLine_detectsSubstring(void) {
    TEST_ASSERT_TRUE(C5UartMarkers::isHandshakeCapturedLine("Handshake captured for 'TEST' after burst #1!"));
    TEST_ASSERT_TRUE(C5UartMarkers::isHandshakeCapturedLine("[INFO] Handshake captured for 'TEST' after burst #1!"));
    TEST_ASSERT_FALSE(C5UartMarkers::isHandshakeCapturedLine("No handshake here"));
}

void test_isHandshakeCompleteLine_legacy(void) {
    TEST_ASSERT_TRUE(C5UartMarkers::isHandshakeCompleteLine("Handshake attack completed for all targets."));
}

void test_isHandshakeCompleteLine_projectZero_taskFinished(void) {
    TEST_ASSERT_TRUE(C5UartMarkers::isHandshakeCompleteLine("Handshake attack task finished."));
}

void test_isHandshakeCompleteLine_projectZero_cleanupComplete(void) {
    TEST_ASSERT_TRUE(C5UartMarkers::isHandshakeCompleteLine("Handshake attack cleanup complete."));
}

void test_isHandshakeCompleteLine_projectZero_attackComplete_variants(void) {
    TEST_ASSERT_TRUE(C5UartMarkers::isHandshakeCompleteLine("All selected networks have been captured! Attack complete."));
    TEST_ASSERT_TRUE(C5UartMarkers::isHandshakeCompleteLine("All selected networks captured! Attack complete."));
}

void test_isHandshakeCompleteLine_doesNotMatchOtherAttacks(void) {
    TEST_ASSERT_FALSE(C5UartMarkers::isHandshakeCompleteLine("Blackout attack task finished."));
    TEST_ASSERT_FALSE(C5UartMarkers::isHandshakeCompleteLine("Deauth attack task finished."));
}

enum class TestHsResult : uint8_t {
    IN_PROGRESS,
    CAPTURED,
    FAILED,
};

static void applyHandshakeLine(const char* line, TestHsResult* r, bool* done) {
    if (!r || !done) return;

    if (C5UartMarkers::isHandshakeCapturedLine(line)) {
        *r = TestHsResult::CAPTURED;
    }
    if (C5UartMarkers::isHandshakeCompleteLine(line)) {
        if (*r == TestHsResult::IN_PROGRESS) {
            *r = TestHsResult::FAILED;
        }
        *done = true;
    }
}

void test_logReplay_success_captureThenFinish(void) {
    TestHsResult r = TestHsResult::IN_PROGRESS;
    bool done = false;

    applyHandshakeLine("Handshake captured for 'MySSID' after burst #2!", &r, &done);
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)TestHsResult::CAPTURED, (uint8_t)r);

    applyHandshakeLine("Handshake attack task finished.", &r, &done);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)TestHsResult::CAPTURED, (uint8_t)r);
}

void test_logReplay_skipExistingFile_thenFinish_countsAsFailed(void) {
    TestHsResult r = TestHsResult::IN_PROGRESS;
    bool done = false;

    applyHandshakeLine("[1/1] Skipping 'MySSID' - PCAP already exists", &r, &done);
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)TestHsResult::IN_PROGRESS, (uint8_t)r);

    applyHandshakeLine("Handshake attack task finished.", &r, &done);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)TestHsResult::FAILED, (uint8_t)r);
}

void test_logReplay_failure_noCapture_thenFinish_failed(void) {
    TestHsResult r = TestHsResult::IN_PROGRESS;
    bool done = false;

    applyHandshakeLine("Burst #1 complete, trying next...", &r, &done);
    applyHandshakeLine("No handshake for 'MySSID' after 3 bursts", &r, &done);
    TEST_ASSERT_FALSE(done);

    applyHandshakeLine("Handshake attack task finished.", &r, &done);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)TestHsResult::FAILED, (uint8_t)r);
}

void test_logReplay_timeout_noCompletionMarker_notDone(void) {
    TestHsResult r = TestHsResult::IN_PROGRESS;
    bool done = false;

    applyHandshakeLine("Handshake attack task started.", &r, &done);
    applyHandshakeLine("Burst attacking 'MySSID' (Ch 36, RSSI: -42 dBm)", &r, &done);

    // No completion marker in this log snippet -> lifecycle remains not-done here.
    TEST_ASSERT_FALSE(done);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_isHandshakeCapturedLine_detectsSubstring);
    RUN_TEST(test_isHandshakeCompleteLine_legacy);
    RUN_TEST(test_isHandshakeCompleteLine_projectZero_taskFinished);
    RUN_TEST(test_isHandshakeCompleteLine_projectZero_cleanupComplete);
    RUN_TEST(test_isHandshakeCompleteLine_projectZero_attackComplete_variants);
    RUN_TEST(test_isHandshakeCompleteLine_doesNotMatchOtherAttacks);

    RUN_TEST(test_logReplay_success_captureThenFinish);
    RUN_TEST(test_logReplay_skipExistingFile_thenFinish_countsAsFailed);
    RUN_TEST(test_logReplay_failure_noCapture_thenFinish_failed);
    RUN_TEST(test_logReplay_timeout_noCompletionMarker_notDone);

    return UNITY_END();
}

