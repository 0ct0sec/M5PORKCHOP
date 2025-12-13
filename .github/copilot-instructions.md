# M5Porkchop Copilot Instructions

## Project Overview

M5Porkchop is a WiFi security research tool for the M5Cardputer (ESP32-S3 based) with a "piglet" mascot personality. It features:
- **OINK Mode**: WiFi scanning, handshake capture, PMKID capture, deauth attacks
- **WARHOG Mode**: Wardriving with GPS logging
- **PIGGYBLUES Mode**: BLE notification spam (AppleJuice, FastPair, Samsung, SwiftPair)
- **HOG ON SPECTRUM Mode**: Real-time WiFi spectrum analyzer with Gaussian lobes
- **RPG XP System**: Level up from BACON N00B to MUDGE UNCHA1NED (40 ranks)
- Interactive ASCII piglet avatar with mood-based phrases
- Settings menu with persistent configuration
- SD card logging and log viewer

## Documentation Style

README and user-facing docs use oldschool Phrack hacker magazine style:
- Section numbering with `--[ X - Title` format
- Subsections with `----[ X.Y - Subtitle` format
- ASCII art boxes and tables with `+---+` borders
- Monospaced code/example blocks indented with 4 spaces
- Edgy, irreverent tone - we made a hacking pig, not enterprise software
- No corporate speak, no hand-holding, assume the reader knows their stuff
- Snarky comments in parentheses are encouraged
- End with `==[EOF]==`

## Code Style Guidelines

1. **No emojis** - Do not use emojis in code, comments, documentation, or UI strings
2. **Static classes** for singletons (OinkMode, WarhogMode, Menu, Display, etc.)
3. **Include guards** with `#pragma once`
4. **Relative includes** from src root: `#include "../core/config.h"`
5. **Debug logging** via `Serial.printf("[MODULE] message\n")`
6. **Debounce pattern** for keyboard: track `keyWasPressed` bool, only act on edge transitions

## Architecture

### Core Components
- `src/core/porkchop.cpp/h` - Main state machine, mode management, event system
- `src/core/config.cpp/h` - Configuration structs (GPSConfig, WiFiConfig, PersonalityConfig), load/save to SPIFFS
- `src/core/sdlog.cpp/h` - SD card logging with tag-based log() function, date-stamped log files
- `src/core/wsl_bypasser.cpp/h` - ESP32 WiFi frame injection for deauth/disassoc, MAC randomization
- `src/core/xp.cpp/h` - RPG leveling system with NVS persistence, achievements, XP events

### Modes
- `src/modes/oink.cpp/h` - OinkMode: WiFi scanning, channel hopping, promiscuous mode, handshake capture
- `src/modes/warhog.cpp/h` - WarhogMode: GPS-enabled wardriving, multiple export formats (CSV, Wigle, Kismet, ML Training)
- `src/modes/piggyblues.cpp/h` - PiggyBluesMode: BLE notification spam targeting Apple/Android/Samsung/Windows devices
- `src/modes/spectrum.cpp/h` - SpectrumMode: Real-time WiFi spectrum analyzer with Gaussian lobes, channel hopping

### UI Layer
- `src/ui/display.cpp/h` - Triple-buffered canvas system (topBar, mainCanvas, bottomBar), 240x135 display, showToast(), showLevelUp()
- `src/ui/menu.cpp/h` - Main menu with callback system
- `src/ui/settings_menu.cpp/h` - Interactive settings with TOGGLE, VALUE, ACTION, TEXT item types
- `src/ui/captures_menu.cpp/h` - LOOT menu: captured handshakes/PMKID viewer, nuke loot feature
- `src/ui/achievements_menu.cpp/h` - Achievements viewer with unlock descriptions
- `src/ui/log_viewer.cpp/h` - SD card log file viewer with scrolling
- `src/ui/swine_stats.cpp/h` - Lifetime stats overlay with buff/debuff system

### Web Interface
- `src/web/fileserver.cpp/h` - WiFi AP file server for SD card access, black/white web UI

### Piglet Personality
- `src/piglet/avatar.cpp/h` - ASCII art pig face rendering with derpy style, direction flipping (L/R)
  - Right-facing frames (`_R` suffix): snout `00` on right, eye on left, pig looks RIGHT →, no pigtail
  - Left-facing frames (`_L` suffix): snout `00` on left, eye on right, pig looks LEFT ←, `z` pigtail on right
  - Default: pig faces right (toward speech bubble)
  - The `z` character represents the curly pigtail, NOT a sleep indicator
- `src/piglet/mood.cpp/h` - Context-aware phrases, happiness tracking, mode-specific phrase arrays
  - `Mood::getEffectiveHappiness()` - Returns -100 to +100 with momentum decay applied
  - `Mood::getLastActivityTime()` - Returns millis() of last activity event (for idle detection)

### Avatar Animation System
The avatar has several animation layers that modify the base frame in-place:

**Blink Animation**
- Triggered randomly every 4-8 seconds (mood-adjusted: excited = faster, sad = slower)
- Single frame - changes eye character to `-` (closed eye)
- Eye position: Right-facing `"(X 00)"` eye at [1], Left-facing `"(00 X)"` eye at [4]
- Does NOT change ears or other features (preserves mode-specific ear state)

**Sniff Animation**
- Triggered on: new network found, handshake captured, PMKID captured, stalking auths
- Changes nose from `00` to `oo` for 100ms
- Nose position: Right-facing `"(X 00)"` nose at [3-4], Left-facing `"(00 X)"` nose at [1-2]
- Call `Avatar::sniff()` to trigger

**Look/Walk Animation System**
Two separate timers control pig behavior when stationary (grass not moving):
- **LOOK Timer** (8-20s, mood-adjusted): Instant facing direction flip, no position change
- **WALK Timer** (15-40s, mood-adjusted): 400ms smooth transition to opposite side of screen
- Both timers disabled when `grassMoving` or `pendingGrassStart` is true
- `transitionToFacingRight` = direction of travel during walk
- Walk uses smooth step easing: `3t² - 2t³` over 400ms

**Tail Dynamics**
- Tail (`z` character) shown when pig faces AWAY from screen center
- Right side + facing right = tail on left (prefix `z(    )`)
- Left side + facing left = tail on right (suffix `(    )z`)
- Facing toward center = no tail shown
- During walk: tail trails behind movement direction
- When tail is on left (prefix), body X offset by -18px to keep aligned with head

