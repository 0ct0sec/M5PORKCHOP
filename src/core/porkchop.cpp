// Porkchop core state machine implementation

#include "porkchop.h"
#include <M5Cardputer.h>
#include "../ui/display.h"
#include "../ui/menu.h"
#include "../ui/settings_menu.h"
#include "../ui/captures_menu.h"
#include "../ui/achievements_menu.h"
#include "../ui/log_viewer.h"
#include "../ui/swine_stats.h"
#include "../ui/boar_bros_menu.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "../modes/warhog.h"
#include "../modes/piggyblues.h"
#include "../modes/spectrum.h"
#include "../web/fileserver.h"
#include "config.h"
#include "xp.h"

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
    
    // Initialize XP system
    XP::init();
    
    // Initialize SwineStats (buff/debuff system)
    SwineStats::init();
    
    // Register level up callback to show popup
    XP::setLevelUpCallback([](uint8_t oldLevel, uint8_t newLevel) {
        Display::showLevelUp(oldLevel, newLevel);
        
        // Check if class tier changed (every 5 levels: 6, 11, 16, 21, 26, 31, 36)
        PorkClass oldClass = XP::getClassForLevel(oldLevel);
        PorkClass newClass = XP::getClassForLevel(newLevel);
        if (newClass != oldClass) {
            // Small delay between popups
            delay(500);
            Display::showClassPromotion(
                XP::getClassNameFor(oldClass),
                XP::getClassNameFor(newClass)
            );
        }
    });
    
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
    // Order: Modes -> Data/Stats -> Services
    std::vector<MenuItem> mainMenuItems = {
        // === MODES ===
        {"OINK", 1, "Hunt for handshakes"},
        {"WARHOG", 2, "Wardrive with GPS"},
        {"PIGGY BLUES", 8, "BLE notification spam"},
        {"HOG ON SPECTRUM", 10, "WiFi spectrum analyzer"},
        // === DATA & STATS ===
        {"SWINE STATS", 11, "Lifetime stats & buffs"},
        {"LOOT", 4, "View saved loot"},
        {"BOAR BROS", 12, "Manage friendly networks"},
        {"ACHIEVEMENTS", 9, "Proof of pwn"},
        // === SERVICES ===
        {"FILE TRANSFER", 3, "WiFi file server"},
        {"LOG VIEWER", 7, "Debug log tail"},
        {"SETTINGS", 5, "Tweak the pig"},
        {"ABOUT", 6, "Credits and info"}
    };
    Menu::setItems(mainMenuItems);
    Menu::setTitle("PORKCHOP OS");
    
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
            case 9: setMode(PorkchopMode::ACHIEVEMENTS); break;
            case 10: setMode(PorkchopMode::SPECTRUM_MODE); break;
            case 11: setMode(PorkchopMode::SWINE_STATS); break;
            case 12: setMode(PorkchopMode::BOAR_BROS); break;
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
    
    // Check for session time XP bonuses
    XP::updateSessionTime();
}

