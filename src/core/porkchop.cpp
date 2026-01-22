// Porkchop core state machine implementation

#include "porkchop.h"
#include <M5Cardputer.h>
#include "../ui/display.h"
#include "../ui/menu.h"
#include "../ui/settings_menu.h"
#include "../ui/captures_menu.h"
#include "../ui/achievements_menu.h"
#include "../ui/bounty_status_menu.h"
#include "../ui/crash_viewer.h"
#include "../ui/diagnostics_menu.h"
#include "../ui/swine_stats.h"
#include "../ui/boar_bros_menu.h"
#include "../ui/wigle_menu.h"
#include "../ui/unlockables_menu.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "../modes/donoham.h"
#include "../modes/warhog.h"
#include "../modes/piggyblues.h"
#include "../modes/spectrum.h"
#include "../modes/pigsync_client.h"
#include "../modes/marco.h"
#include "../web/fileserver.h"
#include "../audio/sfx.h"
#include "config.h"
#include "xp.h"
#include "sdlog.h"
#include "challenges.h"
#include "stress_test.h"

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
        Avatar::cuteJump();  // Celebratory jump on level up!
        
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
    
    // Menu selection handler - items now defined in menu.cpp as static arrays
    Menu::setCallback([this](uint8_t actionId) {
        switch (actionId) {
            case 1: setMode(PorkchopMode::OINK_MODE); break;
            case 2: setMode(PorkchopMode::WARHOG_MODE); break;
            case 3: setMode(PorkchopMode::FILE_TRANSFER); break;
            case 4: setMode(PorkchopMode::CAPTURES); break;
            case 5: setMode(PorkchopMode::SETTINGS); break;
            case 6: setMode(PorkchopMode::ABOUT); break;
            case 7: setMode(PorkchopMode::CRASH_VIEWER); break;
            case 8: setMode(PorkchopMode::PIGGYBLUES_MODE); break;
            case 9: setMode(PorkchopMode::ACHIEVEMENTS); break;
            case 10: setMode(PorkchopMode::SPECTRUM_MODE); break;
            case 11: setMode(PorkchopMode::SWINE_STATS); break;
            case 12: setMode(PorkchopMode::BOAR_BROS); break;
            case 13: setMode(PorkchopMode::WIGLE_MENU); break;
            case 14: setMode(PorkchopMode::DNH_MODE); break;
            case 15: setMode(PorkchopMode::UNLOCKABLES); break;
            case 16: setMode(PorkchopMode::PIGSYNC_DEVICE_SELECT); break;
            case 17: setMode(PorkchopMode::BOUNTY_STATUS); break;
            case 18: setMode(PorkchopMode::MARCO_MODE); break;
            case 19: setMode(PorkchopMode::DIAGNOSTICS); break;
        }
    });
    
    Avatar::setState(AvatarState::HAPPY);
    
    // Initialize non-blocking audio system
    SFX::init();
    
    Serial.println("[PORKCHOP] Initialized");
    SDLog::log("PORK", "Initialized - LV%d %s", XP::getLevel(), XP::getTitle());
}

void Porkchop::update() {
    processEvents();
    handleInput();
    updateMode();
    
    // Tick non-blocking audio engine
    SFX::update();
    
    // Process one queued achievement celebration (debounced)
    XP::processAchievementQueue();
    
    // Stress test injection (if active)
    StressTest::update();
    
    // Check for session time XP bonuses
    XP::updateSessionTime();
}

