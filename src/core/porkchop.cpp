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
#include "../ui/sd_format_menu.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "heap_policy.h"
#include "../modes/donoham.h"
#include "../modes/warhog.h"
#include "../modes/piggyblues.h"
#include "../modes/spectrum.h"
#include "../modes/pigsync_client.h"
#include "../modes/bacon.h"
#include "../modes/charging.h"
#include "../web/fileserver.h"
#include "../audio/sfx.h"
#include "config.h"
#include "heap_health.h"
#include "xp.h"
#include "sdlog.h"
#include "sd_format.h"
#include "challenges.h"
#include "stress_test.h"
#include "network_recon.h"
#include "wifi_utils.h"
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <WiFi.h>

static const char* modeToString(PorkchopMode mode) {
    switch (mode) {
        case PorkchopMode::IDLE: return "IDLE";
        case PorkchopMode::OINK_MODE: return "OINK";
        case PorkchopMode::DNH_MODE: return "DNH";
        case PorkchopMode::WARHOG_MODE: return "WARHOG";
        case PorkchopMode::PIGGYBLUES_MODE: return "PIGGYBLUES";
        case PorkchopMode::SPECTRUM_MODE: return "SPECTRUM";
        case PorkchopMode::MENU: return "MENU";
        case PorkchopMode::SETTINGS: return "SETTINGS";
        case PorkchopMode::CAPTURES: return "CAPTURES";
        case PorkchopMode::ACHIEVEMENTS: return "ACHIEVEMENTS";
        case PorkchopMode::FILE_TRANSFER: return "FILE_TRANSFER";
        case PorkchopMode::CRASH_VIEWER: return "CRASH_VIEWER";
        case PorkchopMode::DIAGNOSTICS: return "DIAGNOSTICS";
        case PorkchopMode::SWINE_STATS: return "SWINE_STATS";
        case PorkchopMode::BOAR_BROS: return "BOAR_BROS";
        case PorkchopMode::WIGLE_MENU: return "WIGLE_MENU";
        case PorkchopMode::UNLOCKABLES: return "UNLOCKABLES";
        case PorkchopMode::BOUNTY_STATUS: return "BOUNTY_STATUS";
        case PorkchopMode::PIGSYNC_DEVICE_SELECT: return "PIGSYNC_DEVICE_SELECT";
        case PorkchopMode::BACON_MODE: return "BACON";
        case PorkchopMode::SD_FORMAT: return "SD_FORMAT";
        case PorkchopMode::CHARGING: return "CHARGING";
        case PorkchopMode::ABOUT: return "ABOUT";
        default: return "UNKNOWN";
    }
}

// Crash-loop guard: count early reboots using RTC memory (survives soft resets).
RTC_DATA_ATTR static uint8_t bootGuardStreak = 0;
static uint32_t bootGuardStartMs = 0;
static const uint8_t BOOT_GUARD_THRESHOLD = 3;
static const uint32_t BOOT_GUARD_WINDOW_MS = 60000;

static PorkchopMode bootModeToPorkchop(BootMode mode) {
    switch (mode) {
        case BootMode::OINK: return PorkchopMode::OINK_MODE;
        case BootMode::DNOHAM: return PorkchopMode::DNH_MODE;
        case BootMode::WARHOG: return PorkchopMode::WARHOG_MODE;
        case BootMode::IDLE:
        default:
            return PorkchopMode::IDLE;
    }
}

static const char* bootModeLabel(BootMode mode) {
    switch (mode) {
        case BootMode::OINK: return "OINK";
        case BootMode::DNOHAM: return "DN0HAM";
        case BootMode::WARHOG: return "WARHOG";
        case BootMode::IDLE:
        default:
            return "IDLE";
    }
}

static bool healthBootToastShown = false;

Porkchop::Porkchop() 
    : currentMode(PorkchopMode::IDLE)
    , previousMode(PorkchopMode::IDLE)
    , startTime(0)
    , handshakeCount(0)
    , networkCount(0)
    , deauthCount(0) {
}

