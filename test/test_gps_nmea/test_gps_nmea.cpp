// GPS NMEA checksum, quality gating, and ring buffer tests
// Pure logic — no hardware dependencies

#include <unity.h>
#include <cstdint>
#include <cstring>

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// NMEA Checksum Validation (extracted from gps.cpp for testability)
// ============================================================================

static bool validateNmeaChecksum(const char* sentence, uint8_t len) {
    if (len < 7) return false;
    if (sentence[0] != '$') return false;

    int starPos = -1;
    for (int i = 1; i < len; i++) {
        if (sentence[i] == '*') {
            starPos = i;
            break;
        }
    }
    if (starPos < 0 || starPos + 2 >= len) return false;

    uint8_t computed = 0;
    for (int i = 1; i < starPos; i++) {
        computed ^= (uint8_t)sentence[i];
    }

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int hi = hexVal(sentence[starPos + 1]);
    int lo = hexVal(sentence[starPos + 2]);
    if (hi < 0 || lo < 0) return false;

    uint8_t expected = (uint8_t)((hi << 4) | lo);
    return computed == expected;
}

// ============================================================================
// Quality gating logic (extracted from gps_quality.h)
// ============================================================================

namespace GPSQuality {
    constexpr uint8_t  MIN_SATELLITES = 3;
    constexpr uint16_t MAX_HDOP       = 800;
    constexpr uint8_t  COAST_MIN_SATS = 5;
    constexpr uint16_t COAST_MAX_HDOP = 500;

    inline bool isFixAcceptable(uint8_t satellites, uint16_t hdop) {
        return (satellites >= MIN_SATELLITES) && (hdop > 0) && (hdop <= MAX_HDOP);
    }

    inline bool isCoastWorthy(uint8_t satellites, uint16_t hdop) {
        return (satellites >= COAST_MIN_SATS) && (hdop > 0) && (hdop <= COAST_MAX_HDOP);
    }
}

// ============================================================================
// Ring buffer logic (extracted for testing)
// ============================================================================

static constexpr uint16_t NMEA_BUF_SIZE = 640;
static char nmeaBuf[NMEA_BUF_SIZE];
static uint16_t nmeaHead = 0;
static uint16_t nmeaTail = 0;

static inline uint16_t nmeaUsed() {
    return (nmeaHead >= nmeaTail)
        ? (nmeaHead - nmeaTail)
        : (NMEA_BUF_SIZE - nmeaTail + nmeaHead);
}

static inline uint16_t nmeaFree() {
    return NMEA_BUF_SIZE - 1 - nmeaUsed();
}

static void nmeaReset() {
    nmeaHead = nmeaTail = 0;
}

static bool nmeaPush(char c) {
    if (nmeaFree() == 0) return false;
    nmeaBuf[nmeaHead] = c;
    nmeaHead = (nmeaHead + 1) % NMEA_BUF_SIZE;
    return true;
}

static char nmeaPop() {
    if (nmeaUsed() == 0) return '\0';
    char c = nmeaBuf[nmeaTail];
    nmeaTail = (nmeaTail + 1) % NMEA_BUF_SIZE;
    return c;
}

// ============================================================================
// Checksum Tests
// ============================================================================