**Speech Bubble Position**
- Bubble position based on `Avatar::isOnRightSide()`, NOT `isFacingRight()`
- Pig on left side → bubble on right; pig on right side → bubble on left
- Bubble stays fixed when pig looks around (only moves when pig walks)

**Grass Animation**
- `setGrassMoving(bool moving, bool directionRight = true)` - Controls grass scroll
- Early-exit optimization: returns immediately if already in requested state
- Does NOT trigger pig transitions - just starts/stops grass
- If pig is mid-transition, grass waits via `pendingGrassStart` flag
- Speed controlled by `setGrassSpeed(uint16_t ms)` - lower = faster

**Mood Peek System**
- 1.5 second flash of emotional state (ears/expression change)
- Triggered on happiness threshold crossing (>70 happy, <-30 sad)
- Also triggered by `forceMoodPeek()` on significant events (handshake, PMKID, deauth success)
- Mode-locked states (HUNTING in OINK/SPECTRUM, ANGRY in PIGGYBLUES) always show mode ears
- Mood peek only shows emotional expression temporarily, then returns to mode state

### Hardware
- `src/gps/gps.cpp/h` - TinyGPS++ wrapper, power management

### ML System
- `src/ml/features.cpp/h` - WiFiFeatures extraction from beacon frames (32-feature vector)
- `src/ml/inference.cpp/h` - Heuristic classifier with Edge Impulse integration scaffold
- `src/ml/edge_impulse.h` - Edge Impulse SDK interface (enable with `#define EDGE_IMPULSE_ENABLED`)

## Display System

```
┌────────────────────────────────────────┐
│ TOP_BAR (14px) - Mode indicator, icons │
├────────────────────────────────────────┤
│                                        │
│ MAIN_CANVAS (107px)                    │
│ - Avatar on left (ASCII pig)           │
│ - Speech bubble on right               │
│ - XP bar at y=91 (18px empty below     │
│   grass art, no layout changes needed) │
│                                        │
├────────────────────────────────────────┤
│ BOTTOM_BAR (14px) - Stats: N:x HS:x D:x│
│ WARHOG: U:x S:x [lat,lon] S:sats       │
└────────────────────────────────────────┘
```

- **Colors**: COLOR_FG = 0xFD75 (piglet pink), COLOR_BG = 0x0000 (black)
- **Display**: 240x135 pixels, landscape orientation

### Toast/Dialog Styling
All modal dialogs and toasts use consistent styling:
```cpp
// Black border (2px) then pink fill with rounded corners
canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);

// Black text on pink background, centered
canvas.setTextColor(COLOR_BG, COLOR_FG);
canvas.setTextDatum(top_center);
canvas.drawString("Message", centerX, boxY + 8);
```
Used in: `showToast()`, `showLevelUp()`, `showClassPromotion()`, warning dialogs, nuke confirmation, achievement details.

## Keyboard Navigation (M5Cardputer)