static bool isAutoConditionSafe(PorkchopMode mode) {
    switch (mode) {
        case PorkchopMode::IDLE:
        case PorkchopMode::MENU:
        case PorkchopMode::SETTINGS:
        case PorkchopMode::ABOUT:
        case PorkchopMode::ACHIEVEMENTS:
        case PorkchopMode::CRASH_VIEWER:
        case PorkchopMode::DIAGNOSTICS:
        case PorkchopMode::SWINE_STATS:
        case PorkchopMode::BOAR_BROS:
        case PorkchopMode::UNLOCKABLES:
        case PorkchopMode::BOUNTY_STATUS:
        case PorkchopMode::SD_FORMAT:
            return true;
        default:
            return false;
    }
}

static void maybeAutoConditionHeap(PorkchopMode mode) {
    if (!isAutoConditionSafe(mode)) {
        return;
    }
    if (FileServer::isRunning() || FileServer::isConnecting()) {
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }
    if (!HeapHealth::consumeConditionRequest()) {
        return;
    }

    bool wasReconRunning = NetworkRecon::isRunning();
    if (wasReconRunning) {
        NetworkRecon::pause();
    }
    // Small, low-disruption brew to coalesce heap when health drops.
    WiFiUtils::brewHeap(HeapPolicy::kBrewAutoDwellMs, false);
    if (wasReconRunning) {
        NetworkRecon::resume();
    }
}

void Porkchop::init() {
    startTime = millis();
    
    // Initialize background network reconnaissance service
    NetworkRecon::init();
    
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
            case 18: setMode(PorkchopMode::BACON_MODE); break;
            case 19: setMode(PorkchopMode::DIAGNOSTICS); break;
            case 20: setMode(PorkchopMode::SD_FORMAT); break;
            case 21: setMode(PorkchopMode::CHARGING); break;
        }
    });

    bootGuardStartMs = millis();
    if (bootGuardStreak < 255) {
        bootGuardStreak++;
    }
    bool bootGuardActive = bootGuardStreak >= BOOT_GUARD_THRESHOLD;

    BootMode bootMode = Config::personality().bootMode;
    bootModeTarget = bootModeToPorkchop(bootMode);
    if (bootModeTarget != PorkchopMode::IDLE && !bootGuardActive) {
        bootModePending = true;
        bootModeStartMs = millis();
        char buf[32];
        snprintf(buf, sizeof(buf), "BOOT -> %s IN 5S", bootModeLabel(bootMode));
        Display::showToast(buf, 5000);
    } else if (bootModeTarget != PorkchopMode::IDLE && bootGuardActive) {
        Display::showToast("BOOT GUARD - IDLE", 4000);
    }
    
    Avatar::setState(AvatarState::HAPPY);
    
    // Initialize non-blocking audio system
    SFX::init();

    if (!healthBootToastShown) {
        healthBootToastShown = true;
        Display::showToast(
            "HEALTH BAR IS HEAP HEALTH.\n"
            "LARGEST CONTIG DRIVES TLS.\n"
            "FRAGMENTATION YOINKS IT.\n"
            "BREW FIXES. JAH BLESS DI RF.",
            5000
        );
    }
    
    Serial.println("[PORKCHOP] Initialized");
    SDLog::log("PORK", "Initialized - LV%d %s", XP::getLevel(), XP::getTitle());
}

void Porkchop::update() {
    // Update background network reconnaissance (channel hopping, cleanup)
    NetworkRecon::update();
    
    processEvents();
    yield(); // Allow other tasks to run between operations
    handleInput();
    yield(); // Allow other tasks to run between operations
    
    if (bootGuardStreak > 0 && (millis() - bootGuardStartMs >= BOOT_GUARD_WINDOW_MS)) {
        bootGuardStreak = 0;
    }
    if (bootModePending) {
        if (currentMode != PorkchopMode::IDLE) {
            bootModePending = false;
        } else if (millis() - bootModeStartMs >= 5000) {
            bootModePending = false;
            setMode(bootModeTarget);
        }
    }
    updateMode();

    maybeAutoConditionHeap(currentMode);
    
    // Tick non-blocking audio engine
    SFX::update();
    yield(); // Allow other tasks to run between operations
    
    // Process one queued achievement celebration (debounced)
    XP::processAchievementQueue();
    yield(); // Allow other tasks to run between operations
    
    // Stress test injection (if active)
    StressTest::update();
    yield(); // Allow other tasks to run between operations
    
    // Check for session time XP bonuses
    XP::updateSessionTime();
    yield(); // Allow other tasks to run between operations
}

