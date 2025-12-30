// Settings menu implementation

#include "settings_menu.h"
#include "display.h"
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../gps/gps.h"
#include <M5Cardputer.h>
#include <SD.h>

// Static member initialization
std::vector<SettingItem> SettingsMenu::items;
uint8_t SettingsMenu::selectedIndex = 0;
uint8_t SettingsMenu::scrollOffset = 0;
bool SettingsMenu::active = false;
bool SettingsMenu::exitRequested = false;
bool SettingsMenu::keyWasPressed = false;
bool SettingsMenu::editing = false;
bool SettingsMenu::textEditing = false;
String SettingsMenu::textBuffer = "";
uint8_t SettingsMenu::cursorPos = 0;
uint8_t SettingsMenu::origGpsRxPin = 0;
uint8_t SettingsMenu::origGpsTxPin = 0;
uint32_t SettingsMenu::origGpsBaud = 0;

void SettingsMenu::init() {
    loadFromConfig();
}

void SettingsMenu::loadFromConfig() {
    items.clear();
    
    // WiFi SSID for file transfer
    items.push_back({
        "WiFi SSID",
        SettingType::TEXT,
        0, 0, 0, 0, "",
        Config::wifi().otaSSID,
        "Network for file xfer"
    });
    
    // WiFi Password
    items.push_back({
        "WiFi Pass",
        SettingType::TEXT,
        0, 0, 0, 0, "",
        Config::wifi().otaPassword,
        "Secret sauce goes here"
    });
    
    // WPA-SEC Key display (masked) - shows key status
    String keyStatus = Config::wifi().wpaSecKey.isEmpty() ? "(not set)" : 
        Config::wifi().wpaSecKey.substring(0, 4) + "..." + 
        Config::wifi().wpaSecKey.substring(Config::wifi().wpaSecKey.length() - 4);
    items.push_back({
        "WPA-SEC",
        SettingType::TEXT,
        0, 0, 0, 0, "",
        keyStatus,
        "wpa-sec.stanev.org key"
    });
    
    // Load Key File action - reads from /wpasec_key.txt
    items.push_back({
        "< Load Key File >",
        SettingType::ACTION,
        0, 0, 0, 0, "", "",
        "Read /wpasec_key.txt"
    });
    
    // WiGLE API Name display (masked)
    String wigleName = Config::wifi().wigleApiName;
    String wigleNameStatus = wigleName.isEmpty() ? "(not set)" : 
        wigleName.substring(0, min((unsigned int)3, wigleName.length())) + "...";
    items.push_back({
        "WiGLE Name",
        SettingType::TEXT,
        0, 0, 0, 0, "",
        wigleNameStatus,
        "wigle.net API name"
    });
    
    // WiGLE API Token display (masked)
    String wigleToken = Config::wifi().wigleApiToken;
    String wigleTokenStatus;
    if (wigleToken.isEmpty()) {
        wigleTokenStatus = "(not set)";
    } else if (wigleToken.length() < 8) {
        wigleTokenStatus = wigleToken.substring(0, 2) + "...";
    } else {
        wigleTokenStatus = wigleToken.substring(0, 4) + "..." + 
            wigleToken.substring(wigleToken.length() - 4);
    }
    items.push_back({
        "WiGLE Token",
        SettingType::TEXT,
        0, 0, 0, 0, "",
        wigleTokenStatus,
        "wigle.net API token"
    });
    
    // Load WiGLE Key File action - reads from /wigle_key.txt
    items.push_back({
        "< Load WiGLE Key >",
        SettingType::ACTION,
        0, 0, 0, 0, "", "",
        "Read /wigle_key.txt"
    });
    
    // Sound toggle
    items.push_back({
        "Sound",
        SettingType::TOGGLE,
        Config::personality().soundEnabled ? 1 : 0,
        0, 1, 1, "", "",
        "Beeps and boops"
    });
    
    // Brightness (0-100%)
    items.push_back({
        "Brightness",
        SettingType::VALUE,
        (int)Config::personality().brightness,
        10, 100, 10, "%", "",
        "Screen glow level"
    });
    
    // Dim timeout (seconds, 0 = never)
    items.push_back({
        "Dim After",
        SettingType::VALUE,
        (int)Config::personality().dimTimeout,
        0, 300, 10, "s", "",
        "0 = never dim"
    });
    
    // Dim level (0-100%, 0 = screen off)
    items.push_back({
        "Dim Level",
        SettingType::VALUE,
        (int)Config::personality().dimLevel,
        0, 50, 5, "%", "",
        "0 = screen off"
    });
    
    // Color theme (0-11, see THEMES array in display.h)
    items.push_back({
        "Theme",
        SettingType::VALUE,
        (int)Config::personality().themeIndex,
        0, 11, 1, "", "",
        "Cycle colors"
    });
    
    // Channel hop interval
    items.push_back({
        "CH Hop",
        SettingType::VALUE,
        (int)Config::wifi().channelHopInterval,
        100, 2000, 100, "ms", "",
        "Faster = more coverage"
    });
    
    // Lock time (client discovery before attack)
    items.push_back({
        "Lock Time",
        SettingType::VALUE,
        (int)Config::wifi().lockTime,
        1000, 10000, 500, "ms", "",
        "Client sniff time"
    });
    
    // Enable deauth
    items.push_back({
        "Deauth",
        SettingType::TOGGLE,
        Config::wifi().enableDeauth ? 1 : 0,
        0, 1, 1, "", "",
        "Kick clients off APs"
    });
    
    // MAC randomization for stealth
    items.push_back({
        "Rnd MAC",
        SettingType::TOGGLE,
        Config::wifi().randomizeMAC ? 1 : 0,
        0, 1, 1, "", "",
        "New MAC each mode start"
    });
    
    // GPS enabled
    items.push_back({
        "GPS",
        SettingType::TOGGLE,
        Config::gps().enabled ? 1 : 0,
        0, 1, 1, "", "",
        "Position tracking"
    });
    
    // GPS power save
    items.push_back({
        "GPS PwrSave",
        SettingType::TOGGLE,
        Config::gps().powerSave ? 1 : 0,
        0, 1, 1, "", "",
        "Sleep when not hunting"
    });
    
    // GPS Scan Interval (seconds between scans in WARHOG mode)
    items.push_back({
        "Scan Intv",
        SettingType::VALUE,
        (int)Config::gps().updateInterval,
        1, 30, 1, "s", "",
        "WARHOG scan frequency"
    });
    
    // GPS Baud Rate (common values: 9600, 38400, 57600, 115200)
    // Use index 0-3 to represent these values
    int baudIndex = 3;  // Default to 115200
    uint32_t baud = Config::gps().baudRate;
    if (baud == 9600) baudIndex = 0;
    else if (baud == 38400) baudIndex = 1;
    else if (baud == 57600) baudIndex = 2;
    else baudIndex = 3;  // 115200
    
    items.push_back({
        "GPS Baud",
        SettingType::VALUE,
        baudIndex,
        0, 3, 1, "", "",
        "Match your GPS module"
    });
    
    // GPS RX Pin (G1 for Grove, G15 for Cap LoRa868)
    items.push_back({
        "GPS RX Pin",
        SettingType::VALUE,
        (int)Config::gps().rxPin,
        1, 46, 1, "", "",
        "G1=Grove, G15=LoRaCap"
    });
    
    // GPS TX Pin (G2 for Grove, G13 for Cap LoRa868)
    items.push_back({
        "GPS TX Pin",
        SettingType::VALUE,
        (int)Config::gps().txPin,
        1, 46, 1, "", "",
        "G2=Grove, G13=LoRaCap"
    });
    
    // Timezone offset (UTC-12 to UTC+14)
    items.push_back({
        "Timezone",
        SettingType::VALUE,
        (int)Config::gps().timezoneOffset,
        -12, 14, 1, "h", "",
        "UTC offset for logs"
    });
    
    // ML Collection Mode (0=Basic, 1=Enhanced)
    items.push_back({
        "ML Mode",
        SettingType::VALUE,
        static_cast<int>(Config::ml().collectionMode),
        0, 1, 1, "", "",
        "Enhanced = beacon sniff"
    });
    
    // SD Card Logging
    items.push_back({
        "SD Log",
        SettingType::TOGGLE,
        SDLog::isEnabled() ? 1 : 0,
        0, 1, 1, "", "",
        "Debug spam to SD"
    });
    
    // === BLE / PIGGY BLUES Settings ===
    
    // BLE Burst Interval (ms between advertisements)
    items.push_back({
        "BLE Burst",
        SettingType::VALUE,
        (int)Config::ble().burstInterval,
        50, 500, 50, "ms", "",
        "Attack speed"
    });
    
    // BLE Advertisement Duration
    items.push_back({
        "BLE Adv Time",
        SettingType::VALUE,
        (int)Config::ble().advDuration,
        50, 200, 25, "ms", "",
        "Per-packet duration"
    });
    
    // === HOGWASH Settings ===
    
    // Captive Portal (DNS redirect in HOGWASH mode)
    items.push_back({
        "Karma Portal",
        SettingType::TOGGLE,
        Config::wifi().hogwashCaptivePortal ? 1 : 0,
        0, 1, 1, "", "",
        "DNS redirect in HOGWASH"
    });
    
    // SSID Cycle Time (how often to switch SSID in HOGWASH, 1-30 seconds)
    items.push_back({
        "SSID Cycle",
        SettingType::VALUE,
        (int)(Config::wifi().hogwashSSIDCycleMs / 1000),
        1, 30, 1, "s", "",
        "HOGWASH SSID switch time"
    });
    
    // Fixed SSID (Evil Twin mode - if set, disables karma cycling)
    items.push_back({
        "Fixed SSID",
        SettingType::TEXT,
        0, 0, 32, 0, "", Config::wifi().hogwashFixedSSID,
        "Set for Evil Twin (empty=karma)"
    });
    // No Save & Exit button - ESC/backtick auto-saves
}

