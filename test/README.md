```
 ██▓███   ▒█████   ██▀███   ██ ▄█▀ ▄████▄   ██░ ██  ▒█████   ██▓███  
▓██░  ██▒▒██▒  ██▒▓██ ▒ ██▒ ██▄█▒ ▒██▀ ▀█  ▓██░ ██▒▒██▒  ██▒▓██░  ██▒
▓██░ ██▓▒▒██░  ██▒▓██ ░▄█ ▒▓███▄░ ▒▓█    ▄ ▒██▀▀██░▒██░  ██▒▓██░ ██▓▒
▒██▄█▓▒ ▒▒██   ██░▒██▀▀█▄  ▓██ █▄ ▒▓▓▄ ▄██▒░▓█ ░██ ▒██   ██░▒██▄█▓▒ ▒
▒██▒ ░  ░░ ████▓▒░░██▓ ▒██▒▒██▒ █▄▒ ▓███▀ ░░▓█▒░██▓░ ████▓▒░▒██▒ ░  ░
▒▓▒░ ░  ░░ ▒░▒░▒░ ░ ▒▓ ░▒▓░▒ ▒▒ ▓▒░ ░▒ ▒  ░ ▒ ░░▒░▒░ ▒░▒░▒░ ▒▓▒░ ░  ░
░▒ ░       ░ ▒ ▒░   ░▒ ░ ▒░░ ░▒ ▒░  ░  ▒    ▒ ░▒░ ░  ░ ▒ ▒░ ░▒ ░     
░░       ░ ░ ░ ▒    ░░   ░ ░ ░░ ░ ░         ░  ░░ ░░ ░ ░ ▒  ░░       
             ░ ░     ░     ░  ░   ░ ░       ░  ░  ░    ░ ░           
                                  ░                                  
                        [ UNIT TEST SUITE ]
```