void Porkchop::setMode(PorkchopMode mode) {
    if (mode == currentMode) return;
    
    // Store the mode we're leaving for cleanup
    PorkchopMode oldMode = currentMode;

    Serial.printf("[MODE] EXIT %s free=%u largest=%u\n",
        modeToString(oldMode),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Save "real" modes as previous (not modal menus)
    // Exception: CAPTURES and WIGLE_MENU ARE saved as previousMode so OINK recovery returns to them
    if (currentMode != PorkchopMode::SETTINGS &&
        currentMode != PorkchopMode::ABOUT &&
        currentMode != PorkchopMode::ACHIEVEMENTS &&
        currentMode != PorkchopMode::MENU &&
        currentMode != PorkchopMode::FILE_TRANSFER &&
        currentMode != PorkchopMode::CRASH_VIEWER &&
        currentMode != PorkchopMode::DIAGNOSTICS &&
        currentMode != PorkchopMode::SWINE_STATS &&
        currentMode != PorkchopMode::BOAR_BROS &&
        currentMode != PorkchopMode::BOUNTY_STATUS &&
        currentMode != PorkchopMode::PIGSYNC_DEVICE_SELECT &&
        currentMode != PorkchopMode::UNLOCKABLES &&
        currentMode != PorkchopMode::SD_FORMAT) {
        previousMode = currentMode;
    }
    // ALSO save CAPTURES and WIGLE_MENU as return points from OINK recovery
    if (currentMode == PorkchopMode::CAPTURES ||
        currentMode == PorkchopMode::WIGLE_MENU) {
        previousMode = currentMode;
    }
    currentMode = mode;

    Serial.printf("[MODE] ENTER %s free=%u largest=%u\n",
        modeToString(currentMode),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Cleanup the mode we're actually leaving (oldMode), not previousMode
    switch (oldMode) {
        case PorkchopMode::OINK_MODE:
            OinkMode::stop();
            break;
        case PorkchopMode::DNH_MODE:
            DoNoHamMode::stop();
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
            // Restart NetworkRecon after FILE_TRANSFER to resume background scanning
            NetworkRecon::start();
            break;
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::hide();
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::hide();
            break;
        case PorkchopMode::SD_FORMAT:
            SdFormatMenu::hide();
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
        case PorkchopMode::BACON_MODE:
            BaconMode::stop();
            break;
        case PorkchopMode::CHARGING:
            ChargingMode::stop();
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
            Display::notify(NoticeKind::STATUS, "PROPER MAD ONE INNIT", 5000, NoticeChannel::TOP_BAR);
            SDLog::log("PORK", "Mode: OINK");
            OinkMode::start();
            break;
        case PorkchopMode::DNH_MODE:
            Avatar::setState(AvatarState::NEUTRAL);  // Calm, passive state
            SDLog::log("PORK", "Mode: DO NO HAM");
            DoNoHamMode::start();
            break;
        case PorkchopMode::WARHOG_MODE:
            Avatar::setState(AvatarState::EXCITED);
            Display::notify(NoticeKind::STATUS, "SNIFFING THE AIR", 5000, NoticeChannel::TOP_BAR);
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
            // Stop NetworkRecon to prevent heap fragmentation during FILE_TRANSFER
            // (promiscuous callbacks interleaved with WebServer allocs cause fragmentation)
            NetworkRecon::stop();
            Avatar::setState(AvatarState::HAPPY);
            FileServer::start(Config::wifi().otaSSID, Config::wifi().otaPassword);
            break;
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::show();
            break;
        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::show();
            break;
        case PorkchopMode::SD_FORMAT:
            SdFormatMenu::show();
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
        case PorkchopMode::BACON_MODE:
            Avatar::setState(AvatarState::HAPPY);
            SDLog::log("PORK", "Mode: BACON");
            BaconMode::init();
            BaconMode::start();
            break;
        case PorkchopMode::ABOUT:
            Display::resetAboutState();
            break;
        case PorkchopMode::CHARGING:
            SDLog::log("PORK", "Mode: CHARGING");
            ChargingMode::init();
            ChargingMode::start();
            break;
        default:
            break;
    }
    
    postEvent(PorkchopEvent::MODE_CHANGE, nullptr);
}

void Porkchop::postEvent(PorkchopEvent event, void* data) {
    // Prevent event queue overflow that could cause heap fragmentation
    if (eventQueue.size() >= MAX_EVENT_QUEUE_SIZE) {
        // Drop oldest event to maintain queue size
        eventQueue.erase(eventQueue.begin());
    }
    eventQueue.push_back({event, data});
}

void Porkchop::registerCallback(PorkchopEvent event, EventCallback callback) {
    // Prevent duplicate callbacks for the same event to avoid multiple executions
    // Note: We can't reliably compare std::function objects, so we just ensure each event
    // type has only one callback by replacing any existing one
    for (auto& pair : callbacks) {
        if (pair.first == event) {
            pair.second = callback; // Replace existing callback
            return;
        }
    }
    // Add bounds checking to prevent unlimited growth
    if (callbacks.size() >= MAX_EVENT_QUEUE_SIZE) {
        // Remove the oldest callback if we're at capacity
        callbacks.erase(callbacks.begin());
    }
    callbacks.push_back({event, callback});
}

void Porkchop::processEvents() {
    // Process events with bounds checking and yield for WDT safety
    size_t processed = 0;
    const size_t MAX_EVENTS_PER_UPDATE = 16; // Limit events processed per update to prevent WDT
    
    for (const auto& item : eventQueue) {
        if (processed >= MAX_EVENTS_PER_UPDATE) {
            // If we hit the limit, remove processed events and keep remaining for next update
            eventQueue.erase(eventQueue.begin(), eventQueue.begin() + processed);
            return; // Exit early to avoid WDT timeout
        }
        
        for (const auto& cb : callbacks) {
            if (cb.first == item.event) {
                // Execute callback with potential for yielding to prevent WDT issues
                cb.second(item.event, item.data);
                
                // Yield occasionally during heavy event processing
                if (++processed % 4 == 0) {
                    yield(); // Allow other tasks to run
                }
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
            case G0Action::DNOHAM:
                setMode(PorkchopMode::DNH_MODE);
                break;
            case G0Action::SPECTRUM:
                setMode(PorkchopMode::SPECTRUM_MODE);
                break;
            case G0Action::PIGSYNC:
                setMode(PorkchopMode::PIGSYNC_DEVICE_SELECT);
                break;
            case G0Action::IDLE:
                setMode(PorkchopMode::IDLE);
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
        yield(); // Allow other tasks to run during menu updates
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
                case 'c': // Charging mode
                case 'C':
                    setMode(PorkchopMode::CHARGING);
                    break;
            }
        }
        yield(); // Allow other tasks to run after processing all keys
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
            Display::notify(NoticeKind::STATUS, "IRIE VIBES ONLY NOW", 0, NoticeChannel::TOP_BAR);
            delay(800);
            
            // Seamless switch to DNH mode
            setMode(PorkchopMode::DNH_MODE);
            return;  // Prevent fall-through to DNH block this frame
        }
        dWasPressed_oink = dPressed;
    }
    
    // DNH mode - O key to switch back to OINK
    if (currentMode == PorkchopMode::DNH_MODE) {
        // O key - switch back to OINK mode (seamless mode switch)
        static bool oWasPressed_dnh = false;
        bool oPressed = M5Cardputer.Keyboard.isKeyPressed('o') || M5Cardputer.Keyboard.isKeyPressed('O');
        if (oPressed && !oWasPressed_dnh) {
            // Clear passive time tracking
            SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
            sess.passiveTimeStart = 0;
            
            // Show toast before mode switch (loading screen)
            Display::notify(NoticeKind::STATUS, "PROPER MAD ONE INNIT", 0, NoticeChannel::TOP_BAR);
            delay(800);
            
            // Seamless switch to OINK mode
            setMode(PorkchopMode::OINK_MODE);
            return;  // Prevent any subsequent key handling this frame
        }
        oWasPressed_dnh = oPressed;
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
    
    yield(); // Allow other tasks to run after processing input
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
        case PorkchopMode::BACON_MODE:
            BaconMode::update();
            // Check if user exited
            if (!BaconMode::isRunning()) {
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
        case PorkchopMode::SD_FORMAT:
            SdFormatMenu::update();
            if (!SdFormatMenu::isActive()) {
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
        case PorkchopMode::CHARGING:
            ChargingMode::update();
            // Auto-exit when unplugged
            if (!ChargingMode::isRunning()) {
                setMode(PorkchopMode::IDLE);
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