String SettingsMenu::getSelectedDescription() {
    if (selectedIndex < items.size()) {
        return items[selectedIndex].description;
    }
    return "";
}

void SettingsMenu::saveToConfig() {
    // WiFi settings - SSID, Password from TEXT items (WPA-SEC and WiGLE loaded from file)
    auto& w = Config::wifi();
    w.otaSSID = items[0].textValue;
    w.otaPassword = items[1].textValue;
    // items[2] is WPA-SEC display (read-only), items[3] is Load Key File action
    // items[4] is WiGLE Name (read-only), items[5] is WiGLE Token (read-only)
    // items[6] is Load WiGLE Key action
    w.channelHopInterval = items[12].value;
    w.lockTime = items[13].value;
    w.enableDeauth = items[14].value == 1;
    w.randomizeMAC = items[15].value == 1;
    Config::setWiFi(w);
    
    // Sound, Brightness, Dimming, and Theme
    auto& p = Config::personality();
    p.soundEnabled = items[7].value == 1;
    p.brightness = items[8].value;
    p.dimTimeout = items[9].value;
    p.dimLevel = items[10].value;
    p.themeIndex = items[11].value;
    Config::setPersonality(p);
    
    // Apply brightness to display (if not dimmed, reset timer too)
    Display::resetDimTimer();
    M5.Display.setBrightness(items[8].value * 255 / 100);
    
    // GPS settings
    auto& g = Config::gps();
    g.enabled = items[16].value == 1;
    g.powerSave = items[17].value == 1;
    g.updateInterval = items[18].value;  // Scan interval in seconds
    
    // Convert baud index to actual baud rate
    static const uint32_t baudRates[] = {9600, 38400, 57600, 115200};
    g.baudRate = baudRates[items[19].value];
    
    // GPS RX/TX pins (G1/G2 for Grove, G13/G15 for Cap LoRa868)
    g.rxPin = items[20].value;
    g.txPin = items[21].value;
    
    g.timezoneOffset = items[22].value;
    Config::setGPS(g);
    
    // ML settings
    auto& m = Config::ml();
    m.collectionMode = static_cast<MLCollectionMode>(items[23].value);
    Config::setML(m);
    
    // SD Logging
    SDLog::setEnabled(items[24].value == 1);
    
    // BLE settings (PIGGY BLUES)
    auto& b = Config::ble();
    b.burstInterval = items[25].value;
    b.advDuration = items[26].value;
    Config::setBLE(b);
    
    // HOGWASH settings
    w.hogwashCaptivePortal = items[27].value == 1;
    w.hogwashSSIDCycleMs = items[28].value * 1000;  // Convert seconds to ms
    strncpy(w.hogwashFixedSSID, items[29].textValue.c_str(), 32);
    w.hogwashFixedSSID[32] = '\0';
    Config::setWiFi(w);
    
    // Save to file
    Config::save();
}

