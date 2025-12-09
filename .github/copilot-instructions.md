# M5Porkchop Copilot Instructions

## Project Overview

M5Porkchop is a WiFi security research tool for the M5Cardputer (ESP32-S3 based) with a "piglet" mascot personality. It features:
- **OINK Mode**: WiFi scanning, handshake capture, deauth attacks
- **WARHOG Mode**: Wardriving with GPS logging
- **PIGGYBLUES Mode**: BLE notification spam (AppleJuice, FastPair, Samsung, SwiftPair)
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
- `src/core/wsl_bypasser.cpp/h` - ESP32 WiFi frame injection for deauth/disassoc

### Modes
- `src/modes/oink.cpp/h` - OinkMode: WiFi scanning, channel hopping, promiscuous mode, handshake capture
- `src/modes/warhog.cpp/h` - WarhogMode: GPS-enabled wardriving, multiple export formats (CSV, Wigle, Kismet, ML Training)
- `src/modes/piggyblues.cpp/h` - PiggyBluesMode: BLE notification spam targeting Apple/Android/Samsung/Windows devices

### UI Layer
- `src/ui/display.cpp/h` - Triple-buffered canvas system (topBar, mainCanvas, bottomBar), 240x135 display, showToast()
- `src/ui/menu.cpp/h` - Main menu with callback system
- `src/ui/settings_menu.cpp/h` - Interactive settings with TOGGLE, VALUE, ACTION, TEXT item types
- `src/ui/captures_menu.cpp/h` - Captured handshakes viewer
- `src/ui/log_viewer.cpp/h` - SD card log file viewer with scrolling

### Web Interface
- `src/web/fileserver.cpp/h` - WiFi AP file server for SD card access, black/white web UI

### Piglet Personality
- `src/piglet/avatar.cpp/h` - ASCII art pig face rendering with derpy style, direction flipping (L/R)
- `src/piglet/mood.cpp/h` - Context-aware phrases, happiness tracking, mode-specific phrase arrays

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
│                                        │
├────────────────────────────────────────┤
│ BOTTOM_BAR (14px) - Stats: N:x HS:x D:x│
│ WARHOG: U:x S:x [lat,lon] S:sats       │
└────────────────────────────────────────┘
```

- **Colors**: COLOR_FG = 0xFD75 (piglet pink), COLOR_BG = 0x0000 (black)
- **Display**: 240x135 pixels, landscape orientation

## Keyboard Navigation (M5Cardputer)

- **`;`** - Previous/Up/Decrease value
- **`.`** - Next/Down/Increase value
- **Enter** - Select/Toggle/Confirm
- **Backtick (`)** - Open menu / Exit to previous mode
- **O/W/B/S** - Quick mode shortcuts from IDLE (Oink/Warhog/piggyBlues/Settings)
- **G0 button** - Physical button on top, returns to IDLE from any mode (uses GPIO0 direct read)

## Mode State Machine

```
PorkchopMode:
  IDLE -> OINK_MODE | WARHOG_MODE | PIGGYBLUES_MODE | MENU | SETTINGS | CAPTURES | ABOUT | FILE_TRANSFER | LOG_VIEWER
  MENU -> (any mode via menu selection)
  SETTINGS/CAPTURES/ABOUT/FILE_TRANSFER/LOG_VIEWER -> MENU (via Enter or backtick)
  G0 button -> IDLE (from any mode)
```

**Important**: `previousMode` only stores "real" modes (IDLE, OINK_MODE, WARHOG_MODE, PIGGYBLUES_MODE), never MENU/SETTINGS/ABOUT, to prevent navigation loops.

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

## Common Patterns

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

- **M5Cardputer**: ESP32-S3, 240x135 ST7789 display, TCA8418 keyboard controller
- **GPS**: Connected via UART (pins configurable in GPSConfig)
- **SD Card**: For handshake/wardriving data export
- **WiFi**: ESP32 native, promiscuous mode for packet capture

## Testing

No automated tests currently. Test on hardware:
1. Verify mode transitions
2. Check phrase display in each mode
3. Confirm settings save/load across reboots
4. Test GPS lock and wardriving CSV export

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

### Background Scanning
WiFi scanning runs in a FreeRTOS background task to keep UI responsive:
- `scanTask()` runs `WiFi.scanNetworks()` on Core 0
- Main loop continues handling display/keyboard
- Scan results processed when task completes (~7 seconds per scan)
- Scan cancelled cleanly on stop (vTaskDelete + WiFi.scanDelete)

### Memory Management
- Max 2000 entries (~240KB) to prevent memory exhaustion
- Auto-saves to CSV and clears when limit reached
- Statistics reset on overflow to stay in sync

### Export Formats
- **CSV**: Simple format with BSSID, SSID, RSSI, channel, auth, GPS coords
- **Wigle**: Compatible with wigle.net uploads
- **Kismet NetXML**: For Kismet-compatible tools
- **ML Training**: 32-feature vector with labels for Edge Impulse

### Data Safety
- SSIDs are properly escaped (CSV quotes, XML entities)
- Control characters stripped from SSID fields
- Periodic ML export every 60s (crash protection)
- SD writes are single-threaded (main loop only, never from scan task)
- Scan cancelled cleanly on stop to prevent orphaned tasks

### Edge Impulse Integration
1. Train model at studio.edgeimpulse.com
2. Export C++ Library for ESP32
3. Copy `edge-impulse-sdk/` to `lib/`
4. Uncomment `#define EDGE_IMPULSE_ENABLED` in `edge_impulse.h`
5. Rebuild - real inference replaces heuristics