--[ Contents

    1 - What is this
    2 - Test Structure
    3 - Running Tests
        3.1 - Local Execution
        3.2 - CI Pipeline
    4 - Test Coverage
    5 - Adding New Tests
    6 - Mocking Strategy
    7 - Coverage Requirements


--[ 1 - What is this

    You're looking at the unit test suite for PORKCHOP. Every serious
    project needs tests, even one with an ASCII pig mascot.

    Tests run on native platform (Linux) using Unity test framework via
    PlatformIO. We test pure logic - no hardware dependencies, no ESP32
    needed. Just raw C++ getting poked with asserts until it proves it
    works.

    230+ tests across 7 files. String validation, channel helpers, RSSI
    conversion, time unit utilities, GPS distance, ML feature extraction,
    beacon parsing, anomaly scoring, string escaping, and the whole XP/
    leveling system. If you break something, you'll know before CI yells
    at you.


--[ 2 - Test Structure

    The test/ folder is laid out like this:

    +-----------------------------------------------+---------------------------+
    | Path                                          | What it does              |
    +-----------------------------------------------+---------------------------+
    | mocks/mock_arduino.h                          | Arduino type stubs        |
    | mocks/mock_esp_wifi.h                         | ESP32 WiFi type stubs     |
    | mocks/mock_preferences.h                      | NVS storage mock          |
    | mocks/testable_functions.h                    | Pure functions to test    |
    +-----------------------------------------------+---------------------------+
    | test_xp/test_xp_levels.cpp                    | XP system (39 tests)      |
    | test_distance/test_distance.cpp               | GPS distance (16 tests)   |
    | test_features/test_feature_extraction.cpp     | ML features (27 tests)    |
    | test_beacon/test_beacon_parsing.cpp           | Beacon parsing (19 tests) |
    | test_classifier/test_heuristic_classifier.cpp | Anomaly scoring (26 tests)|
    | test_utils/test_utils.cpp                     | Utility functions (58 tests)|
    | test_string_escape/test_string_escape.cpp     | XML/CSV escaping (45 tests)|
    +-----------------------------------------------+---------------------------+


    Each test lives in its own subdirectory so PlatformIO compiles them
    separately. This avoids linker conflicts with setUp/tearDown/main.

    The mocks/ folder fakes enough Arduino/ESP32 types that we can
    compile and run on a regular Linux box without the actual hardware.


--[ 3 - Running Tests


----[ 3.1 - Local Execution

    You need gcc/g++ for native builds. On Linux/macOS:

        # Run all tests
        $ pio test -e native

        # Run specific test file
        $ pio test -e native -f test_xp_levels

        # Verbose output (see what's actually happening)
        $ pio test -e native -v

        # With coverage report
        $ pio test -e native_coverage

    Windows users: tests run in CI. We don't test on Windows locally
    because life is too short for MSYS2 configuration.


----[ 3.2 - CI Pipeline

    GitHub Actions runs tests automatically on:

        * Push to main or develop branches
        * Pull requests to main

    CI workflow (.github/workflows/test.yml) does:

        1. Builds native test environment
        2. Runs all 230+ tests across 7 test files
        3. Generates coverage report with lcov
        4. Enforces 70% coverage threshold (drops below = fail)
        5. Uploads HTML coverage report as artifact
        6. Builds M5Cardputer firmware (compile check)

    If tests fail, the merge is blocked. Fix your code.


--[ 4 - Test Coverage

    We test pure logic that can be extracted from hardware dependencies:

    +--------------------+--------------------------------------------+
    | Module             | Functions Tested                           |
    +--------------------+--------------------------------------------+
    | XP System          | calculateLevel(), getXPForLevel(),         |
    |                    | getXPToNextLevel(), getLevelProgress(),    |
    |                    | achievement bitfield math                  |
    +--------------------+--------------------------------------------+
    | Distance           | haversineMeters() GPS distance calc        |
    |                    | (edge cases: poles, meridian, equator)     |
    +--------------------+--------------------------------------------+
    | Features           | isRandomizedMAC(), normalizeValue(),       |
    |                    | parseBeaconInterval(), parseCapability()   |
    +--------------------+--------------------------------------------+
    | Beacon Parsing     | IE extraction, beacon frame building,      |
    |                    | malformed frame handling                   |
    +--------------------+--------------------------------------------+
    | Classifier         | anomalyScoreRSSI(), anomalyScoreBeacon(),  |
    |                    | anomalyScoreEncryption(), anomalyScoreWPS()|
    +--------------------+--------------------------------------------+
    | Utilities          | SSID validation, channel/frequency math,   |
    |                    | RSSI quality, ms/TU time conversion        |
    +--------------------+--------------------------------------------+
    | String Escaping    | escapeXML(), escapeCSV(), needsCSVQuoting()|
    |                    | XML entity escaping, CSV quoting rules     |
    +--------------------+--------------------------------------------+


    Hardware-dependent code (WiFi promiscuous mode, BLE stack, display
    driver) is not unit tested. Those get tested on actual hardware
    like nature intended.


--[ 5 - Adding New Tests

    Want to add tests? Good. Here's the ritual:

        1. Create test_<module>.cpp in test/ folder

        2. Include the essentials:

            #include <unity.h>
            #include "mocks/testable_functions.h"

        3. Add setup/teardown (even if empty):

            void setUp(void) {}
            void tearDown(void) {}

        4. Write test functions (prefix with test_):

            void test_myFunction_shouldDoThing(void) {
                int result = myFunction(42);
                TEST_ASSERT_EQUAL(expected, result);
            }

        5. Register tests in main():

            int main(void) {
                UNITY_BEGIN();
                RUN_TEST(test_myFunction_shouldDoThing);
                return UNITY_END();
            }

    The function must be pure (no side effects, no hardware) to be
    testable this way. If it touches WiFi/BLE/Display, you can't unit
    test it. Extract the logic to a pure function and test that.


--[ 6 - Mocking Strategy

    We fake just enough to make tests compile:

    mock_arduino.h
        String class, millis(), delay(), Serial.printf()
        GPIO stubs, random(), map()

    mock_esp_wifi.h
        wifi_auth_mode_t enum
        wifi_ap_record_t struct
        wifi_promiscuous_pkt_t struct

    mock_preferences.h
        Full Preferences class with std::map backend
        Stores key/value pairs in memory
        Survives within test but resets between runs

    testable_functions.h
        Pure functions extracted from core modules
        calculateLevel(), haversineMeters(), isRandomizedMAC()
        All the anomalyScore*() functions
        Beacon parsing utilities

    The mocks don't simulate real behavior. They just provide enough
    type definitions and stubs that the code compiles. Real behavior
    testing happens on hardware.


--[ 7 - Coverage Requirements

    +---------------+------------------------------------------------+
    | Threshold     | 70% line coverage                              |
    | Enforcement   | CI fails if coverage drops below               |
    | Reports       | HTML uploaded as GitHub Actions artifact       |
    | Tool          | lcov (Linux only)                              |
    +---------------+------------------------------------------------+

    If you add code that drops coverage below 70%, either:

        a) Write tests for the new code
        b) Mark truly untestable code appropriately
        c) Argue convincingly in the PR why it's fine

    Option (c) rarely works. Write the tests.


==[EOF]==