void SettingsMenu::show() {
    active = true;
    exitRequested = false;
    selectedIndex = 0;
    scrollOffset = 0;
    editing = false;
    textEditing = false;
    textBuffer = "";
    cursorPos = 0;
    keyWasPressed = true;  // Ignore the Enter that selected us from menu
    
    // Store original GPS config to detect changes
    origGpsRxPin = Config::gps().rxPin;
    origGpsTxPin = Config::gps().txPin;
    origGpsBaud = Config::gps().baudRate;
    
    loadFromConfig();
}

void SettingsMenu::hide() {
    active = false;
    editing = false;
}

void SettingsMenu::update() {
    if (!active) return;
    handleInput();
}

void SettingsMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    // Handle text input mode separately
    if (textEditing) {
        handleTextInput();
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    auto& item = items[selectedIndex];
    
    // Navigation with ; (up) and . (down)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (editing && item.type == SettingType::VALUE) {
            // Adjust value UP when pressing up key
            item.value = min(item.maxVal, item.value + item.step);
        } else {
            // Move selection up
            editing = false;
            if (selectedIndex > 0) {
                selectedIndex--;
                if (selectedIndex < scrollOffset) {
                    scrollOffset = selectedIndex;
                }
            }
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (editing && item.type == SettingType::VALUE) {
            // Adjust value DOWN when pressing down key
            item.value = max(item.minVal, item.value - item.step);
        } else {
            // Move selection down
            editing = false;
            if (selectedIndex < items.size() - 1) {
                selectedIndex++;
                if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                    scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
                }
            }
        }
    }
    
    // Enter to select/toggle/confirm
    if (keys.enter) {
        if (item.type == SettingType::ACTION) {
            if (item.label == "< Load Key File >") {
                // Load WPA-SEC key from file
                if (Config::loadWpaSecKeyFromFile()) {
                    Display::showToast("Key loaded!");
                    // Refresh the display to show masked key
                    loadFromConfig();
                } else {
                    if (!Config::isSDAvailable()) {
                        Display::showToast("No SD card");
                    } else if (!SD.exists("/wpasec_key.txt")) {
                        Display::showToast("No key file");
                    } else {
                        Display::showToast("Invalid key");
                    }
                }
            } else if (item.label == "< Load WiGLE Key >") {
                // Load WiGLE API credentials from file
                if (Config::loadWigleKeyFromFile()) {
                    Display::showToast("WiGLE key loaded!");
                    // Refresh the display to show masked credentials
                    loadFromConfig();
                } else {
                    if (!Config::isSDAvailable()) {
                        Display::showToast("No SD card");
                    } else if (!SD.exists("/wigle_key.txt")) {
                        Display::showToast("No key file");
                    } else {
                        Display::showToast("Invalid format");
                    }
                }
            }
        } else if (item.type == SettingType::TOGGLE) {
            // Toggle ON/OFF
            item.value = item.value == 0 ? 1 : 0;
        } else if (item.type == SettingType::VALUE) {
            if (editing) {
                // Confirm and exit edit mode
                editing = false;
            } else {
                // Enter edit mode
                editing = true;
            }
        } else if (item.type == SettingType::TEXT) {
            // Skip text editing for read-only display fields
            if (item.label == "WPA-SEC" || item.label == "WiGLE Name" || item.label == "WiGLE Token") {
                // Do nothing - these are display only (loaded from file)
            } else {
                // Enter text editing mode
                textEditing = true;
                textBuffer = item.textValue;
                cursorPos = textBuffer.length();
                keyWasPressed = true;  // Prevent immediate character input
            }
        }
    }
    
    // ESC/backtick to exit edit mode or save and exit menu
    if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        if (editing) {
            editing = false;
        } else {
            saveToConfig();
            
            // Re-init GPS only if pins or baud changed
            if (Config::gps().enabled) {
                bool gpsChanged = (Config::gps().rxPin != origGpsRxPin) ||
                                  (Config::gps().txPin != origGpsTxPin) ||
                                  (Config::gps().baudRate != origGpsBaud);
                if (gpsChanged) {
                    GPS::reinit(Config::gps().rxPin, Config::gps().txPin, Config::gps().baudRate);
                    Display::showToast("GPS reinit");
                    delay(500);
                }
            }
            
            Display::showToast("Saved");
            delay(500);  // Show toast for 500ms before exiting
            exitRequested = true;
        }
    }
}

