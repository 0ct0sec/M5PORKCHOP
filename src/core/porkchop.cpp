// Porkchop core state machine implementation

#include "porkchop.h"
#include <M5Cardputer.h>
#include "../ui/display.h"
#include "../ui/menu.h"
#include "../ui/settings_menu.h"
#include "../ui/captures_menu.h"
#include "../ui/log_viewer.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "../modes/warhog.h"
#include "../modes/piggyblues.h"
#include "../web/fileserver.h"
#include "config.h"

Porkchop::Porkchop() 
    : currentMode(PorkchopMode::IDLE)
    , previousMode(PorkchopMode::IDLE)
    , startTime(0)
    , handshakeCount(0)
    , networkCount(0)
    , deauthCount(0) {
}

void Porkchop::init() {
    startTime = millis();
    
    // Register default event handlers
    registerCallback(PorkchopEvent::HANDSHAKE_CAPTURED, [this](PorkchopEvent, void*) {
        handshakeCount++;
    });
    
    registerCallback(PorkchopEvent::NETWORK_FOUND, [this](PorkchopEvent, void*) {
        networkCount++;
    });
    
    registerCallback(PorkchopEvent::DEAUTH_SENT, [this](PorkchopEvent, void*) {
        deauthCount++;
    });
    
    // Setup main menu with callback
    std::vector<MenuItem> mainMenuItems = {
        {"OINK Mode", 1, "Hunt for handshakes"},
        {"WARHOG Mode", 2, "Wardrive with GPS"},
        {"PIGGY BLUES Mode", 8, "BLE notification spam"},
        {"File Transfer", 3, "WiFi file server"},
        {"Captures", 4, "View saved loot"},
        {"Log Viewer", 7, "Debug log tail"},
        {"Settings", 5, "Tweak the pig"},
        {"About", 6, "Credits and info"}
    };
    Menu::setItems(mainMenuItems);
    Menu::setTitle("PORKCHOP");
    
    // Menu selection handler
    Menu::setCallback([this](uint8_t actionId) {
        switch (actionId) {
            case 1: setMode(PorkchopMode::OINK_MODE); break;
            case 2: setMode(PorkchopMode::WARHOG_MODE); break;
            case 3: setMode(PorkchopMode::FILE_TRANSFER); break;
            case 4: setMode(PorkchopMode::CAPTURES); break;
            case 5: setMode(PorkchopMode::SETTINGS); break;
            case 6: setMode(PorkchopMode::ABOUT); break;
            case 7: setMode(PorkchopMode::LOG_VIEWER); break;
            case 8: setMode(PorkchopMode::PIGGYBLUES_MODE); break;
        }
        Menu::clearSelected();
    });
    
    Avatar::setState(AvatarState::HAPPY);
    
    Serial.println("[PORKCHOP] Initialized");
}

void Porkchop::update() {
    processEvents();
    handleInput();
    updateMode();
}

void Porkchop::setMode(PorkchopMode mode) {
    if (mode == currentMode) return;
    
    // Store the mode we're leaving for cleanup
    PorkchopMode oldMode = currentMode;
    
    // Only save "real" modes as previous (not SETTINGS/ABOUT/MENU/CAPTURES/FILE_TRANSFER/LOG_VIEWER)
    if (currentMode != PorkchopMode::SETTINGS && 
        currentMode != PorkchopMode::ABOUT && 
        currentMode != PorkchopMode::CAPTURES &&
        currentMode != PorkchopMode::MENU &&
        currentMode != PorkchopMode::FILE_TRANSFER &&
        currentMode != PorkchopMode::LOG_VIEWER) {
        previousMode = currentMode;
    }
    currentMode = mode;
    
    // Cleanup the mode we're actually leaving (oldMode), not previousMode
    switch (oldMode) {
        case PorkchopMode::OINK_MODE:
            OinkMode::stop();
            break;
        case PorkchopMode::WARHOG_MODE:
            WarhogMode::stop();
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            PiggyBluesMode::stop();
            break;
        case PorkchopMode::MENU:
            Menu::hide();
            break;
        case PorkchopMode::SETTINGS:
            SettingsMenu::hide();
            break;
        case PorkchopMode::CAPTURES:
            CapturesMenu::hide();
            break;
        case PorkchopMode::FILE_TRANSFER:
            FileServer::stop();
            break;
        case PorkchopMode::LOG_VIEWER:
            LogViewer::hide();
            break;
        default:
            break;
    }
    
    // Init new mode
    switch (currentMode) {
        case PorkchopMode::IDLE:
            Avatar::setState(AvatarState::NEUTRAL);
            Mood::onIdle();
            break;
        case PorkchopMode::OINK_MODE:
            Avatar::setState(AvatarState::HUNTING);
            OinkMode::start();
            break;
        case PorkchopMode::WARHOG_MODE:
            Avatar::setState(AvatarState::EXCITED);
            Display::showToast("Sniffing the air...");
            WarhogMode::start();
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            Avatar::setState(AvatarState::ANGRY);
            PiggyBluesMode::start();
            // If user aborted warning dialog, return to menu
            if (!PiggyBluesMode::isRunning()) {
                currentMode = PorkchopMode::MENU;
                Menu::show();
            }
            break;
        case PorkchopMode::MENU:
            Menu::show();
            break;
        case PorkchopMode::SETTINGS:
            SettingsMenu::show();
            break;
        case PorkchopMode::CAPTURES:
            CapturesMenu::show();
            break;
        case PorkchopMode::FILE_TRANSFER:
            Avatar::setState(AvatarState::HAPPY);
            FileServer::start(Config::wifi().otaSSID.c_str(), Config::wifi().otaPassword.c_str());
            break;
        case PorkchopMode::LOG_VIEWER:
            LogViewer::show();
            break;
        default:
            break;
    }
    
    postEvent(PorkchopEvent::MODE_CHANGE, nullptr);
}