void test_checksum_valid_gpgga(void) {
    // Real GPGGA sentence: XOR of bytes between $ and * = 0x56
    const char* s = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*47";
    TEST_ASSERT_TRUE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_valid_gprmc(void) {
    // $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
    const char* s = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    TEST_ASSERT_TRUE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_lowercase_hex(void) {
    // Same sentence but lowercase hex
    const char* s = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6a";
    TEST_ASSERT_TRUE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_corrupted_data(void) {
    // Flip a byte — checksum should fail
    const char* s = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*48";
    TEST_ASSERT_FALSE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_missing_asterisk(void) {
    const char* s = "$GPGGA,123519,4807.038,N47";
    TEST_ASSERT_FALSE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_too_short(void) {
    const char* s = "$G*00";
    TEST_ASSERT_FALSE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_no_dollar(void) {
    const char* s = "GPGGA,123519*47";
    TEST_ASSERT_FALSE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_invalid_hex(void) {
    const char* s = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*XY";
    TEST_ASSERT_FALSE(validateNmeaChecksum(s, strlen(s)));
}

void test_checksum_empty_payload(void) {
    // $*00 — empty payload, XOR of nothing = 0x00
    const char* s = "$*00";
    TEST_ASSERT_FALSE(validateNmeaChecksum(s, strlen(s))); // Too short (<7)
}

void test_checksum_minimal_valid(void) {
    // Minimal: $XXXXX*HH (7+ chars with valid XOR)
    // Payload "ABCDE" XOR = 'A'^'B'^'C'^'D'^'E' = 0x41^0x42^0x43^0x44^0x45 = 0x01
    const char* s = "$ABCDE*01";
    TEST_ASSERT_TRUE(validateNmeaChecksum(s, strlen(s)));
}

// ============================================================================
// Quality Gating Tests
// ============================================================================

void test_quality_acceptable_good_fix(void) {
    TEST_ASSERT_TRUE(GPSQuality::isFixAcceptable(8, 120));
}

void test_quality_acceptable_minimum(void) {
    TEST_ASSERT_TRUE(GPSQuality::isFixAcceptable(3, 800));
}

void test_quality_reject_too_few_sats(void) {
    TEST_ASSERT_FALSE(GPSQuality::isFixAcceptable(2, 120));
}

void test_quality_reject_zero_sats(void) {
    TEST_ASSERT_FALSE(GPSQuality::isFixAcceptable(0, 120));
}

void test_quality_reject_hdop_too_high(void) {
    TEST_ASSERT_FALSE(GPSQuality::isFixAcceptable(8, 801));
}

void test_quality_reject_hdop_zero(void) {
    TEST_ASSERT_FALSE(GPSQuality::isFixAcceptable(8, 0));
}

void test_quality_reject_both_bad(void) {
    TEST_ASSERT_FALSE(GPSQuality::isFixAcceptable(2, 900));
}

void test_coast_worthy_good(void) {
    TEST_ASSERT_TRUE(GPSQuality::isCoastWorthy(8, 150));
}

void test_coast_worthy_minimum(void) {
    TEST_ASSERT_TRUE(GPSQuality::isCoastWorthy(5, 500));
}

void test_coast_worthy_reject_few_sats(void) {
    TEST_ASSERT_FALSE(GPSQuality::isCoastWorthy(4, 150));
}

void test_coast_worthy_reject_high_hdop(void) {
    TEST_ASSERT_FALSE(GPSQuality::isCoastWorthy(8, 501));
}

void test_coast_worthy_reject_zero_hdop(void) {
    TEST_ASSERT_FALSE(GPSQuality::isCoastWorthy(8, 0));
}

// ============================================================================
// Ring Buffer Tests
// ============================================================================

void test_ring_buffer_empty(void) {
    nmeaReset();
    TEST_ASSERT_EQUAL_UINT16(0, nmeaUsed());
    TEST_ASSERT_EQUAL_UINT16(NMEA_BUF_SIZE - 1, nmeaFree());
}

void test_ring_buffer_push_pop(void) {
    nmeaReset();
    TEST_ASSERT_TRUE(nmeaPush('A'));
    TEST_ASSERT_TRUE(nmeaPush('B'));
    TEST_ASSERT_EQUAL_UINT16(2, nmeaUsed());
    TEST_ASSERT_EQUAL('A', nmeaPop());
    TEST_ASSERT_EQUAL('B', nmeaPop());
    TEST_ASSERT_EQUAL_UINT16(0, nmeaUsed());
}

void test_ring_buffer_fill_to_capacity(void) {
    nmeaReset();
    // Fill to max (size - 1)
    for (uint16_t i = 0; i < NMEA_BUF_SIZE - 1; i++) {
        TEST_ASSERT_TRUE(nmeaPush('X'));
    }
    TEST_ASSERT_EQUAL_UINT16(0, nmeaFree());
    // One more should fail
    TEST_ASSERT_FALSE(nmeaPush('Y'));
}

void test_ring_buffer_wrap_around(void) {
    nmeaReset();
    // Push half the buffer, pop it, then push again to wrap
    uint16_t half = NMEA_BUF_SIZE / 2;
    for (uint16_t i = 0; i < half; i++) {
        nmeaPush((char)(i & 0x7F));
    }
    for (uint16_t i = 0; i < half; i++) {
        nmeaPop();
    }
    TEST_ASSERT_EQUAL_UINT16(0, nmeaUsed());

    // Now push enough to wrap
    for (uint16_t i = 0; i < NMEA_BUF_SIZE - 1; i++) {
        TEST_ASSERT_TRUE(nmeaPush((char)('A' + (i % 26))));
    }
    TEST_ASSERT_EQUAL_UINT16(NMEA_BUF_SIZE - 1, nmeaUsed());

    // Pop all and verify
    for (uint16_t i = 0; i < NMEA_BUF_SIZE - 1; i++) {
        char c = nmeaPop();
        TEST_ASSERT_EQUAL_CHAR((char)('A' + (i % 26)), c);
    }
    TEST_ASSERT_EQUAL_UINT16(0, nmeaUsed());
}

void test_ring_buffer_interleaved(void) {
    nmeaReset();
    // Interleave push/pop
    nmeaPush('1');
    nmeaPush('2');
    TEST_ASSERT_EQUAL('1', nmeaPop());
    nmeaPush('3');
    TEST_ASSERT_EQUAL('2', nmeaPop());
    TEST_ASSERT_EQUAL('3', nmeaPop());
    TEST_ASSERT_EQUAL_UINT16(0, nmeaUsed());
}

// ============================================================================
// Sentence Extraction (integration-level logic test)
// ============================================================================

// Simulates extracting one sentence from a buffer of bytes
void test_sentence_extraction_basic(void) {
    // Feed "$GPGGA,data*CS\r\n" and verify we get just the sentence
    const char* raw = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*47\r\n";

    // Sentence assembly
    char sentence[96];
    uint8_t sLen = 0;
    bool inSentence = false;
    bool gotSentence = false;

    for (size_t i = 0; raw[i] != '\0'; i++) {
        char c = raw[i];
        if (c == '$') {
            sentence[0] = '$';
            sLen = 1;
            inSentence = true;
            continue;
        }
        if (!inSentence) continue;
        if (c == '\r' || c == '\n') {
            if (sLen >= 6) {
                sentence[sLen] = '\0';
                gotSentence = true;
            }
            inSentence = false;
            break;
        }
        if (sLen < 95) {
            sentence[sLen++] = c;
        }
    }

    TEST_ASSERT_TRUE(gotSentence);
    TEST_ASSERT_TRUE(validateNmeaChecksum(sentence, sLen));
}

void test_sentence_extraction_rejects_overlong(void) {
    // Build a sentence >96 chars
    char huge[120];
    huge[0] = '$';
    for (int i = 1; i < 110; i++) huge[i] = 'A';
    huge[110] = '*';
    huge[111] = '0';
    huge[112] = '0';
    huge[113] = '\r';
    huge[114] = '\n';
    huge[115] = '\0';

    char sentence[96];
    uint8_t sLen = 0;
    bool inSentence = false;
    bool overlong = false;

    for (size_t i = 0; huge[i] != '\0'; i++) {
        char c = huge[i];
        if (c == '$') {
            sentence[0] = '$';
            sLen = 1;
            inSentence = true;
            continue;
        }
        if (!inSentence) continue;
        if (c == '\r' || c == '\n') {
            inSentence = false;
            break;
        }
        if (sLen < 95) {
            sentence[sLen++] = c;
        } else {
            overlong = true;
            inSentence = false;
            break;
        }
    }

    TEST_ASSERT_TRUE(overlong);
}

void test_sentence_extraction_multiple_in_stream(void) {
    // Two sentences back to back
    const char* stream =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*47\r\n"
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";

    char sentence[96];
    uint8_t sLen = 0;
    bool inSentence = false;
    int validCount = 0;

    for (size_t i = 0; stream[i] != '\0'; i++) {
        char c = stream[i];
        if (c == '$') {
            sentence[0] = '$';
            sLen = 1;
            inSentence = true;
            continue;
        }
        if (!inSentence) continue;
        if (c == '\r' || c == '\n') {
            if (sLen >= 6) {
                sentence[sLen] = '\0';
                if (validateNmeaChecksum(sentence, sLen)) {
                    validCount++;
                }
            }
            inSentence = false;
            sLen = 0;
            continue;
        }
        if (sLen < 95) {
            sentence[sLen++] = c;
        } else {
            inSentence = false;
            sLen = 0;
        }
    }

    TEST_ASSERT_EQUAL_INT(2, validCount);
}

// ============================================================================
// Test Runner
// ============================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Checksum tests
    RUN_TEST(test_checksum_valid_gpgga);
    RUN_TEST(test_checksum_valid_gprmc);
    RUN_TEST(test_checksum_lowercase_hex);
    RUN_TEST(test_checksum_corrupted_data);
    RUN_TEST(test_checksum_missing_asterisk);
    RUN_TEST(test_checksum_too_short);
    RUN_TEST(test_checksum_no_dollar);
    RUN_TEST(test_checksum_invalid_hex);
    RUN_TEST(test_checksum_empty_payload);
    RUN_TEST(test_checksum_minimal_valid);

    // Quality gating tests
    RUN_TEST(test_quality_acceptable_good_fix);
    RUN_TEST(test_quality_acceptable_minimum);
    RUN_TEST(test_quality_reject_too_few_sats);
    RUN_TEST(test_quality_reject_zero_sats);
    RUN_TEST(test_quality_reject_hdop_too_high);
    RUN_TEST(test_quality_reject_hdop_zero);
    RUN_TEST(test_quality_reject_both_bad);
    RUN_TEST(test_coast_worthy_good);
    RUN_TEST(test_coast_worthy_minimum);
    RUN_TEST(test_coast_worthy_reject_few_sats);
    RUN_TEST(test_coast_worthy_reject_high_hdop);
    RUN_TEST(test_coast_worthy_reject_zero_hdop);

    // Ring buffer tests
    RUN_TEST(test_ring_buffer_empty);
    RUN_TEST(test_ring_buffer_push_pop);
    RUN_TEST(test_ring_buffer_fill_to_capacity);
    RUN_TEST(test_ring_buffer_wrap_around);
    RUN_TEST(test_ring_buffer_interleaved);

    // Sentence extraction tests
    RUN_TEST(test_sentence_extraction_basic);
    RUN_TEST(test_sentence_extraction_rejects_overlong);
    RUN_TEST(test_sentence_extraction_multiple_in_stream);

    return UNITY_END();
}