void SettingsMenu::handleTextInput() {
    // Get keyboard state first - this captures the current state including shift
    auto keys = M5Cardputer.Keyboard.keysState();
    auto& item = items[selectedIndex];
    
    // Check if any key is currently pressed
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    // For text input, we need to debounce based on whether we have printable chars,
    // not just any key press. This allows Shift to be held while typing.
    bool hasPrintableChar = !keys.word.empty();
    bool hasActionKey = keys.enter || keys.del;
    
    // Only debounce if we have something to process
    if (!hasPrintableChar && !hasActionKey) {
        // Just modifier keys pressed (shift, fn, etc.) - don't set debounce
        return;
    }
    
    // Debounce - only act on key down edge for actual characters/actions
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    // Enter to confirm text
    if (keys.enter) {
        item.textValue = textBuffer;
        textEditing = false;
        textBuffer = "";
        cursorPos = 0;
        return;
    }
    
    // Backspace to delete character
    if (keys.del) {
        if (textBuffer.length() > 0) {
            textBuffer.remove(textBuffer.length() - 1);
            cursorPos = textBuffer.length();
        }
        return;
    }
    
    // Check for backtick to cancel
    for (char c : keys.word) {
        if (c == '`') {
            textEditing = false;
            textBuffer = "";
            cursorPos = 0;
            return;
        }
    }
    
    // Add typed characters from word vector
    // The keyboard library handles shift/fn for uppercase and special chars
    if (textBuffer.length() < 32) {
        for (char c : keys.word) {
            // Accept all printable ASCII characters except backtick
            if (c >= 32 && c <= 126 && c != '`' && textBuffer.length() < 32) {
                textBuffer += c;
            }
        }
        cursorPos = textBuffer.length();
    }
}

