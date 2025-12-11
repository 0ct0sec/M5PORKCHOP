// String Escaping Tests
// Tests XML and CSV escaping functions for safe data export

#include <unity.h>
#include <cstring>
#include "../mocks/testable_functions.h"

void setUp(void) {
    // No setup needed
}

void tearDown(void) {
    // No teardown needed
}

// ============================================================================
// escapeXMLChar() tests
// ============================================================================

void test_escapeXMLChar_ampersand(void) {
    const char* result = escapeXMLChar('&');
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("&amp;", result);
}

void test_escapeXMLChar_less_than(void) {
    const char* result = escapeXMLChar('<');
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("&lt;", result);
}

void test_escapeXMLChar_greater_than(void) {
    const char* result = escapeXMLChar('>');
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("&gt;", result);
}

void test_escapeXMLChar_double_quote(void) {
    const char* result = escapeXMLChar('"');
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("&quot;", result);
}

void test_escapeXMLChar_single_quote(void) {
    const char* result = escapeXMLChar('\'');
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("&apos;", result);
}

void test_escapeXMLChar_normal_char(void) {
    TEST_ASSERT_NULL(escapeXMLChar('A'));
    TEST_ASSERT_NULL(escapeXMLChar('z'));
    TEST_ASSERT_NULL(escapeXMLChar('0'));
    TEST_ASSERT_NULL(escapeXMLChar(' '));
    TEST_ASSERT_NULL(escapeXMLChar('_'));
}

// ============================================================================
// needsXMLEscape() tests
// ============================================================================

void test_needsXMLEscape_special_chars(void) {
    TEST_ASSERT_TRUE(needsXMLEscape('&'));
    TEST_ASSERT_TRUE(needsXMLEscape('<'));
    TEST_ASSERT_TRUE(needsXMLEscape('>'));
    TEST_ASSERT_TRUE(needsXMLEscape('"'));
    TEST_ASSERT_TRUE(needsXMLEscape('\''));
}

void test_needsXMLEscape_normal_chars(void) {
    TEST_ASSERT_FALSE(needsXMLEscape('A'));
    TEST_ASSERT_FALSE(needsXMLEscape('z'));
    TEST_ASSERT_FALSE(needsXMLEscape('0'));
    TEST_ASSERT_FALSE(needsXMLEscape(' '));
    TEST_ASSERT_FALSE(needsXMLEscape('_'));
    TEST_ASSERT_FALSE(needsXMLEscape('-'));
}

// ============================================================================
// escapeXML() tests
// ============================================================================

void test_escapeXML_normal_string(void) {
    char output[64];
    const char* input = "TestNetwork";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("TestNetwork", output);
    TEST_ASSERT_EQUAL_UINT(11, len);
}

void test_escapeXML_with_ampersand(void) {
    char output[64];
    const char* input = "AT&T WiFi";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("AT&amp;T WiFi", output);
    TEST_ASSERT_EQUAL_UINT(13, len);
}

void test_escapeXML_with_less_than(void) {
    char output[64];
    const char* input = "Net<work";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("Net&lt;work", output);
    TEST_ASSERT_EQUAL_UINT(11, len);
}

void test_escapeXML_with_greater_than(void) {
    char output[64];
    const char* input = "Net>work";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("Net&gt;work", output);
    TEST_ASSERT_EQUAL_UINT(11, len);
}

void test_escapeXML_with_quotes(void) {
    char output[64];
    const char* input = "Net\"work";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("Net&quot;work", output);
    TEST_ASSERT_EQUAL_UINT(13, len);
}

void test_escapeXML_with_single_quote(void) {
    char output[64];
    const char* input = "Net'work";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("Net&apos;work", output);
    TEST_ASSERT_EQUAL_UINT(13, len);
}

void test_escapeXML_multiple_special_chars(void) {
    char output[128];
    const char* input = "<tag attr=\"value\">";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("&lt;tag attr=&quot;value&quot;&gt;", output);
    TEST_ASSERT_EQUAL_UINT(34, len);
}