void Porkchop::postEvent(PorkchopEvent event, void* data) {
    eventQueue.push_back({event, data});
}

void Porkchop::registerCallback(PorkchopEvent event, EventCallback callback) {
    callbacks.push_back({event, callback});
}

void Porkchop::processEvents() {
    for (const auto& item : eventQueue) {
        for (const auto& cb : callbacks) {
            if (cb.first == item.event) {
                cb.second(item.event, item.data);
            }
        }
    }
    eventQueue.clear();
}

void Porkchop::handleInput() {
    // G0 button (GPIO0 on top side) - always returns to IDLE from any mode
    // Try multiple detection methods for compatibility
    static bool g0WasPressed = false;
    bool g0Pressed = (digitalRead(0) == LOW);  // G0 is active LOW
    
    if (g0Pressed && !g0WasPressed) {
        Display::resetDimTimer();  // Wake screen on G0
        Serial.printf("[PORKCHOP] G0 pressed! Current mode: %d\n", (int)currentMode);
        if (currentMode != PorkchopMode::IDLE) {
            Serial.println("[PORKCHOP] Returning to IDLE");
            setMode(PorkchopMode::IDLE);
            g0WasPressed = true;
            return;
        }
    }
    if (!g0Pressed) {
        g0WasPressed = false;
    }
    
    if (!M5Cardputer.Keyboard.isChange()) return;
    
    // Any keyboard input resets the screen dim timer
    Display::resetDimTimer();
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    // In MENU mode, only handle backtick to exit
    // Let Menu::handleInput() process navigation keys
    if (currentMode == PorkchopMode::MENU) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            setMode(previousMode);
        }
        // Do NOT return here - let Menu::update() handle navigation
        // But we already consumed isChange(), so Menu won't see it
        // Instead, call Menu::update() directly here
        Menu::update();
        return;
    }
    
    // In SETTINGS mode, let SettingsMenu handle everything
    if (currentMode == PorkchopMode::SETTINGS) {
        // Check if settings wants to exit
        if (SettingsMenu::shouldExit()) {
            SettingsMenu::clearExit();
            SettingsMenu::hide();
            setMode(PorkchopMode::MENU);
        }
        return;
    }
    
    // Menu toggle with backtick
    if (M5Cardputer.Keyboard.isKeyPressed('`')) {
        setMode(PorkchopMode::MENU);
        return;
    }
    
    // Enter key to go back from About
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        if (currentMode == PorkchopMode::ABOUT) {
            setMode(PorkchopMode::MENU);
            return;
        }
    }
    
    // Mode shortcuts when in IDLE
    if (currentMode == PorkchopMode::IDLE) {
        for (auto c : keys.word) {
            switch (c) {
                case 'o': // Oink mode
                case 'O':
                    setMode(PorkchopMode::OINK_MODE);
                    break;
                case 'w': // Warhog mode
                case 'W':
                    setMode(PorkchopMode::WARHOG_MODE);
                    break;
                case 'b': // Piggy Blues mode
                case 'B':
                    setMode(PorkchopMode::PIGGYBLUES_MODE);
                    break;
                case 's': // Settings
                case 'S':
                    setMode(PorkchopMode::SETTINGS);
                    break;
            }
        }
    }
    
    // OINK mode - Backspace to stop and return to idle
    if (currentMode == PorkchopMode::OINK_MODE) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            setMode(PorkchopMode::IDLE);
            return;
        }
    }
    
    // FILE_TRANSFER mode - Backspace to stop and return to menu
    if (currentMode == PorkchopMode::FILE_TRANSFER) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            setMode(PorkchopMode::MENU);
            return;
        }
    }
    
    // ESC (fn+backtick) to return to idle
    if (keys.fn && M5Cardputer.Keyboard.isKeyPressed('`')) {
        setMode(PorkchopMode::IDLE);
    }
}

void Porkchop::updateMode() {
    switch (currentMode) {
        case PorkchopMode::OINK_MODE:
            OinkMode::update();
            break;
        case PorkchopMode::WARHOG_MODE:
            WarhogMode::update();
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            PiggyBluesMode::update();
            break;
        case PorkchopMode::CAPTURES:
            CapturesMenu::update();
            if (!CapturesMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::FILE_TRANSFER:
            FileServer::update();
            break;
        case PorkchopMode::LOG_VIEWER:
            LogViewer::update();
            if (!LogViewer::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        default:
            break;
    }
}

uint32_t Porkchop::getUptime() const {
    return (millis() - startTime) / 1000;
}

uint16_t Porkchop::getHandshakeCount() const {
    return OinkMode::getCompleteHandshakeCount();
}

uint16_t Porkchop::getNetworkCount() const {
    return OinkMode::getNetworkCount();
}

uint16_t Porkchop::getDeauthCount() const {
    return OinkMode::getDeauthCount();
}
