# M5Porkchop Copilot Instructions

## Project Overview

M5Porkchop is a WiFi security research tool for the M5Cardputer (ESP32-S3 based) with a "piglet" mascot personality. It features:
- **OINK Mode**: WiFi scanning, handshake capture, deauth attacks
- **WARHOG Mode**: Wardriving with GPS logging
- Interactive ASCII piglet avatar with mood-based phrases
- Settings menu with persistent configuration

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

### Modes
- `src/modes/oink.cpp/h` - OinkMode: WiFi scanning, channel hopping, promiscuous mode, handshake capture
- `src/modes/warhog.cpp/h` - WarhogMode: GPS-enabled wardriving, multiple export formats (CSV, Wigle, Kismet, ML Training)

### UI Layer
- `src/ui/display.cpp/h` - Triple-buffered canvas system (topBar, mainCanvas, bottomBar), 240x135 display
- `src/ui/menu.cpp/h` - Main menu with callback system
- `src/ui/settings_menu.cpp/h` - Interactive settings with TOGGLE, VALUE, ACTION item types

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
- **O/W/S** - Quick mode shortcuts from IDLE (Oink/Warhog/Settings)

## Mode State Machine

```
PorkchopMode:
  IDLE -> OINK_MODE | WARHOG_MODE | MENU | SETTINGS | CAPTURES | ABOUT
  MENU -> (any mode via menu selection)
  SETTINGS/CAPTURES/ABOUT -> MENU (via Enter or backtick)
```

**Important**: `previousMode` only stores "real" modes (IDLE, OINK_MODE, WARHOG_MODE), never MENU/SETTINGS/ABOUT, to prevent navigation loops.

## Phrase System

Mood phrases are context-aware arrays in `mood.cpp`:
- `PHRASES_HAPPY[]` - On handshake capture
- `PHRASES_HUNTING[]` - During OINK mode scanning
- `PHRASES_WARHOG[]` - During WARHOG mode
- `PHRASES_WARHOG_FOUND[]` - When new AP with GPS is logged
- `PHRASES_SAD[]` - No activity
- `PHRASES_IDLE[]` / `PHRASES_MENU_IDLE[]` - Menu/idle state

Call appropriate `Mood::onXxx()` method to trigger phrase updates.

## Settings Menu

Settings use `SettingItem` struct with types:
- `TOGGLE` - ON/OFF, Enter toggles, arrows navigate
- `VALUE` - Numeric with min/max/step, arrows adjust directly
- `ACTION` - Triggers function (e.g., Save & Exit)

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

## ML System

### Current Architecture
```
WiFi Frame → FeatureExtractor → WiFiFeatures (32 floats) → MLInference → MLResult
                                                              ↓
                                              EdgeImpulse SDK (if enabled)
                                                     OR
                                              Heuristic Classifier
```

### ML Labels
- `NORMAL` (0) - Legitimate network
- `ROGUE_AP` (1) - Suspicious access point (strong signal, odd beacon timing)
- `EVIL_TWIN` (2) - Impersonating known network
- `DEAUTH_TARGET` (3) - Vulnerable to deauth attacks
- `VULNERABLE` (4) - Open/WEP/WPA1 network

### Heuristic Detection Rules
The classifier scores networks on:
- Signal strength anomalies (RSSI > -30 suspicious)
- Non-standard beacon intervals (normal is 100ms)
- Beacon jitter (software APs have higher jitter)
- Missing vendor IEs (real routers have many)
- Open network with WPS (honeypot pattern)
- WPA3/PMF for deauth resistance

### Training Data Collection
In WARHOG mode, features are extracted for each network:
```cpp
WarhogMode::exportMLTraining("/sd/training.csv");
```
Outputs all 32 features + BSSID, SSID, label, GPS coords.

## WARHOG Mode Details

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
- Incremental file appending (no data loss on crash)

### Edge Impulse Integration
1. Train model at studio.edgeimpulse.com
2. Export C++ Library for ESP32
3. Copy `edge-impulse-sdk/` to `lib/`
4. Uncomment `#define EDGE_IMPULSE_ENABLED` in `edge_impulse.h`
5. Rebuild - real inference replaces heuristics