void test_escapeXML_empty_string(void) {
    char output[64];
    const char* input = "";
    size_t len = escapeXML(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("", output);
    TEST_ASSERT_EQUAL_UINT(0, len);
}

void test_escapeXML_null_input(void) {
    char output[64];
    size_t len = escapeXML(nullptr, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_UINT(0, len);
}

void test_escapeXML_with_maxInputLen(void) {
    char output[64];
    const char* input = "Test&Network";
    // Only process first 4 chars "Test"
    size_t len = escapeXML(input, output, 4, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("Test", output);
    TEST_ASSERT_EQUAL_UINT(4, len);
}

void test_escapeXML_buffer_size_calculation(void) {
    const char* input = "<>&\"'";
    // Calculate required size without output buffer
    size_t required = escapeXML(input, nullptr, 0, 0);
    // Each char expands: < -> &lt; (4), > -> &gt; (4), & -> &amp; (5), " -> &quot; (6), ' -> &apos; (6)
    TEST_ASSERT_EQUAL_UINT(25, required);
}

void test_escapeXML_buffer_too_small(void) {
    char output[10];
    const char* input = "Test&Network";  // & expands to &amp; (5 chars)
    size_t len = escapeXML(input, output, 0, sizeof(output));
    // Should stop before overflow - "Test&amp" would be 8 chars + null
    TEST_ASSERT_TRUE(len < sizeof(output));
    TEST_ASSERT_TRUE(strlen(output) < sizeof(output));
}

void test_escapeXML_script_injection(void) {
    char output[128];
    const char* input = "<script>alert('xss')</script>";
    escapeXML(input, output, 0, sizeof(output));
    // Should not contain any < or > characters
    TEST_ASSERT_NULL(strchr(output, '<'));
    TEST_ASSERT_NULL(strchr(output, '>'));
    // Should contain escaped versions
    TEST_ASSERT_NOT_NULL(strstr(output, "&lt;"));
    TEST_ASSERT_NOT_NULL(strstr(output, "&gt;"));
}

// ============================================================================
// needsCSVQuoting() tests
// ============================================================================

void test_needsCSVQuoting_normal_string(void) {
    TEST_ASSERT_FALSE(needsCSVQuoting("TestNetwork"));
    TEST_ASSERT_FALSE(needsCSVQuoting("MyHomeWiFi"));
    TEST_ASSERT_FALSE(needsCSVQuoting("Network_2.4GHz"));
}

void test_needsCSVQuoting_with_comma(void) {
    TEST_ASSERT_TRUE(needsCSVQuoting("Network,Name"));
}

void test_needsCSVQuoting_with_quote(void) {
    TEST_ASSERT_TRUE(needsCSVQuoting("Network\"Name"));
}

void test_needsCSVQuoting_with_newline(void) {
    TEST_ASSERT_TRUE(needsCSVQuoting("Network\nName"));
}

void test_needsCSVQuoting_with_cr(void) {
    TEST_ASSERT_TRUE(needsCSVQuoting("Network\rName"));
}

void test_needsCSVQuoting_null_input(void) {
    TEST_ASSERT_FALSE(needsCSVQuoting(nullptr));
}

void test_needsCSVQuoting_empty_string(void) {
    TEST_ASSERT_FALSE(needsCSVQuoting(""));
}

// ============================================================================
// isCSVControlChar() tests
// ============================================================================

void test_isCSVControlChar_control_chars(void) {
    TEST_ASSERT_TRUE(isCSVControlChar('\n'));   // 10
    TEST_ASSERT_TRUE(isCSVControlChar('\r'));   // 13
    TEST_ASSERT_TRUE(isCSVControlChar('\t'));   // 9
    TEST_ASSERT_TRUE(isCSVControlChar(1));      // SOH
    TEST_ASSERT_TRUE(isCSVControlChar(31));     // Unit separator
}

void test_isCSVControlChar_null_is_not_control(void) {
    // Null is special - used as terminator, not stripped
    TEST_ASSERT_FALSE(isCSVControlChar('\0'));
}

void test_isCSVControlChar_printable_chars(void) {
    TEST_ASSERT_FALSE(isCSVControlChar(' '));   // 32
    TEST_ASSERT_FALSE(isCSVControlChar('A'));
    TEST_ASSERT_FALSE(isCSVControlChar('~'));   // 126
}

// ============================================================================
// escapeCSV() tests
// ============================================================================

void test_escapeCSV_normal_string(void) {
    char output[64];
    const char* input = "TestNetwork";
    size_t len = escapeCSV(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"TestNetwork\"", output);
    TEST_ASSERT_EQUAL_UINT(13, len);  // 11 chars + 2 quotes
}

void test_escapeCSV_with_quote(void) {
    char output[64];
    const char* input = "Net\"work";
    size_t len = escapeCSV(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"Net\"\"work\"", output);  // Quote doubled
    TEST_ASSERT_EQUAL_UINT(12, len);  // 8 chars + 2 for doubled quote + 2 outer quotes
}

void test_escapeCSV_with_multiple_quotes(void) {
    char output[64];
    const char* input = "\"test\"";
    size_t len = escapeCSV(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"\"\"test\"\"\"", output);  // Each quote doubled
    TEST_ASSERT_EQUAL_UINT(12, len);
}

void test_escapeCSV_strips_control_chars(void) {
    char output[64];
    const char* input = "Net\nwork";  // Newline should be stripped
    escapeCSV(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"Network\"", output);  // Newline removed
}

void test_escapeCSV_strips_tab(void) {
    char output[64];
    const char* input = "Net\twork";  // Tab should be stripped
    escapeCSV(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"Network\"", output);  // Tab removed
}

void test_escapeCSV_preserves_comma(void) {
    char output[64];
    const char* input = "Net,work";
    escapeCSV(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"Net,work\"", output);  // Comma preserved, wrapped in quotes
}

void test_escapeCSV_empty_string(void) {
    char output[64];
    const char* input = "";
    size_t len = escapeCSV(input, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"\"", output);  // Empty quoted field
    TEST_ASSERT_EQUAL_UINT(2, len);
}

void test_escapeCSV_null_input(void) {
    char output[64];
    size_t len = escapeCSV(nullptr, output, 0, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("\"\"", output);  // Empty quoted field
    TEST_ASSERT_EQUAL_UINT(2, len);
}

void test_escapeCSV_max_ssid_length(void) {
    char output[128];
    const char* input = "12345678901234567890123456789012";  // Exactly 32 chars
    size_t len = escapeCSV(input, output, 32, sizeof(output));
    TEST_ASSERT_EQUAL_UINT(34, len);  // 32 + 2 quotes
}

void test_escapeCSV_truncates_at_32(void) {
    char output[128];
    const char* input = "1234567890123456789012345678901234567890";  // 40 chars
    size_t len = escapeCSV(input, output, 0, sizeof(output));  // Default cap is 32
    TEST_ASSERT_EQUAL_UINT(34, len);  // 32 + 2 quotes
    // Verify truncation
    TEST_ASSERT_EQUAL_UINT(34, strlen(output));
}

void test_escapeCSV_with_maxInputLen(void) {
    char output[64];
    const char* input = "TestNetwork";
    size_t len = escapeCSV(input, output, 4, sizeof(output));  // Only "Test"
    TEST_ASSERT_EQUAL_STRING("\"Test\"", output);
    TEST_ASSERT_EQUAL_UINT(6, len);
}

void test_escapeCSV_buffer_size_calculation(void) {
    const char* input = "Test\"Net";  // 8 chars, quote doubles
    size_t required = escapeCSV(input, nullptr, 0, 0);
    // "Test""Net" with outer quotes = 2 + 4 + 2 + 3 = 11
    TEST_ASSERT_EQUAL_UINT(11, required);
}

void test_escapeCSV_complex_ssid(void) {
    char output[128];
    const char* input = "Home\"WiFi\"\n2.4G";  // Quotes and newline
    escapeCSV(input, output, 0, sizeof(output));
    // Should be: "Home""WiFi""2.4G" (newline stripped, quotes doubled)
    TEST_ASSERT_EQUAL_STRING("\"Home\"\"WiFi\"\"2.4G\"", output);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // escapeXMLChar tests
    RUN_TEST(test_escapeXMLChar_ampersand);
    RUN_TEST(test_escapeXMLChar_less_than);
    RUN_TEST(test_escapeXMLChar_greater_than);
    RUN_TEST(test_escapeXMLChar_double_quote);
    RUN_TEST(test_escapeXMLChar_single_quote);
    RUN_TEST(test_escapeXMLChar_normal_char);
    
    // needsXMLEscape tests
    RUN_TEST(test_needsXMLEscape_special_chars);
    RUN_TEST(test_needsXMLEscape_normal_chars);
    
    // escapeXML tests
    RUN_TEST(test_escapeXML_normal_string);
    RUN_TEST(test_escapeXML_with_ampersand);
    RUN_TEST(test_escapeXML_with_less_than);
    RUN_TEST(test_escapeXML_with_greater_than);
    RUN_TEST(test_escapeXML_with_quotes);
    RUN_TEST(test_escapeXML_with_single_quote);
    RUN_TEST(test_escapeXML_multiple_special_chars);
    RUN_TEST(test_escapeXML_empty_string);
    RUN_TEST(test_escapeXML_null_input);
    RUN_TEST(test_escapeXML_with_maxInputLen);
    RUN_TEST(test_escapeXML_buffer_size_calculation);
    RUN_TEST(test_escapeXML_buffer_too_small);
    RUN_TEST(test_escapeXML_script_injection);
    
    // needsCSVQuoting tests
    RUN_TEST(test_needsCSVQuoting_normal_string);
    RUN_TEST(test_needsCSVQuoting_with_comma);
    RUN_TEST(test_needsCSVQuoting_with_quote);
    RUN_TEST(test_needsCSVQuoting_with_newline);
    RUN_TEST(test_needsCSVQuoting_with_cr);
    RUN_TEST(test_needsCSVQuoting_null_input);
    RUN_TEST(test_needsCSVQuoting_empty_string);
    
    // isCSVControlChar tests
    RUN_TEST(test_isCSVControlChar_control_chars);
    RUN_TEST(test_isCSVControlChar_null_is_not_control);
    RUN_TEST(test_isCSVControlChar_printable_chars);
    
    // escapeCSV tests
    RUN_TEST(test_escapeCSV_normal_string);
    RUN_TEST(test_escapeCSV_with_quote);
    RUN_TEST(test_escapeCSV_with_multiple_quotes);
    RUN_TEST(test_escapeCSV_strips_control_chars);
    RUN_TEST(test_escapeCSV_strips_tab);
    RUN_TEST(test_escapeCSV_preserves_comma);
    RUN_TEST(test_escapeCSV_empty_string);
    RUN_TEST(test_escapeCSV_null_input);
    RUN_TEST(test_escapeCSV_max_ssid_length);
    RUN_TEST(test_escapeCSV_truncates_at_32);
    RUN_TEST(test_escapeCSV_with_maxInputLen);
    RUN_TEST(test_escapeCSV_buffer_size_calculation);
    RUN_TEST(test_escapeCSV_complex_ssid);
    
    return UNITY_END();
}