void Porkchop::setMode(PorkchopMode mode) {
    if (mode == currentMode) return;
    
    // Store the mode we're leaving for cleanup
    PorkchopMode oldMode = currentMode;
    
    // Detect seamless OINK <-> DNH switch (preserve WiFi state)
    bool seamlessSwitch = 
        (oldMode == PorkchopMode::OINK_MODE && mode == PorkchopMode::DNH_MODE) ||
        (oldMode == PorkchopMode::DNH_MODE && mode == PorkchopMode::OINK_MODE);
    
    // Only save "real" modes as previous (not modal menus)
    if (currentMode != PorkchopMode::SETTINGS &&
        currentMode != PorkchopMode::ABOUT &&
        currentMode != PorkchopMode::CAPTURES &&
        currentMode != PorkchopMode::ACHIEVEMENTS &&
        currentMode != PorkchopMode::MENU &&
        currentMode != PorkchopMode::FILE_TRANSFER &&
        currentMode != PorkchopMode::CRASH_VIEWER &&
        currentMode != PorkchopMode::DIAGNOSTICS &&
        currentMode != PorkchopMode::SWINE_STATS &&
        currentMode != PorkchopMode::BOAR_BROS &&
        currentMode != PorkchopMode::WIGLE_MENU &&
        currentMode != PorkchopMode::BOUNTY_STATUS &&
        currentMode != PorkchopMode::PIGSYNC_DEVICE_SELECT &&
        currentMode != PorkchopMode::UNLOCKABLES) {
        previousMode = currentMode;
    }
    currentMode = mode;
    
    // Cleanup the mode we're actually leaving (oldMode), not previousMode
    switch (oldMode) {
        case PorkchopMode::OINK_MODE:
            if (seamlessSwitch) {
                OinkMode::stopSeamless();  // Preserve WiFi state for DNH
            } else {
                OinkMode::stop();
            }
            break;
        case PorkchopMode::DNH_MODE:
            if (seamlessSwitch) {
                DoNoHamMode::stopSeamless();  // Preserve WiFi state for OINK
            } else {
                DoNoHamMode::stop();
            }
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
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::hide();
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::hide();
            break;
        case PorkchopMode::SWINE_STATS:
            SwineStats::hide();
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::hide();
            break;
        case PorkchopMode::WIGLE_MENU:
            WigleMenu::hide();
            break;
        case PorkchopMode::UNLOCKABLES:
            UnlockablesMenu::hide();
            break;
        case PorkchopMode::BOUNTY_STATUS:
            BountyStatusMenu::hide();
            break;
        case PorkchopMode::PIGSYNC_DEVICE_SELECT:
            PigSyncMode::stopDiscovery();
            PigSyncMode::stop();
            break;
        case PorkchopMode::MARCO_MODE:
            MarcoMode::stop();
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
            SDLog::log("PORK", "Mode: IDLE");
            break;
        case PorkchopMode::OINK_MODE:
            Avatar::setState(AvatarState::HUNTING);
            SDLog::log("PORK", "Mode: OINK");
            if (seamlessSwitch) {
                OinkMode::startSeamless();  // Preserves WiFi state from DNH
            } else {
                OinkMode::start();
            }
            break;
        case PorkchopMode::DNH_MODE:
            Avatar::setState(AvatarState::NEUTRAL);  // Calm, passive state
            SDLog::log("PORK", "Mode: DO NO HAM");
            if (seamlessSwitch) {
                DoNoHamMode::startSeamless();  // Preserves WiFi state from OINK
            } else {
                DoNoHamMode::start();
            }
            break;
        case PorkchopMode::WARHOG_MODE:
            Avatar::setState(AvatarState::EXCITED);
            Display::showToast("SNIFFING THE AIR...");
            SDLog::log("PORK", "Mode: WARHOG");
            // Disable ML/Enhanced features for heap savings
            {
                auto mlCfg = Config::ml();
                mlCfg.enabled = false;
                mlCfg.collectionMode = MLCollectionMode::BASIC;
                Config::setML(mlCfg);
            }
            WarhogMode::start();
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            Avatar::setState(AvatarState::ANGRY);
            SDLog::log("PORK", "Mode: PIGGYBLUES");
            PiggyBluesMode::start();
            // If user aborted warning dialog, return to menu
            if (!PiggyBluesMode::isRunning()) {
                currentMode = PorkchopMode::MENU;
                Menu::show();
            }
            break;
        case PorkchopMode::SPECTRUM_MODE:
            Avatar::setState(AvatarState::HUNTING);
            SDLog::log("PORK", "Mode: SPECTRUM");
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
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::show();
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::show();
            break;
        case PorkchopMode::SWINE_STATS:
            SwineStats::show();
            break;
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::show();
            break;
        case PorkchopMode::WIGLE_MENU:
            WigleMenu::show();
            break;
        case PorkchopMode::UNLOCKABLES:
            UnlockablesMenu::show();
            break;
        case PorkchopMode::BOUNTY_STATUS:
            BountyStatusMenu::show();
            break;
        case PorkchopMode::PIGSYNC_DEVICE_SELECT:
            Avatar::setState(AvatarState::EXCITED);
            SDLog::log("PORK", "Mode: PIGSYNC Device Select");
            PigSyncMode::start();
            PigSyncMode::startDiscovery();
            break;
        case PorkchopMode::MARCO_MODE:
            Avatar::setState(AvatarState::HAPPY);
            SDLog::log("PORK", "Mode: MARCO");
            MarcoMode::init();
            MarcoMode::start();
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
    // G0 button (GPIO0 on top side) - configurable action
    static bool g0WasPressed = false;
    bool g0Pressed = (digitalRead(0) == LOW);  // G0 is active LOW

    if (g0Pressed && !g0WasPressed) {
        G0Action g0Action = Config::personality().g0Action;
        if (g0Action != G0Action::SCREEN_TOGGLE) {
            Display::resetDimTimer();  // Wake screen on G0
        }
        Serial.printf("[PORKCHOP] G0 pressed! Current mode: %d\n", (int)currentMode);
        switch (g0Action) {
            case G0Action::SCREEN_TOGGLE:
                Display::toggleScreenPower();
                break;
            case G0Action::OINK:
                setMode(PorkchopMode::OINK_MODE);
                break;
            case G0Action::DNHAM:
                setMode(PorkchopMode::DNH_MODE);
                break;
            case G0Action::SPECTRUM:
                setMode(PorkchopMode::SPECTRUM_MODE);
                break;
            case G0Action::PIGSYNC:
                setMode(PorkchopMode::PIGSYNC_DEVICE_SELECT);
                break;
            default:
                break;
        }
        g0WasPressed = true;
        return;
    }
    if (!g0Pressed) {
        g0WasPressed = false;
    }
    
    if (!M5Cardputer.Keyboard.isChange()) return;
    
    // Any keyboard input resets the screen dim timer
    Display::resetDimTimer();
    
    auto keys = M5Cardputer.Keyboard.keysState();
    // ESC maps to the key above Tab (shares ` / ~)
    bool escPressed = M5Cardputer.Keyboard.isKeyPressed('`');

    // ESC to return to IDLE from any active mode
    if (escPressed && currentMode != PorkchopMode::IDLE) {
        setMode(PorkchopMode::IDLE);
        return;
    }
    
    // In MENU mode, let Menu::handleInput() process navigation keys
    if (currentMode == PorkchopMode::MENU) {
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

    // In PIGSYNC_DEVICE_SELECT mode, handle navigation and channel switching
    if (currentMode == PorkchopMode::PIGSYNC_DEVICE_SELECT) {
        uint8_t deviceCount = PigSyncMode::getDeviceCount();

        // Handle device navigation (up/down) - only if devices exist
        if (deviceCount > 0) {
            if (M5Cardputer.Keyboard.isKeyPressed(';')) {
                // Up arrow - select previous device
                PigSyncMode::selectDevice(PigSyncMode::getSelectedIndex() > 0 ?
                    PigSyncMode::getSelectedIndex() - 1 : deviceCount - 1);
            }
            if (M5Cardputer.Keyboard.isKeyPressed('.')) {
                // Down arrow - select next device
                PigSyncMode::selectDevice((PigSyncMode::getSelectedIndex() + 1) % deviceCount);
            }
        }

        // Enter to connect to selected device
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER) && PigSyncMode::getDeviceCount() > 0) {
            uint8_t selectedIdx = PigSyncMode::getSelectedIndex();
            if (selectedIdx < PigSyncMode::getDeviceCount()) {
                PigSyncMode::connectTo(selectedIdx);
            }
        }

        // A to abort sync (when connected)
        if (PigSyncMode::isConnected() && M5Cardputer.Keyboard.isKeyPressed('a')) {
            if (PigSyncMode::isSyncing()) {
                PigSyncMode::abortSync();
            }
        }

        // D to disconnect (when connected)
        if (PigSyncMode::isConnected() && M5Cardputer.Keyboard.isKeyPressed('d')) {
            PigSyncMode::disconnect();
        }

        // R to rescan (when not connected)
        if (!PigSyncMode::isConnected() && M5Cardputer.Keyboard.isKeyPressed('r')) {
            PigSyncMode::startScan();
        }

        return; // Consume input for PIGSYNC_DEVICE_SELECT
    }
    
    // Backtick opens menu from IDLE (kept out of back/exit flow)
    if (currentMode == PorkchopMode::IDLE &&
        M5Cardputer.Keyboard.isKeyPressed('`')) {
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
    
    // T key stress test cycle disabled
    
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
                case 'd': // DO NO HAM mode
                case 'D':
                    setMode(PorkchopMode::DNH_MODE);
                    return; // Prevent fall-through to DNH handler
                case 'f': // File transfer (PORKCHOP COMMANDER)
                case 'F':
                    setMode(PorkchopMode::FILE_TRANSFER);
                    break;
                case '1': // PIG DEMANDS overlay
                    Display::showChallenges();
                    break;
                case '2': // PIGSYNC device select
                    setMode(PorkchopMode::PIGSYNC_DEVICE_SELECT);
                    break;
            }
        }
    }
    
    // OINK mode - B to exclude network
    if (currentMode == PorkchopMode::OINK_MODE) {
        // B key - add selected network to BOAR BROS exclusion list
        static bool bWasPressed = false;
        bool bPressed = M5Cardputer.Keyboard.isKeyPressed('b') || M5Cardputer.Keyboard.isKeyPressed('B');
        if (bPressed && !bWasPressed) {
            int idx = OinkMode::getSelectionIndex();
            if (OinkMode::excludeNetwork(idx)) {
                Display::showToast("BOAR BRO ADDED!");
                delay(500);
                OinkMode::moveSelectionDown();
            } else {
                Display::showToast("ALREADY A BRO");
                delay(500);
            }
        }
        bWasPressed = bPressed;
        
        // D key - switch to DO NO HAM mode (seamless mode switch)
        static bool dWasPressed_oink = false;
        bool dPressed = M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D');
        if (dPressed && !dWasPressed_oink) {
            // Track passive time for achievements
            SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
            sess.passiveTimeStart = millis();
            
            // Show toast before mode switch (loading screen)
            Display::showToast("IRIE VIBES ONLY NOW");
            delay(800);
            
            // Seamless switch to DNH mode
            setMode(PorkchopMode::DNH_MODE);
            return;  // Prevent fall-through to DNH block this frame
        }
        dWasPressed_oink = dPressed;
    }
    
    // DNH mode - D key to switch back to OINK
    if (currentMode == PorkchopMode::DNH_MODE) {
        // D key - switch back to OINK mode (seamless mode switch)
        static bool dWasPressed_dnh = false;
        bool dPressed = M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D');
        if (dPressed && !dWasPressed_dnh) {
            // Clear passive time tracking
            SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
            sess.passiveTimeStart = 0;
            
            // Show toast before mode switch (loading screen)
            Display::showToast("PROPER MAD ONE INNIT");
            delay(800);
            
            // Seamless switch to OINK mode
            setMode(PorkchopMode::OINK_MODE);
            return;  // Prevent any subsequent key handling this frame
        }
        dWasPressed_dnh = dPressed;
    }
    
    // WARHOG mode - use ESC to return to idle
    if (currentMode == PorkchopMode::WARHOG_MODE) {
        // no-op: ESC handled globally
    }
    
    // PIGGYBLUES mode - use ESC to return to idle
    if (currentMode == PorkchopMode::PIGGYBLUES_MODE) {
        // no-op: ESC handled globally
    }
    
    
    // SPECTRUM mode - ESC returns to idle globally
    // If monitoring a network, Spectrum handles its own keys
    if (currentMode == PorkchopMode::SPECTRUM_MODE) {
        // no-op: ESC handled globally
    }
    
    // FILE_TRANSFER mode - use ESC to return to idle
    if (currentMode == PorkchopMode::FILE_TRANSFER) {
        // no-op: ESC handled globally
    }
}

void Porkchop::updateMode() {
    switch (currentMode) {
        case PorkchopMode::OINK_MODE:
            OinkMode::update();
            break;
        case PorkchopMode::DNH_MODE:
            DoNoHamMode::update();
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
        case PorkchopMode::MARCO_MODE:
            MarcoMode::update();
            // Check if user exited
            if (!MarcoMode::isRunning()) {
                setMode(PorkchopMode::MENU);
            }
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
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::update();
            if (!CrashViewer::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::update();
            if (!DiagnosticsMenu::isActive()) {
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
        case PorkchopMode::WIGLE_MENU:
            WigleMenu::update();
            if (!WigleMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::UNLOCKABLES:
            UnlockablesMenu::update();
            if (!UnlockablesMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::BOUNTY_STATUS:
            BountyStatusMenu::update();
            if (!BountyStatusMenu::isActive()) {
                setMode(PorkchopMode::MENU);
            }
            break;
        case PorkchopMode::PIGSYNC_DEVICE_SELECT:
            // Update PigSync discovery process (includes dialogue phases)
            PigSyncMode::update();
            // Stay in device select mode for terminal display
            if (!PigSyncMode::isRunning()) {
                // User exited, go back to menu
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
