// Porkchop core state machine implementation

#include "porkchop.h"
#include <M5Cardputer.h>
#include "../ui/display.h"
#include "../ui/menu.h"
#include "../ui/settings_menu.h"
#include "../ui/captures_menu.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "../modes/warhog.h"

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
        {"OINK Mode", 1},
        {"WARHOG Mode", 2},
        {"Captures", 3},
        {"Settings", 4},
        {"About", 5}
    };
    Menu::setItems(mainMenuItems);
    Menu::setTitle("PORKCHOP");
    
    // Menu selection handler
    Menu::setCallback([this](uint8_t actionId) {
        switch (actionId) {
            case 1: setMode(PorkchopMode::OINK_MODE); break;
            case 2: setMode(PorkchopMode::WARHOG_MODE); break;
            case 3: setMode(PorkchopMode::CAPTURES); break;
            case 4: setMode(PorkchopMode::SETTINGS); break;
            case 5: setMode(PorkchopMode::ABOUT); break;
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
    
    // Only save "real" modes as previous (not SETTINGS/ABOUT/MENU/CAPTURES)
    if (currentMode != PorkchopMode::SETTINGS && 
        currentMode != PorkchopMode::ABOUT && 
        currentMode != PorkchopMode::CAPTURES &&
        currentMode != PorkchopMode::MENU) {
        previousMode = currentMode;
    }
    currentMode = mode;
    
    // Cleanup previous mode
    switch (previousMode) {
        case PorkchopMode::OINK_MODE:
            OinkMode::stop();
            break;
        case PorkchopMode::WARHOG_MODE:
            WarhogMode::stop();
            break;
        case PorkchopMode::MENU:
            Menu::hide();
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
            WarhogMode::start();
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
    if (!M5Cardputer.Keyboard.isChange()) return;
    
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
        case PorkchopMode::CAPTURES:
            CapturesMenu::update();
            if (!CapturesMenu::isActive()) {
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