void SettingsMenu::draw(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    
    int y = 2;
    int lineHeight = 16;
    
    for (uint8_t i = 0; i < VISIBLE_ITEMS && (scrollOffset + i) < items.size(); i++) {
        uint8_t idx = scrollOffset + i;
        auto& item = items[idx];
        
        bool isSelected = (idx == selectedIndex);
        
        // Highlight selected row
        if (isSelected) {
            canvas.fillRect(0, y, DISPLAY_W, lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // Draw label
        canvas.setTextDatum(top_left);
        canvas.drawString(item.label, 4, y + 2);
        
        // Draw value on the right
        String valStr;
        if (item.type == SettingType::TOGGLE) {
            valStr = item.value ? "ON" : "OFF";
        } else if (item.type == SettingType::VALUE) {
            // Special handling for GPS Baud rate (display actual baud instead of index)
            if (item.label == "GPS Baud") {
                static const char* baudLabels[] = {"9600", "38400", "57600", "115200"};
                if (isSelected && editing) {
                    valStr = "[" + String(baudLabels[item.value]) + "]";
                } else {
                    valStr = String(baudLabels[item.value]);
                }
            // Special handling for ML Mode (display Basic/Enhanced instead of 0/1)
            } else if (item.label == "ML Mode") {
                static const char* modeLabels[] = {"Basic", "Enhanced"};
                if (isSelected && editing) {
                    valStr = "[" + String(modeLabels[item.value]) + "]";
                } else {
                    valStr = String(modeLabels[item.value]);
                }
            // Special handling for Theme (display theme name instead of index)
            } else if (item.label == "Theme") {
                const char* themeName = THEMES[item.value].name;
                if (isSelected && editing) {
                    valStr = "[" + String(themeName) + "]";
                } else {
                    valStr = String(themeName);
                }
            } else if (isSelected && editing) {
                valStr = "[" + String(item.value) + item.suffix + "]";
            } else {
                valStr = String(item.value) + item.suffix;
            }
        } else if (item.type == SettingType::TEXT) {
            if (isSelected && textEditing) {
                // Show text with cursor
                String display = textBuffer;
                if (display.length() > 12) {
                    display = "..." + display.substring(display.length() - 9);
                }
                valStr = "[" + display + "_]";
            } else {
                // Show stored value, masked for password
                if (item.label.indexOf("Pass") >= 0 && item.textValue.length() > 0) {
                    valStr = "****";
                } else if (item.textValue.length() > 12) {
                    valStr = item.textValue.substring(0, 9) + "...";
                } else if (item.textValue.length() > 0) {
                    valStr = item.textValue;
                } else {
                    valStr = "<empty>";
                }
            }
        } else if (item.type == SettingType::ACTION) {
            // Action - label is the whole thing, centered
            valStr = "";
        }
        
        if (valStr.length() > 0) {
            canvas.setTextDatum(top_right);
            canvas.drawString(valStr, DISPLAY_W - 4, y + 2);
        }
        
        y += lineHeight;
    }
    
    // Scroll indicators
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    if (scrollOffset > 0) {
        canvas.drawString("^", DISPLAY_W / 2, 0);
    }
    if (scrollOffset + VISIBLE_ITEMS < items.size()) {
        canvas.drawString("v", DISPLAY_W / 2, MAIN_H - 10);
    }
}