- **`;`** - Previous/Up/Decrease value
- **`.`** - Next/Down/Increase value
- **Enter** - Select/Toggle/Confirm
- **Backtick (`)** - Open menu / Exit to previous mode
- **O/W/B/H/S/T** - Quick mode shortcuts from IDLE (Oink/Warhog/piggyBlues/Hog on spectrum/Swine stats/Tweak settings)
- **P** - Take screenshot (saves to `/screenshots/screenshotNNN.bmp` on SD card)
- **Backspace** - Stop current mode and return to IDLE
- **G0 button** - Physical button on top, returns to IDLE from any mode (uses GPIO0 direct read)

## Mode State Machine

```
PorkchopMode:
  IDLE -> OINK_MODE | WARHOG_MODE | PIGGYBLUES_MODE | SPECTRUM_MODE | MENU | SETTINGS | CAPTURES | ACHIEVEMENTS | ABOUT | FILE_TRANSFER | LOG_VIEWER | SWINE_STATS
  MENU -> (any mode via menu selection)
  SETTINGS/CAPTURES/ACHIEVEMENTS/ABOUT/FILE_TRANSFER/LOG_VIEWER/SWINE_STATS -> MENU (via Enter or backtick)
  G0 button -> IDLE (from any mode)
```

**Important**: `previousMode` only stores "real" modes (IDLE, OINK_MODE, WARHOG_MODE, PIGGYBLUES_MODE, SPECTRUM_MODE), never MENU/SETTINGS/ACHIEVEMENTS/ABOUT/SWINE_STATS, to prevent navigation loops.

## Phrase System

Mood phrases are context-aware arrays in `mood.cpp`:
- `PHRASES_HAPPY[]` - On handshake capture
- `PHRASES_EXCITED[]` - Major events (many networks, high activity)
- `PHRASES_HUNTING[]` - During OINK mode scanning
- `PHRASES_SLEEPY[]` - Low activity periods
- `PHRASES_SAD[]` - No activity
- `PHRASES_IDLE[]` - Idle state
- `PHRASES_WARHOG[]` - During WARHOG mode
- `PHRASES_WARHOG_FOUND[]` - When new AP with GPS is logged
- `PHRASES_PIGGYBLUES_TARGETED[]` - When BLE target acquired
- `PHRASES_PIGGYBLUES_STATUS[]` - During BLE spam activity
- `PHRASES_PIGGYBLUES_IDLE[]` - PiggyBlues idle/scanning
- `PHRASES_DEAUTH_SUCCESS[]` - After successful deauth
- `PHRASES_PMKID_CAPTURED[]` - Clientless PMKID capture (3 beeps!)

Call appropriate `Mood::onXxx()` method to trigger phrase updates.

## Settings Menu

Settings use `SettingItem` struct with types:
- `TOGGLE` - ON/OFF, Enter toggles, arrows navigate
- `VALUE` - Numeric with min/max/step, arrows adjust directly
- `ACTION` - Triggers function (e.g., Save & Exit)
- `TEXT` - String input (Enter to edit, type characters, Enter to confirm, backtick to cancel)

Text input handles Shift+key for uppercase and special characters via keyboard library.

Settings persist to `/porkchop.conf` via ArduinoJson.

## Build Commands

```powershell
pio run                        # Build all
pio run -e m5cardputer         # Build release only
pio run -t upload              # Build and upload
pio run -t upload -e m5cardputer  # Upload release only
```

## Testing

### Unit Tests (CI)
Tests run automatically on GitHub Actions for every push to `main` or `develop` and every PR to `main`. The test suite uses Unity framework on the `native` platform (not ESP32).

**Note**: Tests require GCC and only run on Linux/macOS. Windows does not have GCC by default, so tests are run exclusively in CI (Ubuntu). Do not attempt to run `pio test -e native` on Windows without WSL or MinGW.

```bash
pio test -e native             # Run all tests locally (Linux/macOS only)
pio test -e native_coverage    # Run with coverage report
```

**Test directory structure** - Each test lives in its own subdirectory to ensure separate compilation:
```
test/
  test_beacon/test_beacon_parsing.cpp    # 802.11 beacon frame parsing
  test_classifier/test_heuristic_classifier.cpp  # ML anomaly scoring
  test_distance/test_distance.cpp        # Haversine GPS distance
  test_features/test_feature_extraction.cpp  # ML feature helpers
  test_xp/test_xp_levels.cpp             # XP/leveling system
  mocks/                                 # Shared mock headers
```

**Adding new tests:**
1. Create subdirectory: `test/test_newmodule/`
2. Create test file: `test/test_newmodule/test_newmodule.cpp`
3. Include mocks with relative path: `#include "../mocks/testable_functions.h"`
4. Define `setUp()`, `tearDown()`, `main()` with `UNITY_BEGIN()`/`UNITY_END()`
5. Add testable functions to `test/mocks/testable_functions.h`

### Writing Testable Functions (Lessons Learned)

When extracting logic from source files to `test/mocks/testable_functions.h` for unit testing:

**1. Pure Functions Only**
- Functions must have NO side effects (no Serial, WiFi, Display, SD, etc.)
- Take inputs as parameters, return outputs - no global state
- Use `inline` to avoid multiple definition linker errors

**2. Calculate Expected Values Carefully**
When writing tests that check string lengths or buffer sizes, manually trace through the function:
```cpp
// Example: escapeCSV("Net\"work") 
// Input: N, e, t, ", w, o, r, k = 8 chars
// Opening quote: outPos = 1
// N,e,t: outPos = 4  
// " doubled to "": outPos = 6
// w,o,r,k: outPos = 10
// Closing quote: outPos = 11
// Result: "Net""work" = 11 chars, NOT 12
```
Off-by-one errors are the most common test failures. Trace through the algorithm step-by-step.

**3. Test File Structure**
```cpp
#include <unity.h>
#include "../mocks/testable_functions.h"

void setUp(void) {}
void tearDown(void) {}

void test_functionName_scenario(void) {
    // Arrange
    char input[] = "test";
    char output[32];
    
    // Act
    size_t result = myFunction(input, output, sizeof(output));
    
    // Assert
    TEST_ASSERT_EQUAL_UINT(expected, result);
    TEST_ASSERT_EQUAL_STRING("expected", output);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_functionName_scenario);
    return UNITY_END();
}
```

**4. Common Pitfalls**
- **Null terminator**: Don't count `\0` in string length, but DO allocate space for it
- **Buffer overflow checks**: Test with `output = nullptr, maxLen = 0` to verify size calculation
- **Edge cases**: Empty string, max length string, null pointer, single character
- **Boundary values**: Test at exact boundaries (e.g., RSSI = -30 and -29 for threshold at -30)

**5. Coverage Environment**
The `native_coverage` environment requires both compile AND link flags:
```ini
[env:native_coverage]
build_flags =
    -fprofile-arcs
    -ftest-coverage
    -lgcov        # MUST link gcov library
    --coverage    # MUST be in build_flags, not separate
```
Missing `-lgcov` causes `undefined reference to '__gcov_init'` linker errors.

**6. CI-Only Testing (Windows)**
Tests only run in GitHub Actions CI (Ubuntu). Do NOT attempt `pio test -e native` on Windows.
Push and monitor CI runs at: https://github.com/neledov/M5PORKCHOP/actions

### Hardware Testing
For features that can't be unit tested:
1. Verify mode transitions
2. Check phrase display in each mode
3. Confirm settings save/load across reboots
4. Test GPS lock and wardriving CSV export

### Adding a new phrase category
```cpp
// In mood.cpp
const char* PHRASES_NEWMODE[] = {
    "Phrase 1",
    "Phrase 2"
};

void Mood::onNewModeEvent() {
    int idx = random(0, sizeof(PHRASES_NEWMODE) / sizeof(PHRASES_NEWMODE[0]));
    currentPhrase = PHRASES_NEWMODE[idx];
    lastPhraseChange = millis();
}
```

### Adding a new setting
```cpp
// In settings_menu.cpp loadFromConfig()
items.push_back({
    "Label",
    SettingType::VALUE,  // or TOGGLE
    initialValue,
    minVal, maxVal, step, "suffix"
});

// In saveToConfig() - apply the value
```

### Handling new mode
```cpp
// In porkchop.cpp setMode()
case PorkchopMode::NEW_MODE:
    NewMode::start();
    Avatar::setState(AvatarState::STATE);
    break;
```

## Hardware Specifics

- **M5Cardputer**: ESP32-S3, 240x135 ST7789 display, 74HC138 keyboard controller
- **M5Cardputer-Adv**: ESP32-S3, 240x135 ST7789 display, TCA8418 keyboard controller, EXT 14-pin bus
- **GPS**: Connected via UART (pins configurable in Settings Menu)
- **SD Card**: For handshake/wardriving data export
- **WiFi**: ESP32 native, promiscuous mode for packet capture

### GPS Hardware Configurations

| Hardware Setup | GPS RX Pin | GPS TX Pin | Baud Rate | Notes |
|----------------|------------|------------|-----------|-------|
| Original Cardputer + Grove GPS Unit | G1 | G2 | 115200 | Default config |
| Cardputer v1.1 + Grove GPS Unit | G1 | G2 | 115200 | Default config |
| Cardputer-Adv + Cap LoRa868 (U201) | G13 | G15 | 115200 | Change in Settings |

Users with Cardputer-Adv and Cap LoRa868 module must change GPS pins in Settings Menu:
- GPS RX Pin: 13
- GPS TX Pin: 15

The Cap LoRa868 connects via the EXT 2.54-14P bus, not the Grove HY2.0-4P port.

## PIGGYBLUES Mode Details

### Overview
BLE notification spam mode that sends crafted advertisements to trigger notifications on nearby devices. Targets:
- **Apple** (AppleJuice) - AirPods pairing popups
- **Android** (FastPair) - Google Fast Pair notifications  
- **Samsung** - Buds pairing dialogs
- **Windows** (SwiftPair) - Bluetooth device notifications

### Architecture
- Uses NimBLE 2.x for BLE advertising
- Periodic device scanning to find targets (blocking 3-second scan)
- Round-robin payload rotation across vendor types
- Configurable burst interval and target count

### Key Functions
- `scanForDevices()` - Blocking BLE scan, shows "Probing BLE..." toast
- `sendAppleJuice()` / `sendAndroidFastPair()` / `sendSamsungSpam()` / `sendWindowsSwiftPair()` - Vendor-specific payloads
- `identifyVendor()` - Classifies devices from manufacturer data

### Configuration
- `cfgScanDuration` - BLE scan duration in ms (default 3000)
- `cfgBurstInterval` - Time between advertisement bursts
- `cfgMaxTargets` - Maximum simultaneous targets

### Safety
- Warning dialog on first start (user must confirm)
- BLE stack recovery on advertising failures
- Proper NimBLE deinit on stop

## HOG ON SPECTRUM Mode Details

### Overview
Real-time WiFi spectrum analyzer that visualizes 2.4GHz band activity with Gaussian lobes representing actual 802.11b/g channel width spreading.

### Display Layout
- **y=0-75**: Spectrum area with RSSI scale (-95 to -30 dBm)
- **y=78**: Channel markers (1-13)
- **y=91**: XP bar (shared with other modes)

### Visualization
- Gaussian lobes with sigma ~6.6 pixels (22MHz channel width)
- Lobe height based on RSSI - stronger signal = taller peak
- Monochrome pink (COLOR_FG) - no color coding
- **[VULN!]** indicator for weak security (OPEN/WEP/WPA1)
- **[DEAUTH]** indicator for networks without PMF protection
- Stale networks removed after 5 seconds

### Channel Hopping
- 100ms per channel = ~1.3s full sweep across channels 1-13
- Uses promiscuous mode for beacon capture
- Same `busy` guard pattern as OINK/WARHOG for thread safety

### Controls
- **`;`** - Scroll left (lower channels)
- **`.`** - Scroll right (higher channels)
- **Enter** - Cycle through discovered networks, centers view on selection
- **Backspace/G0** - Exit mode

### Achievement
- **N13TZSCH3**: Stare at spectrum for 15 minutes straight
  - Toast: "the ether deauths back"
  - Tracks `startTime` in mode, checks elapsed time in `update()`

## ML System

### Current Architecture
```
WiFi Frame → FeatureExtractor → WiFiFeatures (32 floats) → MLInference → MLResult
                                                              ↓
                                              EdgeImpulse SDK (if enabled)
                                                     OR
                                              Heuristic Classifier
```

### ML Collection Modes
Configured via `config.ml.collectionMode` (MLCollectionMode enum in config.h):

- **BASIC (0)**: Uses ESP32 scan API only. Features from `wifi_ap_record_t`. Fast, reliable, limited feature set.
- **ENHANCED (1)**: Enables promiscuous mode beacon capture during WARHOG. Parses raw 802.11 frames for deep feature extraction (vendor IEs, HT capabilities, beacon timing jitter). More features but higher CPU/memory usage.

### Feature Extraction
Two extraction paths depending on mode:

1. **extractFromScan()**: Called with `wifi_ap_record_t` from scan API. Gets RSSI, channel, authmode, basic IE presence.
2. **extractFromBeacon()**: Called with raw beacon frame buffer. Parses IEs directly:
   - IE 0 (SSID) - Hidden network detection (zero-length or all-null bytes)
   - IE 3 (DS Parameter Set) - Channel extraction
   - IE 45 (HT Capabilities) - 802.11n features
   - IE 48 (RSN) - WPA2/WPA3, PMF, cipher suite detection
   - IE 50 (Extended Supported Rates) - Additional rate analysis
   - IE 221 (Vendor Specific) - WPS, WPA1, vendor fingerprinting

### Race Condition Guard
In Enhanced mode, beacon callback runs in WiFi task context while main loop processes data. Use the `beaconMapBusy` volatile flag pattern:

```cpp
volatile bool beaconMapBusy = false;

// In promiscuous callback:
if (beaconMapBusy) return;  // Skip if main thread is processing

// In main loop before iterating beaconMap:
beaconMapBusy = true;
// ... process beaconMap ...
beaconMapBusy = false;
```

### ML Labels
- `NORMAL` (0) - Legitimate network
- `ROGUE_AP` (1) - Suspicious access point (strong signal, odd beacon timing)
- `EVIL_TWIN` (2) - Impersonating known network
- `DEAUTH_TARGET` (3) - Vulnerable to deauth attacks
- `VULNERABLE` (4) - Open/WEP/WPA1 network

### Anomaly Scoring
The `anomalyScore` field (0.0-1.0) is calculated from multiple signals:
- RSSI > -30 dBm (suspiciously strong)
- Open or WEP encryption
- Hidden SSID
- Non-standard beacon interval (not ~100ms)
- No HT capabilities (unusual for modern APs)
- WPS enabled on open network (honeypot pattern)

### Heuristic Detection Rules
The classifier scores networks on:
- Signal strength anomalies (RSSI > -30 suspicious)
- Non-standard beacon intervals (normal is 100ms)
- Beacon jitter (software APs have higher jitter)
- Missing vendor IEs (real routers have many)
- Open network with WPS (honeypot pattern)
- WPA3/PMF for deauth resistance

### Training Data Collection
In WARHOG mode (Enhanced), ML training data is automatically exported:
- **Periodic export**: Every 60 seconds to `/ml_training.csv` (full accumulated data)
- **On stop**: Final export when G0 is pressed
- **Crash protection**: Periodic dumps ensure at most 1 minute of data loss

```cpp
// Manual export (if needed)
WarhogMode::exportMLTraining("/ml_training.csv");
```
Outputs all 32 features + BSSID, SSID, label, GPS coords.

## WARHOG Mode Details

### Architecture: GPS as Gate (Not Queue)
WARHOG uses a "GPS as gate" architecture - networks are written directly to disk per-scan, not accumulated in RAM waiting for GPS:

- **With GPS fix**: Network written to both CSV (wardriving) and ML file (if Enhanced mode)
- **Without GPS**: Network written to ML file only (no coordinates) - still valuable for ML training
- **No RAM accumulation**: Data goes directly to SD card, no `entries[]` vector

This eliminates memory pressure issues when operating indoors without GPS coverage.

### Background Scanning
WiFi scanning runs in a FreeRTOS background task to keep UI responsive:
- `scanTask()` runs `WiFi.scanNetworks()` on Core 0
- Main loop continues handling display/keyboard
- Scan results processed when task completes (~7 seconds per scan)
- Scan cancelled cleanly on stop (vTaskDelete + WiFi.scanDelete)

### Per-Network File Writes
Each network discovered is immediately written to disk:
```cpp
// With GPS:
appendCSVEntry(bssid, ssid, rssi, channel, auth, lat, lon, alt);
appendMLEntry(bssid, ssid, features, label, lat, lon);

// Without GPS (Enhanced mode only):
appendMLEntry(bssid, ssid, features, label, 0, 0);
```
Files are created on first write with `ensureCSVFileReady()` / `ensureMLFileReady()`.

### Memory Management
- `seenBSSIDs` set tracks processed networks (max 5000, ~120KB)
- `beaconFeatures` map caches beacon data for Enhanced mode (max 500)
- No `entries[]` vector - direct disk writes eliminate RAM growth
- Emergency cleanup clears tracking sets if heap critical (<25KB)

### Export Formats
Session files are created with GPS timestamp or millis+random suffix:
- **CSV**: `/wardriving/warhog_YYYYMMDD_HHMMSS.csv` - BSSID, SSID, RSSI, channel, auth, GPS coords
- **ML Training**: `/ml_training_YYYYMMDD_HHMMSS.ml.csv` - 32-feature vector with labels

Legacy export functions (`exportWigle`, `exportKismet`) are deprecated - data is already on disk.

### Data Safety
- SSIDs are properly escaped (CSV quotes, XML entities)
- Control characters stripped from SSID fields
- Per-network writes with immediate flush (crash protection)
- SD writes are single-threaded (main loop only, never from scan task)
- Scan cancelled cleanly on stop to prevent orphaned tasks

### Edge Impulse Integration
1. Train model at studio.edgeimpulse.com
2. Export C++ Library for ESP32
3. Copy `edge-impulse-sdk/` to `lib/`
4. Uncomment `#define EDGE_IMPULSE_ENABLED` in `edge_impulse.h`
5. Rebuild - real inference replaces heuristics

## Intentional Design Patterns (NOT BUGS)

These patterns may look like issues during code review but are intentional design decisions:

### OINK Mode - Single-Slot Deferred Events
The `pendingNetworkAdd` pattern uses a single slot, not a queue:
```cpp
if (!pendingNetworkAdd) {
    memcpy(&pendingNetwork, &net, sizeof(DetectedNetwork));
    pendingNetworkAdd = true;
}
```
**Why**: Avoids heap allocation in WiFi callback. Missing one beacon is harmless - network will be captured on next beacon. This is safer than a dynamic queue in ISR-like context.

### OINK Mode - oinkBusy Guard
The `oinkBusy` volatile bool prevents concurrent vector access:
```cpp
oinkBusy = true;
// ... process networks/handshakes vectors ...
oinkBusy = false;
```
**Why**: Promiscuous callback runs in WiFi task context. The guard skips callback processing while main loop is iterating vectors. Not a mutex, but sufficient for this use case.

### OINK Mode - Deauth Jitter
The `sendDeauthBurst()` function adds random delays between frames:
```cpp
delay(random(1, 6));  // 1-5ms jitter
```
**Why**: WIDS systems detect deauth floods by looking for machine-perfect 0ms timing between frames. Random jitter makes traffic look more organic. Applied between forward/reverse frames and between burst iterations.

### OINK Mode - Deauth Rate Explained
Deauth attacks run in ATTACKING state with 100ms minimum cycle time:

| Clients | Per Cycle | D: Counter Rate |
|---------|-----------|-----------------|
| 0 | 1 broadcast | ~10/sec |
| 1 | 5-8 burst + 1 broadcast | ~50-80/sec |
| 2+ | N×burst + 1 broadcast | ~100+/sec |

**D: counter** shows forward deauths only (reverse deauths and disassocs not counted).
**If D: shows ~10/sec**: No clients discovered during LOCKING phase - only broadcast deauth.
**If D: resets to 0**: State transition to NEXT_TARGET (normal cycling behavior).

### OINK Mode - Hashcat 22000 Format Export
Captured handshakes and PMKIDs are saved in hashcat-ready format:

**WPA*02 (EAPOL handshake):**
```
WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*NONCE_AP*EAPOL_CLIENT*MESSAGEPAIR
```
- MIC: 16 bytes extracted from M2 frame (offset 81, NOT 77)
- EAPOL: Full M2 frame with MIC zeroed for verification
- MESSAGEPAIR: 0x00 for M1+M2, 0x02 for M2+M3 fallback
- Saved as `BSSID_hs.22000`

**WPA*01 (PMKID):**
```
WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
```
- PMKID: 16 bytes from PMKID KDE in M1 Key Data
- 01 = PMKID taken from AP
- Saved as `BSSID.22000`

**PMKID Filtering:**
- Some APs include PMKID KDE (`dd 14 00 0f ac 04`) but with all-zero data
- These are useless for cracking (no cached PMK for client)
- Code skips zero PMKIDs during extraction AND save
- If piglet announces PMKID capture, it's a real crackable one

### MAC Randomization
`WSLBypasser::randomizeMAC()` generates a random locally-administered MAC on mode start:
```cpp
mac[0] = (mac[0] & 0xFC) | 0x02;  // Set local bit, clear multicast
```
**Why**: Prevents device fingerprinting across sessions. Uses ESP32 hardware RNG (`esp_fill_random`). Called in OINK, WARHOG, and SPECTRUM mode start() if `Config::wifi().randomizeMAC` is true (default ON).

### WARHOG Mode - seenBSSIDs Set Growth
The `seenBSSIDs` std::set grows during wardriving session:
- 24 bytes per entry (8 byte uint64_t key + 16 byte red-black tree node overhead)
- Capped at MAX_SEEN_BSSIDS (5,000) = ~120KB max
- Cleared on `start()`, not during session
- When limit reached, new inserts are skipped (some networks may be re-saved to CSV)
**Why**: Prevents re-processing already-saved networks. Capped to prevent OOM on long walks. Duplicates in CSV are acceptable.

### WARHOG Mode - Heap Monitoring and Emergency Cleanup
Periodic heap monitoring (every 30s) with emergency cleanup:
```cpp
if (freeHeap < HEAP_CRITICAL_THRESHOLD) {  // 25KB
    seenBSSIDs.clear();
    beaconFeatures.clear();
}
```
**Side effect**: All networks will be re-detected as "new" and re-saved to CSV after emergency cleanup.
**Why**: Better to have CSV duplicates than crash. User can dedupe CSV offline if needed.

### WARHOG Mode - beaconMapBusy Race Window
The `beaconMapBusy` guard has a small theoretical race window:
```cpp
// In processScanResults:
beaconMapBusy = true;
// ... access beaconFeatures ...

// In promiscuousCallback (WiFi task context):
if (beaconMapBusy) return;  // Skip if busy
```
**Why**: Not a mutex, but sufficient. Callback just skips processing if busy - missing a few beacons is acceptable. Not a crash risk.

### WARHOG Mode - SD Retry with openFileWithRetry()
SD card operations use retry pattern (3 attempts, 10ms delay):
```cpp
static File openFileWithRetry(const char* path, const char* mode) {
    for (int retry = 0; retry < SD_RETRY_COUNT; retry++) {
        File f = SD.open(path, mode);
        if (f) return f;
        delay(SD_RETRY_DELAY_MS);
    }
    return f;
}
```
**Why**: SD cards can have transient busy states. Matches pattern in `sdlog.cpp`.

### WARHOG Mode - Graceful Task Shutdown
Background scan task uses `stopRequested` flag for clean exit:
```cpp
stopRequested = true;
// Wait up to 500ms for task to exit naturally
for (int i = 0; i < 10 && scanTaskHandle != NULL; i++) {
    delay(50);
}
// Force cleanup only if task didn't exit
if (scanTaskHandle != NULL) {
    vTaskDelete(scanTaskHandle);
}
```
**Why**: Allows `WiFi.scanNetworks()` to complete naturally instead of killing mid-operation, preventing WiFi stack corruption.

### PIGGYBLUES Mode - No BLE deinit()
The code explicitly avoids `NimBLEDevice::deinit()`:
```cpp
// DON'T call deinit - ESP32-S3 has issues reinitializing BLE after deinit
// Just keep BLE initialized but idle
```
**Why**: ESP32-S3 has known issues with BLE reinitialization. Keeping stack alive but idle is more reliable.

### Avatar - setGrassMoving Early Exit
Per-frame calls to `setGrassMoving()` must be cheap:
```cpp
if (moving && (grassMoving || pendingGrassStart)) {
    return;  // Already moving or pending - don't interrupt
}
```
**Why**: OINK mode calls `setGrassMoving(channelHopping)` every frame in update(). Without early exit, this was causing performance issues (reduced deauth rate). The function now returns immediately if already in the correct state.

### Avatar - No Treadmill Transitions on Per-Frame Calls
`setGrassMoving()` does NOT trigger pig walk transitions:
```cpp
// Just start grass immediately - no transition blocking
if (transitioning) {
    pendingGrassStart = true;  // Wait for current transition
} else {
    grassMoving = true;  // Start now
}
```
**Why**: The treadmill effect (pig walks to position) is for mode start only, not per-frame sync. Previous implementation triggered transitions every frame when conditions were right, causing deauth rate slowdown.

### Settings Menu - 32 Character Limit
Text input enforces length limit:
```cpp
if (textBuffer.length() < 32) {
    textBuffer += c;
}
```
**Why**: WiFi SSIDs max 32 chars, passwords fit in allocated buffers.

### Features.cpp - IE Parsing Bounds
IE parsing has proper bounds checking:
```cpp
while (offset + 2 < len) {
    uint8_t ieLen = frame[offset + 1];
    if (offset + 2 + ieLen > len) break;
    // ... parse IE ...
    offset += 2 + ieLen;
}
```
**Why**: Malformed beacons from attackers must not crash the device.

### Config Values - Not Magic Numbers
Timeouts and limits in `config.h` have comments explaining their purpose:
```cpp
uint16_t channelHopInterval = 500;  // ms between channel hops
uint16_t burstInterval = 200;       // ms between advertisement bursts (50-500)
```
**Why**: Values are configurable at runtime via settings menu, not hardcoded constants.

## XP System (RPG Leveling)

### Overview
Persistent experience point system with 40 rank titles (Phrack/swine themed). Data stored in NVS (Preferences library) - survives reboots and reflash. XP bar displays at y=91 in the existing 18px empty space below the grass art.

### Buff/Debuff System (SWINE STATS)
The game includes a mood-based buff/debuff system that modifies deauth packet rates, XP gain, and channel hopping speed. Buffs/debuffs are calculated from Mood happiness and session stats.

**BUFFS (Positive Effects):**
| NAME | TRIGGER | EFFECT |
|------|---------|--------|
| R4G3 | happiness > 70 | +50% deauth burst (5→8 pkts) |
| SNOUT$HARP | happiness > 50 | +25% XP gain |
| H0TSTR3AK | 2+ handshakes in session | Momentum bonus |
| C4FF31N4T3D | happiness > 80 | -30% channel hop interval |

**DEBUFFS (Negative Effects):**
| NAME | TRIGGER | EFFECT |
|------|---------|--------|
| SLOP$LUG | happiness < -50 | -30% deauth burst (5→3 pkts) |
| F0GSNOUT | happiness < -30 | -15% XP gain |
| TR0UGHDR41N | 5min no activity | +2ms deauth jitter |
| HAM$TR1NG | happiness < -70 | +50% channel hop interval |

**Realistic Deauth Packet Rates:**
- Base: 5 frames per burst with 1-5ms jitter
- R4G3 Mode: 8 frames per burst (aggressive)
- SLOP$LUG Mode: 3 frames per burst (conserving)
- Jitter modified by TR0UGHDR41N (1-7ms when debuffed)

**Access via:**
- **S key** from IDLE mode
- **SWINE STATS** menu item (between HOG ON SPECTRUM and FILE TRANSFER)

**Tabbed Display Layout:**
SWINE STATS uses a two-tab interface (switch with `;` / `.` keys):

Tab 1 - ST4TS:
```
┌────────────────────────────────────────┐
│ [ST4TS] [B00STS]                       │
│ LVL 23: KERNEL BAC0N         < R0GU3 > │
│ [████████████░░░░░] 72%                │
│         12500 XP (72%)                 │
├────────────────────────────────────────┤
│ N3TW0RKS: 4269    H4NDSH4K3S: 127      │
│ PMK1DS: 43        D34UTHS: 1892        │
│ D1ST4NC3: 23.4km  BL3 BL4STS: 5420     │
│ S3SS10NS: 89      GH0STS: 42           │
└────────────────────────────────────────┘
```

Tab 2 - B00STS:
```
┌────────────────────────────────────────┐
│ [ST4TS] [B00STS]                       │
│ CL4SS P3RKS:                           │
│ [*] P4CK3T NOSE -10% hop               │
│ [*] H4RD SNOUT +1 burst                │
│ [*] R04D H0G +15% dist XP              │
│ [*] SH4RP TUSKS +1s lock               │
│ M00D B00STS:                           │
│ [+] R4G3 +50% deauth pwr               │
│ [-] TR0UGHDR41N +2ms jitter            │
└────────────────────────────────────────┘
```

### Class System (8 Tiers)
Every 5 levels, players promote to a new class tier with permanent cumulative buffs. Class is calculated from level (stateless design - existing characters get buffs immediately after firmware update).

**Class Tiers:**
| LEVELS | CLASS | BUFF UNLOCKED |
|--------|-------|---------------|
| 1-5 | SH0AT | (none - starter tier) |
| 6-10 | SN1FF3R | P4CK3T NOSE: -10% channel hop interval |
| 11-15 | PWNER | H4RD SNOUT: +1 deauth burst frame |
| 16-20 | R00T | R04D H0G: +15% distance XP |
| 21-25 | R0GU3 | SH4RP TUSKS: +1s lock time (3s→4s, better client discovery) |
| 26-30 | EXPL01T | CR4CK NOSE: +10% capture XP (HS/PMKID) |
| 31-35 | WARL0RD | IR0N TUSKS: -1ms deauth jitter max |
| 36-40 | L3G3ND | OMNI P0RK: +5% all effects |

**Buff Stacking Example (L38 Player):**
Has all 7 class buffs active simultaneously:
- Channel hop: 500ms × 0.9 × 0.95 = 427ms
- Deauth burst: 5 + 1 = 6 frames (then ×1.05 = 6)
- Lock time: 3000ms + 1000ms = 4200ms (after ×1.05)
- Distance XP: base × 1.15 × 1.05 = 120% bonus
- Capture XP: base × 1.10 × 1.05 = 115% bonus
- Jitter max: 5 - 1 = 4ms

**Class Promotion Popup:**
When level crosses class boundary, a popup displays:
```
┌─────────────────────────────┐
│   * CL4SS PR0M0T10N *       │
│     PWNER -> R00T           │
│    new powers acquired      │
└─────────────────────────────┘
```
Triggered after `showLevelUp()` with 500ms delay.

**Implementation:**
- `PorkClass` enum in `xp.h` (8 values: SH0AT through L3G3ND)
- `XP::getClass()` / `XP::getClassName()` - returns current class
- `XP::getClassForLevel()` / `XP::getClassNameFor()` - class by level
- `ClassBuff` enum in `swine_stats.h` (7 buffs)
- `SwineStats::calculateClassBuffs()` - returns bitfield of active class buffs
- `SwineStats::hasClassBuff(ClassBuff)` - check specific buff
- Effect getters integrate class buffs: `getDeauthBurstCount()`, `getChannelHopInterval()`, `getLockTime()`, `getDistanceXPMultiplier()`, `getCaptureXPMultiplier()`

### Storage
- **Namespace**: `porkxp` (NVS)
- **Keys**: `xp` (uint32), `lvl` (uint8), `hi_hs` (uint16), `hi_net` (uint16), `hi_km` (uint16), `ach` (uint16)
- **Size**: ~40 bytes total in NVS

### XP Events and Values
```cpp
NETWORK_FOUND      = 1     // Normal network discovered
NETWORK_HIDDEN     = 3     // Hidden SSID found
HANDSHAKE_CAPTURED = 50    // WPA handshake grabbed
DEAUTH_SUCCESS     = 15    // Deauth frames sent
WARHOG_LOGGED      = 2     // AP logged with GPS
WARHOG_KM          = 25    // 1km walked while wardriving
GPS_LOCK           = 5     // First GPS fix of session
BLE_BURST          = 2     // BLE spam burst sent
SESSION_30MIN      = 10    // 30 minutes active
SESSION_1HR        = 25    // 1 hour active
SESSION_2HR        = 50    // 2 hours active (dedication)
```

### Rank Titles (40 Levels)
Exponential XP curve - quick early levels, months at high levels:
```
L1:  BACON N00B          L21: KARMA SW1NE
L2:  SCRIPT PIGG0        L22: EVIL TWIN H0G
L3:  PIGLET 0DAY         L23: KERNEL BAC0N
L4:  SNOUT SCAN          L24: MON1TOR BOAR
L5:  SLOP NMAP           L25: WPA3 WARTH0G
L6:  BEACON BOAR         L26: KRACK SW1NE
L7:  CHAN H4M            L27: FR4G ATTACK
L8:  PROBE PORK          L28: DRAGONBL00D
L9:  SSID SW1NE          L29: DEATH PR00F
L10: PKT PIGLET          L30: PLANET ERR0R
L11: DEAUTH H0G          L31: P0RK FICTION
L12: HANDSHAKE HAM       L32: RESERVOIR H0G
L13: PMKID PORK          L33: HATEFUL 0INK
L14: EAPOL B0AR          L34: JACK1E B0AR
L15: SAUSAGE SYNC        L35: 80211 WARL0RD
L16: WARDRIVE HOG        L36: MACHETE SW1NE
L17: GPS L0CK PIG        L37: CRUNCH P1G
L18: BLE SPAM HAM        L38: DARK TANGENT
L19: TRUFFLE R00T        L39: PHIBER 0PT1K
L20: INJECT P1G          L40: MUDGE UNCHA1NED
```

### Achievements (47 Bitflags)
47 secret badges stored in `data.achievements` uint64_t bitfield. Viewable via Achievements menu.

```cpp
// Original achievements (bits 0-16)
ACH_FIRST_BLOOD     = 1ULL << 0   // First handshake
ACH_CENTURION       = 1ULL << 1   // 100 networks in one session
ACH_MARATHON_PIG    = 1ULL << 2   // 10km walked in session
ACH_NIGHT_OWL       = 1ULL << 3   // Hunt between midnight-5am
ACH_GHOST_HUNTER    = 1ULL << 4   // 10 hidden networks
ACH_APPLE_FARMER    = 1ULL << 5   // 100 Apple BLE packets
ACH_WARDRIVER       = 1ULL << 6   // 1000 networks lifetime
ACH_DEAUTH_KING     = 1ULL << 7   // 100 successful deauths
ACH_PMKID_HUNTER    = 1ULL << 8   // Capture PMKID
ACH_WPA3_SPOTTER    = 1ULL << 9   // Find WPA3 network
ACH_GPS_MASTER      = 1ULL << 10  // 100 GPS-tagged networks
ACH_TOUCH_GRASS     = 1ULL << 11  // 50km total lifetime
ACH_SILICON_PSYCHO  = 1ULL << 12  // 5000 networks lifetime
ACH_CLUTCH_CAPTURE  = 1ULL << 13  // Handshake at <10% battery
ACH_SPEED_RUN       = 1ULL << 14  // 50 networks in 10 minutes
ACH_CHAOS_AGENT     = 1ULL << 15  // 1000 BLE packets
ACH_NIETZSWINE      = 1ULL << 16  // Stare at spectrum for 15 minutes

// Network milestones (bits 17-21)
ACH_TEN_THOUSAND    = 1ULL << 17  // 10,000 networks lifetime
ACH_NEWB_SNIFFER    = 1ULL << 18  // First 10 networks
ACH_FIVE_HUNDRED    = 1ULL << 19  // 500 networks in session
ACH_OPEN_SEASON     = 1ULL << 20  // 50 open networks
ACH_WEP_LOLZER      = 1ULL << 21  // Find a WEP network (ancient relic)

// Handshake/PMKID milestones (bits 22-26)
ACH_HANDSHAKE_HAM   = 1ULL << 22  // 10 handshakes lifetime
ACH_FIFTY_SHAKES    = 1ULL << 23  // 50 handshakes lifetime
ACH_PMKID_FIEND     = 1ULL << 24  // 10 PMKIDs captured
ACH_TRIPLE_THREAT   = 1ULL << 25  // 3 handshakes in session
ACH_HOT_STREAK      = 1ULL << 26  // 5 handshakes in session

// Deauth milestones (bits 27-29)
ACH_FIRST_DEAUTH    = 1ULL << 27  // First successful deauth
ACH_DEAUTH_THOUSAND = 1ULL << 28  // 1000 successful deauths
ACH_RAMPAGE         = 1ULL << 29  // 10 deauths in session

// Distance/WARHOG milestones (bits 30-33)
ACH_HALF_MARATHON   = 1ULL << 30  // 21km in session
ACH_HUNDRED_KM      = 1ULL << 31  // 100km lifetime
ACH_GPS_ADDICT      = 1ULL << 32  // 500 GPS-tagged networks
ACH_ULTRAMARATHON   = 1ULL << 33  // 50km in session

// BLE/PIGGYBLUES milestones (bits 34-38)
ACH_PARANOID_ANDROID = 1ULL << 34 // 100 Android FastPair spam
ACH_SAMSUNG_SPRAY   = 1ULL << 35  // 100 Samsung spam
ACH_WINDOWS_PANIC   = 1ULL << 36  // 100 Windows SwiftPair spam
ACH_BLE_BOMBER      = 1ULL << 37  // 5000 BLE packets
ACH_OINKAGEDDON     = 1ULL << 38  // 10000 BLE packets

// Time/session milestones (bits 39-42)
ACH_SESSION_VET     = 1ULL << 39  // 100 sessions
ACH_FOUR_HOUR_GRIND = 1ULL << 40  // 4 hour session
ACH_EARLY_BIRD      = 1ULL << 41  // Active 5-7am
ACH_WEEKEND_WARRIOR = 1ULL << 42  // Session on weekend

// Special/rare (bits 43-46)
ACH_ROGUE_SPOTTER   = 1ULL << 43  // ML detects rogue AP
ACH_HIDDEN_MASTER   = 1ULL << 44  // 50 hidden networks
ACH_WPA3_HUNTER     = 1ULL << 45  // 25 WPA3 networks
ACH_MAX_LEVEL       = 1ULL << 46  // Reach level 40
```

### Achievements Menu
`src/ui/achievements_menu.cpp/h` - Accessible from main menu ("ACHIEVEMENTS").

- Shows all 47 achievements with `[X]` unlocked / `[ ]` locked
- Locked achievements show `???` for name (no spoilers)
- Bottom bar shows unlock description for selected achievement, or "UNKNOWN" if locked
- Enter key shows toast-style detail popup (pink bg, black text)
- Detail shows achievement name, UNLOCKED/LOCKED status, unlock condition
- Uses only system colors (COLOR_FG/COLOR_BG)

### Integration Points
XP is awarded via `Mood::onXxx()` handlers:
```cpp
// In mood.cpp event handlers:
void Mood::onHandshakeCaptured() {
    XP::addXP(XPEvent::HANDSHAKE_CAPTURED);
    // ... phrase selection ...
}
```

Distance tracking in `warhog.cpp` uses Haversine formula:
```cpp
// Awards XP every 1km walked while wardriving
if (sessionDistanceM >= 1000.0) {
    XP::addXP(XPEvent::WARHOG_KM);
    sessionDistanceM -= 1000.0;
}
```

### Level Up Popup
Blocking popup via `Display::showLevelUpPopup()`:
- Shows new rank title and level number
- Random phrase from `Mood::getRandomLevelUpPhrase()`
- Waits for Enter or G0 button press