void Porkchop::setMode(PorkchopMode mode) {
    if (mode == currentMode) return;
    
    // Store the mode we're leaving for cleanup
    PorkchopMode oldMode = currentMode;
    
    // Only save "real" modes as previous (not SETTINGS/ABOUT/MENU/CAPTURES/ACHIEVEMENTS/FILE_TRANSFER/LOG_VIEWER/SWINE_STATS/BOAR_BROS)
    if (currentMode != PorkchopMode::SETTINGS && 
        currentMode != PorkchopMode::ABOUT && 
        currentMode != PorkchopMode::CAPTURES &&
        currentMode != PorkchopMode::ACHIEVEMENTS &&
        currentMode != PorkchopMode::MENU &&
        currentMode != PorkchopMode::FILE_TRANSFER &&
        currentMode != PorkchopMode::LOG_VIEWER &&
        currentMode != PorkchopMode::SWINE_STATS &&
        currentMode != PorkchopMode::BOAR_BROS) {
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
        case PorkchopMode::SPECTRUM_MODE:
            SpectrumMode::stop();
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
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::hide();
            break;
        case PorkchopMode::FILE_TRANSFER:
            FileServer::stop();
            break;
        case PorkchopMode::LOG_VIEWER:
            LogViewer::hide();
            break;
        case PorkchopMode::SWINE_STATS:
            SwineStats::hide();
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::hide();
            break;
        default:
            break;
    }
    
    // Init new mode
    switch (currentMode) {
        case PorkchopMode::IDLE:
            Avatar::setState(AvatarState::NEUTRAL);
            Mood::onIdle();
            XP::save();  // Save XP when returning to idle
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
        case PorkchopMode::SPECTRUM_MODE:
            Avatar::setState(AvatarState::HUNTING);
            SpectrumMode::start();
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
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::show();
            break;
        case PorkchopMode::FILE_TRANSFER:
            Avatar::setState(AvatarState::HAPPY);
            FileServer::start(Config::wifi().otaSSID.c_str(), Config::wifi().otaPassword.c_str());
            break;
        case PorkchopMode::LOG_VIEWER:
            LogViewer::show();
            break;
        case PorkchopMode::SWINE_STATS:
            SwineStats::show();
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::show();
            break;
        case PorkchopMode::ABOUT:
            Display::resetAboutState();
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
    
    // Screenshot with P key (global, works in any mode)
    if (M5Cardputer.Keyboard.isKeyPressed('p') || M5Cardputer.Keyboard.isKeyPressed('P')) {
        if (!Display::isSnapping()) {
            Display::takeScreenshot();
        }
        return;
    }
    
    // Enter key in About mode - easter egg
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        if (currentMode == PorkchopMode::ABOUT) {
            Display::onAboutEnterPressed();
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
                case 'h': // HOG ON SPECTRUM mode
                case 'H':
                    setMode(PorkchopMode::SPECTRUM_MODE);
                    break;
                case 's': // SWINE STATS
                case 'S':
                    setMode(PorkchopMode::SWINE_STATS);
                    break;
                case 't': // Settings (Tweak)
                case 'T':
                    setMode(PorkchopMode::SETTINGS);
                    break;
            }
        }
    }
    
    // OINK mode - Backspace to stop and return to idle, E to exclude network
    if (currentMode == PorkchopMode::OINK_MODE) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            setMode(PorkchopMode::IDLE);
            return;
        }
        // E key - add selected network to BOAR BROS exclusion list
        static bool eWasPressed = false;
        bool ePressed = M5Cardputer.Keyboard.isKeyPressed('e') || M5Cardputer.Keyboard.isKeyPressed('E');
        if (ePressed && !eWasPressed) {
            int idx = OinkMode::getSelectionIndex();
            if (OinkMode::excludeNetwork(idx)) {
                Display::showToast("BOAR BRO added!");
                delay(500);
                OinkMode::moveSelectionDown();
            } else {
                Display::showToast("Already a bro");
                delay(500);
            }
        }
        eWasPressed = ePressed;
    }
    
    // WARHOG mode - Backspace to stop and return to idle
    if (currentMode == PorkchopMode::WARHOG_MODE) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            setMode(PorkchopMode::IDLE);
            return;
        }
    }
    
    // PIGGYBLUES mode - Backspace to stop and return to idle
    if (currentMode == PorkchopMode::PIGGYBLUES_MODE) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            setMode(PorkchopMode::IDLE);
            return;
        }
    }
    
    // SPECTRUM mode - Backspace to stop and return to idle
    if (currentMode == PorkchopMode::SPECTRUM_MODE) {
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
        case PorkchopMode::SPECTRUM_MODE:
            SpectrumMode::update();
            break;
        case PorkchopMode::CAPTURES:
            CapturesMenu::update();
            if (!CapturesMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::update();
            if (!AchievementsMenu::isActive()) {
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
        case PorkchopMode::SWINE_STATS:
            SwineStats::update();
            if (!SwineStats::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::update();
            if (!BoarBrosMenu::isActive()) {
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
    // Include both handshakes and PMKIDs - both are crackable captures
    return OinkMode::getCompleteHandshakeCount() + OinkMode::getPMKIDCount();
}

uint16_t Porkchop::getNetworkCount() const {
    return OinkMode::getNetworkCount();
}

uint16_t Porkchop::getDeauthCount() const {
    return OinkMode::getDeauthCount();
}
