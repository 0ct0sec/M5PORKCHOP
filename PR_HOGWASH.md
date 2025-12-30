# 🐷 HOGWASH Mode - Karma AP Implementation

## Summary

This PR introduces **HOGWASH Mode**, a new operational mode that implements a Karma Access Point attack. The pig listens for WiFi probe requests from nearby devices, extracts the SSIDs they're looking for, and broadcasts a matching fake AP to lure them in.

## 🎯 Features

### Core Karma AP Functionality
- **Probe Request Capture**: Monitors WiFi probe requests in promiscuous mode
- **SSID Cycling**: Broadcasts as the last probed SSID to attract devices
- **Smart Rotation**: SSID cycling pauses while clients are connected (10s disconnect detection)
- **Hook Detection**: Tracks connected stations with Apple device identification
- **MAC Randomization Detection**: Shows `[RandomMAC]` when devices use randomized addresses

### Captive Portal
- **DNS Redirect**: All domains redirect to the AP (192.168.4.1)
- **HTTP Server**: Serves portal HTML on port 80
- **Custom Portal**: Drop `/portal.html` on SD card for custom pages
- **Platform Detection**: Handles Android, iOS, macOS, and Windows captive portal endpoints

### XP & Achievements System
| Event | XP | Cap |
|-------|-----|-----|
| `HOGWASH_PROBE_NEW` | +3 | 200/session |
| `HOGWASH_HOOK` | +25 | - |
| `HOGWASH_APPLE_HOOK` | +35 | - |
| `HOGWASH_SESSION_5MIN` | +10 | - |

**6 New Achievements:**
- `F1RST H00K` - First device hooked
- `K4RMA K1NG` - 50 devices hooked lifetime
- `H0N3Y P0T` - 5 devices connected simultaneously  
- `TR4P M4ST3R` - 100 unique SSIDs captured
- `4PPL3 P1CK3R` - Hook 10 Apple devices
- `TR4FF1C W4RD3N` - 30 minutes continuous operation

### Avatar & Mood
- **DEVIOUS State**: New sneaky avatar face (`> >` eyes, `~` expression)
- **Probe Phrases**: "I am [SSID] now", "looking for [SSID]?", etc.
- **Hook Phrases**: "GOTCHA!", "yoink", "GET OVER HERE"
- **Mixed Status/Flavor**: 60% stats, 40% flavor phrases during operation

### UI & Settings
- **Bottom Bar**: Shows `P:probes U:unique H:hooked [SSID]`
- **Sound Notifications**: Double beep for hooks, triple for Apple devices
- **Settings**:
  - `Karma Portal`: Toggle captive portal (default: OFF)
  - `SSID Cycle`: 1-30 seconds (default: 5s)

## 📁 Files Changed

### New Files
- `src/modes/hogwash.cpp` - Core implementation (~700 lines)
- `src/modes/hogwash.h` - Mode interface and structures

### Modified Files
- `src/core/porkchop.cpp/.h` - Mode integration (K key binding)
- `src/core/xp.cpp/.h` - XP events and achievements2 field
- `src/core/config.cpp/.h` - Settings (portal toggle, cycle time)
- `src/piglet/avatar.cpp/.h` - DEVIOUS state and frames
- `src/piglet/mood.cpp/.h` - Phrases and onHogwashUpdate()
- `src/ui/display.cpp` - UI elements and bottom bar
- `src/ui/settings_menu.cpp` - New settings items
- `README.md` - Full documentation

### Tests
- `test/test_hogwash/test_hogwash.cpp` - 33 test cases covering:
  - SSID queue operations
  - Probe request parsing
  - Achievement2 bitfield operations
  - XP anti-farm cap logic
  - Captive portal HTML loading
  - Platform endpoint detection

## 🛡️ Technical Details

### WiFi Stack
- Uses ESP32 promiscuous mode for probe capture
- Soft AP with dynamic SSID switching
- `esp_wifi_set_inactive_time(10)` for fast disconnect detection
- Mixed ESP-IDF and Arduino WiFi API usage

### Memory
- RAM: 23.1% (75.5KB / 327KB)
- Flash: 53.7% (1.69MB / 3.15MB)

### Key Design Decisions
1. **Deferred XP**: Probe callback runs in WiFi task; XP grants queued for main loop
2. **achievements2 field**: Uses second uint64_t for HOGWASH achievements (NVS compatible)
3. **Station Count**: Uses `WiFi.softAPgetStationNum()` for real-time client detection
4. **Inactivity Timeout**: 10 seconds instead of default 300s for responsive cycling

## ⚠️ Disclaimer

HOGWASH mode demonstrates Karma AP attacks for **educational purposes only**. Use responsibly on your own devices. The same legal considerations as PIGGY BLUES apply.

## 📸 Screenshots

The pig displays the DEVIOUS face throughout HOGWASH operation:
```
 >  > 
(~ 00)
(    )
```

## Testing

```bash
# Run HOGWASH tests
pio test -e native --filter "test_hogwash"

# Build for device
pio run -e m5cardputer

# Flash and monitor
pio run -e m5cardputer -t upload && pio device monitor -b 115200
```

## Commits (17 total)

See individual commit messages for detailed changes. Key commits:
- `a2e26a4` - Core implementation
- `38e89f9` - Captive portal
- `072af77` - SSID rotation fixes
- `88f00c7` - DEVIOUS avatar persistence
