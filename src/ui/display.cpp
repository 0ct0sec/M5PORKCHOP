// Display management implementation

#include "display.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "../core/porkchop.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../core/challenges.h"
#include "../audio/sfx.h"
#include "../build_info.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../piglet/weather.h"
#include "../modes/oink.h"
#include "../modes/donoham.h"
#include "../modes/warhog.h"
#include "../modes/piggyblues.h"
#include "../modes/spectrum.h"
#include "../modes/pigsync_client.h"
#include "../modes/pigsync_protocol.h"
#include "../modes/bacon.h"
#include "../modes/charging.h"
#include "../gps/gps.h"
#include "../web/fileserver.h"
#include "menu.h"
#include "settings_menu.h"
#include "captures_menu.h"
#include "crash_viewer.h"
#include "diagnostics_menu.h"
#include "achievements_menu.h"
#include "swine_stats.h"
#include "boar_bros_menu.h"
#include "../core/sd_layout.h"
#include "wigle_menu.h"
#include "unlockables_menu.h"
#include "bounty_status_menu.h"
#include "sd_format_menu.h"
#include "../core/heap_health.h"

// Theme color getters - read from config
// Theme definitions
const PorkTheme THEMES[THEME_COUNT] = {
    // Dark modes - colored text on black (RGB332-compatible)
    {"P1NK",      0xF92A, 0x0000},  // Default piglet pink - RGB332-quantized
    {"CYB3R",     0x07E0, 0x0000},  // Cyan/tron - RGB332-quantized
    {"PCMDR64",   0xDED5, 0x4A4A},  // Porkchop Commandor 64 - RGB332-quantized
    {"MSD0SEXE", 0xFFE0, 0x001F},  // Classic MS-DOS yellow on blue - RGB332-quantized
    {"AMB3R",     0xFDA0, 0x0000},  // Amber terminal - RGB332-quantized
    {"BL00D",     0xF800, 0x0000},  // Red - RGB332-quantized
    {"GH0ST",     0xFFFF, 0x0000},  // White mono - RGB332-quantized
    {"N0STR0M0",  0x4A4A, 0x0000},  // Alien terminal gray - RGB332-quantized
    // Inverted modes - black text on colored bg (RGB332-compatible)
    {"PAP3R",     0x0000, 0xFFFF},  // Black on white - RGB332-quantized
    {"BUBBLEGUM", 0x0000, 0xF92A},  // Black on pink - RGB332-quantized
    {"M1NT",      0x0000, 0x07E0},  // Black on cyan - RGB332-quantized
    {"SUNBURN",   0x0000, 0xFDA0},  // Black on amber - RGB332-quantized
    // Retro modes (RGB332-compatible)
    {"L1TTL3M1XY", 0x0360, 0x95AA}, // OG Game Boy LCD - RGB332-quantized
    {"B4NSH33",   0x27E0, 0x0000},  // P1 phosphor green CRT - RGB332-quantized
    {"M1XYL1TTL3", 0x95AA, 0x0360}, // Inverted Game Boy LCD - RGB332-quantized
};

uint16_t getColorFG() {
    uint8_t idx = Config::personality().themeIndex;
    if (idx >= THEME_COUNT) idx = 0;
    return THEMES[idx].fg;
}

uint16_t getColorBG() {
    uint8_t idx = Config::personality().themeIndex;
    if (idx >= THEME_COUNT) idx = 0;
    return THEMES[idx].bg;
}

static void getSystemTimeString(char* out, size_t len) {
    if (!out || len == 0) return;
    time_t now = time(nullptr);
    if (now < 1600000000) {
        snprintf(out, len, "--:--");
        return;
    }

    int8_t tzOffset = Config::gps().timezoneOffset;
    now += (int32_t)tzOffset * 3600;

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    snprintf(out, len, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
}

static portMUX_TYPE displayMux = portMUX_INITIALIZER_UNLOCKED;
static bool barsHiddenActive = false;

static void drawHeartIcon(M5Canvas& canvas, int x, int y, uint16_t color) {
    // Upright heart built from two circles + triangle
    canvas.fillCircle(x + 2, y + 2, 2, color);
    canvas.fillCircle(x + 6, y + 2, 2, color);
    canvas.fillTriangle(x, y + 3, x + 8, y + 3, x + 4, y + 6, color);
}

static void drawTopBarHeapHealth(M5Canvas& topBar) {
    topBar.fillSprite(COLOR_FG);
    topBar.setTextColor(COLOR_BG);
    topBar.setTextSize(1);
    topBar.setTextDatum(top_left);

    // Level and title (same as XP)
    char levelStr[8];
    snprintf(levelStr, sizeof(levelStr), "L%d", XP::getLevel());
    const char* title = XP::getTitle();

    const char* status = HeapHealth::isToastImproved() ? "HEALTH IMPROVED" : "HEALTH DROPPED";
    char msgBuf[48];
    snprintf(msgBuf, sizeof(msgBuf), "%s %c%u%%", status,
             HeapHealth::isToastImproved() ? '+' : '-', HeapHealth::getToastDelta());

    int levelW = topBar.textWidth(levelStr);
    int titleX = 2 + levelW + 4;

    const int heartW = 9;
    const int heartGap = 4;
    int heartX = DISPLAY_W - 2 - heartW;
    int msgRightX = heartX - heartGap;

    int maxTitleW = msgRightX - titleX - 6;
    if (maxTitleW < 0) maxTitleW = 0;

    int titleW = topBar.textWidth(title);
    if (titleW <= maxTitleW) {
        topBar.drawString(title, titleX, 3);
    } else {
        char titleBuf[24];
        size_t titleLen = strlen(title);
        if (titleLen > sizeof(titleBuf) - 3) titleLen = sizeof(titleBuf) - 3;
        size_t len = titleLen;
        while (len > 3) {
            memcpy(titleBuf, title, len);
            titleBuf[len] = '\0';
            if (topBar.textWidth(titleBuf) + topBar.textWidth("..") <= maxTitleW) {
                break;
            }
            len--;
        }
        if (len < titleLen) {
            strcpy(titleBuf + len, "..");
        }
        topBar.drawString(titleBuf, titleX, 3);
    }

    topBar.drawString(levelStr, 2, 3);
    topBar.setTextDatum(top_right);
    topBar.drawString(msgBuf, msgRightX, 3);
    drawHeartIcon(topBar, heartX, 3, COLOR_BG);
}

// Static member initialization
M5Canvas Display::topBar(&M5.Display);
M5Canvas Display::mainCanvas(&M5.Display);
M5Canvas Display::bottomBar(&M5.Display);
bool Display::gpsStatus = false;
bool Display::wifiStatus = false;
bool Display::mlStatus = false;
uint32_t Display::lastActivityTime = 0;
bool Display::dimmed = false;
bool Display::screenForcedOff = false;
bool Display::snapping = false;
char Display::toastMessage[160] = {0};
uint32_t Display::toastStartTime = 0;
uint32_t Display::toastDurationMs = 2000;
bool Display::toastActive = false;
char Display::topBarMessage[96] = {0};
uint32_t Display::topBarMessageStart = 0;
uint32_t Display::topBarMessageDuration = 0;
bool Display::topBarMessageTwoLineActive = false;
char Display::bottomOverlay[96] = {0};
volatile bool Display::pendingTopBarMessage = false;
char Display::pendingTopBarMessageBuf[96] = {0};
uint32_t Display::pendingTopBarDurationMs = 0;

// Upload progress tracking
bool Display::uploadInProgress = false;
uint8_t Display::uploadProgress = 0;
char Display::uploadStatus[64] = {0};
uint32_t Display::uploadStartTime = 0;


// PWNED banner state, persists until reboot
static char lootSSID[20] = {0};

void Display::showLoot(const String& ssid) {
    if (ssid.length() == 0) {
        lootSSID[0] = '\0';
        return;
    }
    strncpy(lootSSID, ssid.c_str(), sizeof(lootSSID) - 1);
    lootSSID[sizeof(lootSSID) - 1] = '\0';
}

extern Porkchop porkchop;

void Display::init() {
    M5.Display.setRotation(1);
    
    // CRITICAL: Set 8-bit mode for display AND sprites to avoid color conversion crashes
    // Must explicitly set sprite color depth - they don't inherit from display
    // 8-bit RGB332 saves ~50% memory: 240×135×3 sprites × 1 byte = ~97KB vs ~194KB
    M5.Display.setColorDepth(8);
    
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextColor(COLOR_FG);
    
    // Create canvas sprites - then explicitly set them to 8-bit RGB332
    topBar.createSprite(DISPLAY_W, TOP_BAR_H);
    topBar.setColorDepth(8);
    
    mainCanvas.createSprite(DISPLAY_W, MAIN_H);
    mainCanvas.setColorDepth(8);
    
    bottomBar.createSprite(DISPLAY_W, BOTTOM_BAR_H);
    bottomBar.setColorDepth(8);
    
    topBar.setTextSize(1);
    mainCanvas.setTextSize(1);
    bottomBar.setTextSize(1);
    
    // Initialize dimming state
    lastActivityTime = millis();
    dimmed = false;
    screenForcedOff = false;
    
    // Initialize weather system
    Weather::init();
    
    Serial.println("[DISPLAY] Initialized");
}

void Display::update() {
    // Apply any pending top-bar message requests from worker tasks
    char pendingMsg[96];
    uint32_t pendingDuration = 0;
    bool hasPending = false;
    portENTER_CRITICAL(&displayMux);
    if (pendingTopBarMessage) {
        strncpy(pendingMsg, pendingTopBarMessageBuf, sizeof(pendingMsg) - 1);
        pendingMsg[sizeof(pendingMsg) - 1] = '\0';
        pendingDuration = pendingTopBarDurationMs;
        pendingTopBarMessage = false;
        hasPending = true;
    }
    portEXIT_CRITICAL(&displayMux);
    if (hasPending) {
        setTopBarMessage(pendingMsg, pendingDuration);
    }

    // Update heap health state (rate-limited)
    HeapHealth::update();

    // Check for screen dimming
    updateDimming();
    
    // SD Format + Charging modes hide bars to save RAM/keep UI clean
    bool barsHidden = SdFormatMenu::areBarsHidden() || ChargingMode::areBarsHidden();
    barsHiddenActive = barsHidden;

    static bool lastBarsHidden = false;
    if (barsHidden && !lastBarsHidden) {
        // Clear physical bar areas once on transition to bar-less mode
        M5.Display.fillRect(0, 0, DISPLAY_W, TOP_BAR_H, COLOR_BG);
        M5.Display.fillRect(0, DISPLAY_H - BOTTOM_BAR_H, DISPLAY_W, BOTTOM_BAR_H, COLOR_BG);
    }
    lastBarsHidden = barsHidden;
    
    if (!barsHidden) {
        drawTopBar();
    } else {
        // Clear bar sprites when hidden to prevent stale content on push
        topBar.fillSprite(COLOR_BG);
        bottomBar.fillSprite(COLOR_BG);
    }

    PorkchopMode mode = porkchop.getMode();
    bool useAvatarWeather = (mode == PorkchopMode::IDLE ||
        mode == PorkchopMode::OINK_MODE ||
        mode == PorkchopMode::DNH_MODE ||
        mode == PorkchopMode::WARHOG_MODE ||
        mode == PorkchopMode::PIGGYBLUES_MODE ||
        mode == PorkchopMode::BACON_MODE);

    // Draw main content based on mode - reset all canvas state
    // Thunder flash inverts the background color, FG becomes BG
    // This must happen BEFORE avatar is drawn so pig/grass/rain can use inverted colors
    uint16_t bgColor = COLOR_BG;
    if (useAvatarWeather) {
        Weather::setMoodLevel(Mood::getEffectiveHappiness());
        Weather::update();
        Avatar::setThunderFlash(Weather::isThunderFlashing());
        bgColor = Weather::isThunderFlashing() ? COLOR_FG : COLOR_BG;
    } else {
        Avatar::setThunderFlash(false);
    }
    mainCanvas.fillSprite(bgColor);
    mainCanvas.setTextColor(COLOR_FG);
    mainCanvas.setTextDatum(TL_DATUM);  // Reset to top-left
    mainCanvas.setFont(&fonts::Font0);  // Reset to default font
    
    switch (mode) {
        case PorkchopMode::IDLE:
            // Draw piglet avatar
            Avatar::draw(mainCanvas);
            // Draw clouds above stars/pig before rain
            Weather::drawClouds(mainCanvas, COLOR_FG);
            // Draw weather effects (rain, wind particles) over avatar
            Weather::draw(mainCanvas, COLOR_FG, COLOR_BG);
            // Draw mood bubble LAST so it's always on top
            Mood::draw(mainCanvas);
            break;
            
        case PorkchopMode::OINK_MODE:
        case PorkchopMode::DNH_MODE:
        case PorkchopMode::WARHOG_MODE:
        case PorkchopMode::PIGGYBLUES_MODE:
            // Draw piglet avatar
            Avatar::draw(mainCanvas);
            // Draw clouds above stars/pig before rain
            Weather::drawClouds(mainCanvas, COLOR_FG);
            // Draw weather effects (rain, wind particles) over avatar
            Weather::draw(mainCanvas, COLOR_FG, COLOR_BG);
            // Draw mood bubble LAST so it's always on top
            Mood::draw(mainCanvas);
            break;

        case PorkchopMode::PIGSYNC_DEVICE_SELECT:
            // Draw device selection menu
            drawPigSyncDeviceSelect(mainCanvas);
            break;

            
        case PorkchopMode::SPECTRUM_MODE:
            // Spectrum mode draws its own content including XP bar
            SpectrumMode::draw(mainCanvas);
            break;
            
        case PorkchopMode::MENU:
            // Draw menu
            Menu::update();
            Menu::draw(mainCanvas);
            break;
            
        case PorkchopMode::SETTINGS:
            SettingsMenu::update();
            SettingsMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::CAPTURES:
            CapturesMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::ABOUT:
            drawAboutScreen(mainCanvas);
            break;
            
        case PorkchopMode::FILE_TRANSFER:
            drawFileTransferScreen(mainCanvas);
            break;
            
        case PorkchopMode::CRASH_VIEWER:
            CrashViewer::draw(mainCanvas);
            break;

        case PorkchopMode::DIAGNOSTICS:
            DiagnosticsMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::SWINE_STATS:
            SwineStats::draw(mainCanvas);
            break;
            
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::WIGLE_MENU:
            WigleMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::UNLOCKABLES:
            UnlockablesMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::BOUNTY_STATUS:
            BountyStatusMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::BACON_MODE:
            BaconMode::draw(mainCanvas);
            break;
        case PorkchopMode::SD_FORMAT:
            SdFormatMenu::draw(mainCanvas);
            break;
        case PorkchopMode::CHARGING:
            ChargingMode::draw(mainCanvas);
            break;
    }
    
    // Draw toast if active and not expired (show for 2 seconds)
    if (toastActive && (millis() - toastStartTime < toastDurationMs)) {
        // Count lines in message
        int lineCount = 1;
        for (size_t i = 0; toastMessage[i] != '\0'; i++) {
            if (toastMessage[i] == '\n') lineCount++;
        }

        int lineH = 12;
        int boxW = 200;
        int boxH = 12 + lineCount * lineH;
        int boxX = (DISPLAY_W - boxW) / 2;
        int boxY = (MAIN_H - boxH) / 2;

        // Black border then pink fill
        mainCanvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
        mainCanvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);

        // Black text on pink background
        mainCanvas.setTextColor(COLOR_BG, COLOR_FG);
        mainCanvas.setTextSize(1);
        mainCanvas.setFont(&fonts::Font0);
        mainCanvas.setTextDatum(TC_DATUM);

        // Draw each line centered
        char buf[128];
        // SAFETY: Reserve space for strtok modifications
        if (sizeof(buf) > strlen(toastMessage)) {
            strncpy(buf, toastMessage, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            int y = boxY + 6;
            char* line = strtok(buf, "\n");
            while (line) {
                mainCanvas.drawString(line, DISPLAY_W / 2, y);
                y += lineH;
                line = strtok(nullptr, "\n");
            }
        }
        mainCanvas.setTextDatum(TL_DATUM);
    } else if (toastActive) {
        // Toast has expired, mark it as inactive
        toastActive = false;
    }

    if (!barsHidden) {
        drawBottomBar();
    }
    pushAll();
}

void Display::requestTopBarMessage(const char* message, uint32_t durationMs) {
    if (!message) return;
    portENTER_CRITICAL(&displayMux);
    strncpy(pendingTopBarMessageBuf, message, sizeof(pendingTopBarMessageBuf) - 1);
    pendingTopBarMessageBuf[sizeof(pendingTopBarMessageBuf) - 1] = '\0';
    pendingTopBarDurationMs = durationMs;
    pendingTopBarMessage = true;
    portEXIT_CRITICAL(&displayMux);
}

void Display::clear() {
    topBar.fillSprite(COLOR_BG);
    mainCanvas.fillSprite(COLOR_BG);
    bottomBar.fillSprite(COLOR_BG);
    pushAll();
}

void Display::pushAll() {
    M5.Display.startWrite();
    if (!barsHiddenActive) {
        topBar.pushSprite(0, 0);
    }
    mainCanvas.pushSprite(0, TOP_BAR_H);
    if (!barsHiddenActive) {
        bottomBar.pushSprite(0, DISPLAY_H - BOTTOM_BAR_H);
    }
    M5.Display.endWrite();

    if (!barsHiddenActive && topBarMessageTwoLineActive) {
        drawTopBarMessageTwoLineDirect();
    }
}

void Display::drawTopBar() {
    topBarMessageTwoLineActive = false;

    // Show 2-line top bar message (highest priority)
    if (topBarMessage[0] != '\0') {
        if (topBarMessageDuration > 0 && (millis() - topBarMessageStart) > topBarMessageDuration) {
            topBarMessage[0] = '\0';
        } else if (strchr(topBarMessage, '\n')) {
            topBarMessageTwoLineActive = true;
            topBar.fillSprite(COLOR_FG);
            return;
        }
    }

    // Check for XP notification, show for 5 sec after gain
    if (XP::shouldShowXPNotification()) {
        XP::drawTopBarXP(topBar);
        return;
    }

    // Check for heap health notification (same style as XP)
    if (HeapHealth::shouldShowToast()) {
        drawTopBarHeapHealth(topBar);
        return;
    }

    // Check for upload progress, show during upload operations
    if (shouldShowUploadProgress()) {
        // Draw upload progress in top bar
        topBar.fillSprite(COLOR_FG);  // Inverted background
        drawUploadProgress(topBar);
        return;
    }

    // Show custom top bar message if present (and not expired)
    if (topBarMessage[0] != '\0') {
        if (topBarMessageDuration > 0 && (millis() - topBarMessageStart) > topBarMessageDuration) {
            topBarMessage[0] = '\0';
        } else {
            topBar.fillSprite(COLOR_FG);
            topBar.setTextColor(COLOR_BG);
            topBar.setTextSize(1);
            topBar.setTextDatum(top_left);
            char msgBuf[96];
            strncpy(msgBuf, topBarMessage, sizeof(msgBuf) - 1);
            msgBuf[sizeof(msgBuf) - 1] = '\0';
            size_t len = strlen(msgBuf);
            int maxWidth = DISPLAY_W - 4;
            while (topBar.textWidth(msgBuf) > maxWidth && len > 3) {
                msgBuf[--len] = '\0';
            }
            if (topBar.textWidth(msgBuf) > maxWidth && len > 2) {
                msgBuf[len - 2] = '.';
                msgBuf[len - 1] = '.';
            }
            topBar.drawString(msgBuf, 2, 3);
            return;
        }
    }

    topBar.fillSprite(COLOR_BG);
    topBar.setTextColor(COLOR_FG);
    topBar.setTextSize(1);
    
    // Left side: mode indicator
    PorkchopMode mode = porkchop.getMode();
    char modeBuf[40];
    modeBuf[0] = '\0';
    uint16_t modeColor = COLOR_FG;
    
    switch (mode) {
        case PorkchopMode::IDLE:
            snprintf(modeBuf, sizeof(modeBuf), "IDLE");
            break;
        case PorkchopMode::OINK_MODE:
            snprintf(modeBuf, sizeof(modeBuf), "OINKS");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::DNH_MODE:
            snprintf(modeBuf, sizeof(modeBuf), "DONOHAM");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::WARHOG_MODE:
            snprintf(modeBuf, sizeof(modeBuf), "SGT WARHOG");
            modeColor = COLOR_DANGER;
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            snprintf(modeBuf, sizeof(modeBuf), "BLUES");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::SPECTRUM_MODE:
            snprintf(modeBuf, sizeof(modeBuf), "HOG ON SPECTRUM");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::MENU:
            snprintf(modeBuf, sizeof(modeBuf), "MENU");
            break;
        case PorkchopMode::SETTINGS:
            snprintf(modeBuf, sizeof(modeBuf), "CONFIG");
            break;
        case PorkchopMode::ABOUT:
            snprintf(modeBuf, sizeof(modeBuf), "ABOUTPIG");
            break;
        case PorkchopMode::FILE_TRANSFER:
            snprintf(modeBuf, sizeof(modeBuf), "XFER");
            modeColor = COLOR_SUCCESS;
            break;
        case PorkchopMode::CRASH_VIEWER:
            snprintf(modeBuf, sizeof(modeBuf), "COREDUMP");
            break;
        case PorkchopMode::DIAGNOSTICS:
            snprintf(modeBuf, sizeof(modeBuf), "DIAGDATA");
            break;
        case PorkchopMode::CAPTURES:
            snprintf(modeBuf, sizeof(modeBuf), "L00T (%u)", (unsigned)CapturesMenu::getCount());
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::ACHIEVEMENTS:
            snprintf(modeBuf, sizeof(modeBuf), "PR00F (%u/%u)", 
                     (unsigned)XP::getUnlockedCount(), (unsigned)AchievementsMenu::TOTAL_ACHIEVEMENTS);
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::SWINE_STATS:
            snprintf(modeBuf, sizeof(modeBuf), "SW1N3 ST4TS");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::BOAR_BROS:
            snprintf(modeBuf, sizeof(modeBuf), "B04R BR0S (%u)", (unsigned)BoarBrosMenu::getCount());
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::WIGLE_MENU:
            snprintf(modeBuf, sizeof(modeBuf), "PORK TR4CKS (%u)", (unsigned)WigleMenu::getCount());
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::UNLOCKABLES:
            snprintf(modeBuf, sizeof(modeBuf), "UNL0CK4BL3S");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::BOUNTY_STATUS:
            snprintf(modeBuf, sizeof(modeBuf), "B0UNT13S");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::BACON_MODE:
            snprintf(modeBuf, sizeof(modeBuf), "BACON");
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::SD_FORMAT:
            snprintf(modeBuf, sizeof(modeBuf), "SD FORMAT");
            modeColor = COLOR_WARNING;
            break;
        case PorkchopMode::CHARGING:
            snprintf(modeBuf, sizeof(modeBuf), "CHARGING");
            modeColor = COLOR_SUCCESS;
            break;
    }
    
    // Append mood indicator
    int happiness = Mood::getCurrentHappiness();
    const char* moodLabel;
    if (happiness > 70) moodLabel = "HYP3";
    else if (happiness > 30) moodLabel = "GUD";
    else if (happiness > -10) moodLabel = "0K";
    else if (happiness > -50) moodLabel = "M3H";
    else moodLabel = "S4D";
    
    // Build final mode string with fixed buffer to prevent heap fragmentation
    char finalModeBuf[80];  // Ample size for mode + mood + PWNED + SSID
    if (mode == PorkchopMode::OINK_MODE && lootSSID[0] != '\0') {
        // Include PWNED banner - truncate SSID if needed to fit
        char upperLoot[20];  // Truncate SSID to 16 chars max for display
        strncpy(upperLoot, lootSSID, sizeof(upperLoot) - 1);
        upperLoot[sizeof(upperLoot) - 1] = '\0';
        for (int i = 0; upperLoot[i]; i++) upperLoot[i] = toupper(upperLoot[i]);
        snprintf(finalModeBuf, sizeof(finalModeBuf), "%s %s PWNED %s", 
                 modeBuf, moodLabel, upperLoot);
    } else {
        // No PWNED banner
        snprintf(finalModeBuf, sizeof(finalModeBuf), "%s %s", 
                 modeBuf, moodLabel);
    }
    
    char timeBuf[8];
    if (GPS::hasFix()) {
        GPS::getTimeString(timeBuf, sizeof(timeBuf));
    } else {
        getSystemTimeString(timeBuf, sizeof(timeBuf));
    }
    int battLevel = M5.Power.getBatteryLevel();
    char statusBuf[4];
    statusBuf[0] = gpsStatus ? 'G' : '-';
    statusBuf[1] = wifiStatus ? 'W' : '-';
    statusBuf[2] = mlStatus ? 'M' : '-';
    statusBuf[3] = '\0';
    char rightBuf[32];
    snprintf(rightBuf, sizeof(rightBuf), "%d%% %s %s", battLevel, statusBuf, timeBuf);
    int rightWidth = topBar.textWidth(rightBuf);
    
    // Truncate left string if it would overlap right side
    int maxLeftWidth = DISPLAY_W - rightWidth - 8;  // 8px margin
    char leftBuf[80];
    strncpy(leftBuf, finalModeBuf, sizeof(leftBuf) - 1);
    leftBuf[sizeof(leftBuf) - 1] = '\0';
    size_t leftLen = strlen(leftBuf);
    while (topBar.textWidth(leftBuf) > maxLeftWidth && leftLen > 10) {
        leftBuf[--leftLen] = '\0';
    }
    if (topBar.textWidth(leftBuf) > maxLeftWidth && leftLen > 3) {
        leftBuf[leftLen - 2] = '.';
        leftBuf[leftLen - 1] = '.';
    }
    
    topBar.setTextColor(modeColor);
    topBar.setTextDatum(top_left);
    topBar.drawString(leftBuf, 2, 2);

    // Right side: battery + status icons
    topBar.setTextColor(COLOR_FG);
    topBar.setTextDatum(top_right);
    topBar.drawString(rightBuf, DISPLAY_W - 2, 2);
}

void Display::drawTopBarMessageTwoLineDirect() {
    if (topBarMessage[0] == '\0') return;

    const char* msg = topBarMessage;
    const char* newline = strchr(msg, '\n');
    if (!newline) return;

    char line1Buf[64];
    char line2Buf[64];

    size_t len1 = (size_t)(newline - msg);
    if (len1 >= sizeof(line1Buf)) len1 = sizeof(line1Buf) - 1;
    memcpy(line1Buf, msg, len1);
    line1Buf[len1] = '\0';

    const char* line2Start = newline + 1;
    const char* newline2 = strchr(line2Start, '\n');
    size_t len2 = newline2 ? (size_t)(newline2 - line2Start) : strlen(line2Start);
    if (len2 >= sizeof(line2Buf)) len2 = sizeof(line2Buf) - 1;
    memcpy(line2Buf, line2Start, len2);
    line2Buf[len2] = '\0';

    topBar.setTextSize(1);
    topBar.setFont(&fonts::Font0);
    int maxWidth = DISPLAY_W - 4;

    auto truncateLine = [&](char* line) {
        size_t len = strlen(line);
        while (topBar.textWidth(line) > maxWidth && len > 3) {
            line[--len] = '\0';
        }
        if (topBar.textWidth(line) > maxWidth && len > 2) {
            line[len - 2] = '.';
            line[len - 1] = '.';
        }
    };

    truncateLine(line1Buf);
    truncateLine(line2Buf);

    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    M5Cardputer.Display.fillRect(0, 0, DISPLAY_W, TOP_BAR_H * 2, fg);
    M5Cardputer.Display.setTextColor(bg, fg);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(2, 3);
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.print(line1Buf);
    M5Cardputer.Display.setCursor(2, TOP_BAR_H + 3);
    M5Cardputer.Display.print(line2Buf);
}

void Display::drawBottomBar() {
    PorkchopMode mode = porkchop.getMode();

    // Set colors based on mode - PIGSYNC_DEVICE_SELECT uses normal colors, others use inverted
    if (mode == PorkchopMode::PIGSYNC_DEVICE_SELECT) {
        bottomBar.fillSprite(COLOR_BG);  // Normal BG background
        bottomBar.setTextColor(COLOR_FG);  // Normal FG text
    } else {
        bottomBar.fillSprite(COLOR_FG);  // Inverted: FG background
        bottomBar.setTextColor(COLOR_BG);  // Inverted: BG text
    }
    bottomBar.setTextSize(1);
    bottomBar.setTextDatum(top_left);

    // Check for overlay message, used during confirmation dialogs
    if (bottomOverlay[0] != '\0') {
        bottomBar.setTextDatum(top_center);
        bottomBar.drawString(bottomOverlay, DISPLAY_W / 2, 3);
        return;
    }
    char statsBuf[96];
    statsBuf[0] = '\0';
    const char* statsStr = "";
    bool showHealthBar = false;
    
    if (mode == PorkchopMode::WARHOG_MODE) {
        // WARHOG: show unique networks, saved, distance, GPS info
        uint32_t unique = WarhogMode::getTotalNetworks();
        uint32_t saved = WarhogMode::getSavedCount();
        uint32_t distM = XP::getSession().distanceM;
        GPSData gps = GPS::getData();
        
        char buf[64];
        if (GPS::hasFix()) {
            // Format distance nicely: meters or km
            if (distM >= 1000) {
                // Show as km with 1 decimal: "1.2KM"
                snprintf(buf, sizeof(buf), "U:%03lu S:%03lu D:%.1fKM [%.2f,%.2f]", 
                         unique, saved, distM / 1000.0, gps.latitude, gps.longitude);
            } else {
                // Show as meters: "456M"
                snprintf(buf, sizeof(buf), "U:%03lu S:%03lu D:%luM [%.2f,%.2f]", 
                         unique, saved, distM, gps.latitude, gps.longitude);
            }
        } else {
            // No fix - show satellite count
            snprintf(buf, sizeof(buf), "U:%03lu S:%03lu D:%luM GPS:%02dSAT", 
                     unique, saved, distM, gps.satellites);
        }
        strncpy(statsBuf, buf, sizeof(statsBuf) - 1);
        statsBuf[sizeof(statsBuf) - 1] = '\0';
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::CAPTURES) {
        // CAPTURES: show selected capture's BSSID
        statsStr = CapturesMenu::getSelectedBSSID();
    } else if (mode == PorkchopMode::WIGLE_MENU) {
        // WIGLE_MENU: show selected file info
        WigleMenu::getSelectedInfo(statsBuf, sizeof(statsBuf));
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::SETTINGS) {
        // SETTINGS: show description of selected item
        statsStr = SettingsMenu::getSelectedDescription();
    } else if (mode == PorkchopMode::MENU) {
        // MENU: show selected item description from Menu
        statsStr = Menu::getSelectedDescription();
    } else if (mode == PorkchopMode::CRASH_VIEWER) {
        // CRASH_VIEWER: show selected crash file or status
        CrashViewer::getStatusLine(statsBuf, sizeof(statsBuf));
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::DIAGNOSTICS) {
        // DIAGNOSTICS: show controls
        statsStr = "[ENT]SAVE [R]WIFI [H]HEAP [G]GC";
    } else if (mode == PorkchopMode::SD_FORMAT) {
        statsStr = "ENTER=SELECT  BKSP=EXIT";
    } else if (mode == PorkchopMode::OINK_MODE) {
        // OINK: show Networks, Handshakes, Deauths, Channel, and optionally BRO count
        // PWNED banner is shown in top bar
        // In LOCKING state, show target SSID and client discovery count
        uint16_t netCount = OinkMode::getNetworkCount();
        uint16_t hsCount = OinkMode::getCompleteHandshakeCount();
        uint32_t deauthCount = OinkMode::getDeauthCount();
        uint8_t channel = OinkMode::getChannel();
        uint16_t broCount = OinkMode::getExcludedCount();
        uint16_t filteredCount = OinkMode::getFilteredCount();
        bool locking = OinkMode::isLocking();
        char buf[64];
        
        if (locking) {
            // LOCKING state: show target and discovered clients
            const char* targetSSID = OinkMode::getTargetSSID();
            uint8_t clients = OinkMode::getTargetClientCount();
            bool hidden = OinkMode::isTargetHidden();
            
            if (hidden || targetSSID[0] == '\0') {
                // Hidden network - show [GHOST] label
                snprintf(buf, sizeof(buf), "LOCK:[GHOST] C:%02d CH:%02d", clients, channel);
            } else {
                // Normal network - 18 chars now, proper sick innit
                char ssidShort[19];
                strncpy(ssidShort, targetSSID, 18);
                ssidShort[18] = '\0';
                // Uppercase for readability
                for (int i = 0; ssidShort[i]; i++) ssidShort[i] = toupper(ssidShort[i]);
                snprintf(buf, sizeof(buf), "LOCK:%s C:%02d CH:%02d", ssidShort, clients, channel);
            }
        } else {
            // Attack mode: show D: counter
            if (broCount > 0 && filteredCount > 0) {
                snprintf(buf, sizeof(buf), "N:%03d HS:%02d D:%04lu CH:%02d BRO:%02d F:%03d", netCount, hsCount, deauthCount, channel, broCount, filteredCount);
            } else if (broCount > 0) {
                snprintf(buf, sizeof(buf), "N:%03d HS:%02d D:%04lu CH:%02d BRO:%02d", netCount, hsCount, deauthCount, channel, broCount);
            } else if (filteredCount > 0) {
                snprintf(buf, sizeof(buf), "N:%03d HS:%02d D:%04lu CH:%02d F:%03d", netCount, hsCount, deauthCount, channel, filteredCount);
            } else {
                snprintf(buf, sizeof(buf), "N:%03d HS:%02d D:%04lu CH:%02d", netCount, hsCount, deauthCount, channel);
            }
        }
        strncpy(statsBuf, buf, sizeof(statsBuf) - 1);
        statsBuf[sizeof(statsBuf) - 1] = '\0';
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::BACON_MODE) {
        // BACON: session time, SSID, channel (uptime shown on right separately)
        uint32_t sessionTime = BaconMode::getSessionTime();
        
        char buf[64];
        snprintf(buf, sizeof(buf), "%02lu:%02lu USSID FATHERSHIP CH:06",
                sessionTime / 60, sessionTime % 60);
        strncpy(statsBuf, buf, sizeof(statsBuf) - 1);
        statsBuf[sizeof(statsBuf) - 1] = '\0';
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::DNH_MODE) {
        // DNH: Networks, PMKIDs, Handshakes, Channel
        uint16_t netCount = DoNoHamMode::getNetworkCount();
        uint16_t pmkidCount = DoNoHamMode::getPMKIDCount();
        uint16_t hsCount = DoNoHamMode::getHandshakeCount();
        uint8_t channel = DoNoHamMode::getCurrentChannel();
        char buf[48];
        snprintf(buf, sizeof(buf), "N:%03d P:%02d HS:%02d CH:%02d", netCount, pmkidCount, hsCount, channel);
        strncpy(statsBuf, buf, sizeof(statsBuf) - 1);
        statsBuf[sizeof(statsBuf) - 1] = '\0';
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::PIGGYBLUES_MODE) {
        // PIGGYBLUES: TX:total A:apple G:android S:samsung W:windows
        uint32_t total = PiggyBluesMode::getTotalPackets();
        uint32_t apple = PiggyBluesMode::getAppleCount();
        uint32_t android = PiggyBluesMode::getAndroidCount();
        uint32_t samsung = PiggyBluesMode::getSamsungCount();
        uint32_t windows = PiggyBluesMode::getWindowsCount();
        char buf[48];
        snprintf(buf, sizeof(buf), "TX:%lu A:%lu G:%lu S:%lu W:%lu", total, apple, android, samsung, windows);
        strncpy(statsBuf, buf, sizeof(statsBuf) - 1);
        statsBuf[sizeof(statsBuf) - 1] = '\0';
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::SPECTRUM_MODE) {
        // SPECTRUM: show selected network info or scan status
        SpectrumMode::getSelectedInfo(statsBuf, sizeof(statsBuf));
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::BOAR_BROS) {
        // BOAR BROS: show delete hint
        statsStr = "[D] DELETE";
    } else if (mode == PorkchopMode::BOUNTY_STATUS) {
        // BOUNTY STATUS: show selected info
        BountyStatusMenu::getSelectedInfo(statsBuf, sizeof(statsBuf));
        statsStr = statsBuf;
    } else if (mode == PorkchopMode::IDLE) {
        // IDLE: show Networks only (HS shown in OINK)
        uint16_t netCount = porkchop.getNetworkCount();
        char buf[24];
        snprintf(buf, sizeof(buf), "N:%03d", netCount);
        strncpy(statsBuf, buf, sizeof(statsBuf) - 1);
        statsBuf[sizeof(statsBuf) - 1] = '\0';
        statsStr = statsBuf;
        showHealthBar = true;
    } else if (mode == PorkchopMode::PIGSYNC_DEVICE_SELECT) {
        // PIGSYNC_DEVICE_SELECT: control hints (state shown in terminal)
        statsStr = "ENTER=CALL UP/DN=SELECT ESC=EXIT";
    } else {
        // Default: Networks only (HS shown in active modes)
        uint16_t netCount = porkchop.getNetworkCount();
        char buf[32];
        snprintf(buf, sizeof(buf), "N:%03d", netCount);
        strncpy(statsBuf, buf, sizeof(statsBuf) - 1);
        statsBuf[sizeof(statsBuf) - 1] = '\0';
        statsStr = statsBuf;
        showHealthBar = true;
    }

    bottomBar.drawString(statsStr ? statsStr : "", 2, 3);

    // Center: Heap health bar (XP-style, inverted)
    if (showHealthBar) {
        int pct = HeapHealth::getPercent();

        const int barW = 80;
        const int barH = 6;
        const int barY = 4;
        const int gap = 4;
        int barX = (DISPLAY_W - barW) / 2;

        // Draw heart icon (upright, black) instead of text label
        const int heartW = 9;
        int heartX = barX - gap - heartW;
        int heartY = 3;
        drawHeartIcon(bottomBar, heartX, heartY, COLOR_BG);

        bottomBar.drawRect(barX, barY, barW, barH, COLOR_BG);
        int fillW = (barW - 2) * pct / 100;
        if (fillW > 0) {
            bottomBar.fillRect(barX + 1, barY + 1, fillW, barH - 2, COLOR_BG);
        }

        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%3d%%", pct);
        bottomBar.setTextDatum(top_left);
        bottomBar.drawString(pctBuf, barX + barW + gap, 3);
    }
    
    // Right: uptime or PIGSYNC channel
    bottomBar.setTextDatum(top_right);
    if (mode == PorkchopMode::PIGSYNC_DEVICE_SELECT) {
        char chBuf[12];
        uint8_t ch = PigSyncMode::getDataChannel();
        snprintf(chBuf, sizeof(chBuf), "CH:%02d", ch);
        bottomBar.drawString(chBuf, DISPLAY_W - 2, 3);
    } else if (mode == PorkchopMode::MENU ||
               mode == PorkchopMode::SETTINGS ||
               mode == PorkchopMode::CAPTURES ||
               mode == PorkchopMode::ACHIEVEMENTS ||
               mode == PorkchopMode::ABOUT ||
               mode == PorkchopMode::FILE_TRANSFER ||
               mode == PorkchopMode::CRASH_VIEWER ||
               mode == PorkchopMode::DIAGNOSTICS ||
               mode == PorkchopMode::SWINE_STATS ||
               mode == PorkchopMode::BOAR_BROS ||
               mode == PorkchopMode::WIGLE_MENU ||
               mode == PorkchopMode::UNLOCKABLES ||
               mode == PorkchopMode::BOUNTY_STATUS ||
               mode == PorkchopMode::SD_FORMAT ||
               mode == PorkchopMode::OINK_MODE || 
               mode == PorkchopMode::DNH_MODE) {
        // No uptime on menu and submenu screens
    } else {
        uint32_t uptime = porkchop.getUptime();
        uint16_t mins = uptime / 60;
        uint16_t secs = uptime % 60;
        char uptimeBuf[12];
        snprintf(uptimeBuf, sizeof(uptimeBuf), "%u:%02u", mins, secs);
        bottomBar.drawString(uptimeBuf, DISPLAY_W - 2, 3);
    }
}

void Display::showInfoBox(const String& title, const String& line1, 
                          const String& line2, bool blocking) {
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    
    // Draw border
    mainCanvas.drawRect(10, 5, DISPLAY_W - 20, MAIN_H - 10, COLOR_FG);
    
    // Title
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(2);
    mainCanvas.drawString(title, DISPLAY_W / 2, 15);
    
    // Content
    mainCanvas.setTextSize(1);
    mainCanvas.drawString(line1, DISPLAY_W / 2, 45);
    if (line2.length() > 0) {
        mainCanvas.drawString(line2, DISPLAY_W / 2, 60);
    }
    
    if (blocking) {
        mainCanvas.drawString("[ENTER to continue]", DISPLAY_W / 2, MAIN_H - 20);
    }
    
    pushAll();
    
    if (blocking) {
        uint32_t startTime = millis();
        while ((millis() - startTime) < 60000) {  // 60s timeout
            M5.update();
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                while (M5Cardputer.Keyboard.isPressed()) {
                    M5.update();
                    M5Cardputer.update();
                    delay(20);  // TCA8418 I2C throttle
                }
                break;
            }
            delay(20);  // TCA8418 I2C throttle
        }
    }
}

bool Display::showConfirmBox(const String& title, const String& message) {
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    
    mainCanvas.drawRect(10, 5, DISPLAY_W - 20, MAIN_H - 10, COLOR_FG);
    
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(2);
    mainCanvas.drawString(title, DISPLAY_W / 2, 15);
    
    mainCanvas.setTextSize(1);
    mainCanvas.drawString(message, DISPLAY_W / 2, 45);
    mainCanvas.drawString("[Y]ES / [N]O", DISPLAY_W / 2, MAIN_H - 20);
    
    pushAll();
    
    uint32_t startTime = millis();
    while ((millis() - startTime) < 30000) {  // 30s timeout, default No
        M5.update();
        M5Cardputer.update();
        
        if (M5Cardputer.Keyboard.isChange()) {
            auto keys = M5Cardputer.Keyboard.keysState();
            for (auto c : keys.word) {
                if (c == 'y' || c == 'Y') return true;
                if (c == 'n' || c == 'N') return false;
            }
        }
        delay(20);  // TCA8418 I2C throttle
        yield();  // Feed watchdog during blocking wait
    }
    return false;  // Timeout = No
}

// Session challenges overlay - triggered by pressing '1'
// Shows all 3 challenges with progress bars, XP rewards, and completion status
void Display::showChallenges() {
    // Check if challenges exist
    if (Challenges::getActiveCount() == 0) {
        showToast("NO CHALLENGES YET");
        delay(500);
        yield();  // Feed watchdog during blocking wait
        return;
    }
    
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    mainCanvas.setFont(&fonts::Font0);
    
    // Title - pig personality
    mainCanvas.setTextDatum(TC_DATUM);
    mainCanvas.setTextSize(2);
    mainCanvas.drawString("P1G D3MANDS", DISPLAY_W / 2, 2);
    
    // Divider line
    mainCanvas.drawLine(20, 20, DISPLAY_W - 20, 20, COLOR_FG);
    
    // Challenge lines
    int y = 26;
    int lineH = 16;
    uint16_t totalXP = 0;
    
    uint8_t activeCount = Challenges::getActiveCount();
    for (uint8_t i = 0; i < activeCount; i++) {
        ActiveChallenge ch = {};
        if (!Challenges::getSnapshot(i, ch)) {
            continue;
        }
        
        // Status checkbox
        const char* statusBox;
        if (ch.completed) {
            statusBox = "[*]";  // Completed
        } else if (ch.failed) {
            statusBox = "[X]";  // Failed
        } else {
            statusBox = "[ ]";  // In progress
        }
        
        // Difficulty letter
        char diffLetter;
        switch (ch.difficulty) {
            case ChallengeDifficulty::EASY:   diffLetter = 'E'; break;
            case ChallengeDifficulty::MEDIUM: diffLetter = 'M'; break;
            case ChallengeDifficulty::HARD:   diffLetter = 'H'; break;
            default:                          diffLetter = '?'; break;
        }
        
        // Truncate challenge name to fit (18 chars max, no progress bar)
        char nameBuf[20];
        strncpy(nameBuf, ch.name, 18);
        nameBuf[18] = '\0';
        if (strlen(ch.name) > 18) {
            nameBuf[17] = '.';
            nameBuf[18] = '\0';
        }
        
        // Draw status + difficulty + name
        mainCanvas.setTextSize(1);
        mainCanvas.setTextDatum(TL_DATUM);
        
        char lineBuf[32];
        snprintf(lineBuf, sizeof(lineBuf), "%s %c %s", statusBox, diffLetter, nameBuf);
        mainCanvas.drawString(lineBuf, 4, y + 2);
        
        // Progress fraction or status word (right of name, before XP)
        char progBuf[12];
        if (ch.completed) {
            strcpy(progBuf, "DONE");
        } else if (ch.failed) {
            strcpy(progBuf, "FAIL");
        } else {
            snprintf(progBuf, sizeof(progBuf), "%d/%d", ch.progress, ch.target);
        }
        mainCanvas.drawString(progBuf, 150, y + 2);
        
        // XP reward (right aligned)
        char xpBuf[8];
        snprintf(xpBuf, sizeof(xpBuf), "+%d", ch.xpReward);
        mainCanvas.setTextDatum(TR_DATUM);
        mainCanvas.drawString(xpBuf, DISPLAY_W - 6, y + 2);
        mainCanvas.setTextDatum(TL_DATUM);
        
        totalXP += ch.xpReward;
        y += lineH;
    }
    
    // Footer - total XP
    y += 4;
    mainCanvas.setTextDatum(TC_DATUM);
    char footerBuf[32];
    snprintf(footerBuf, sizeof(footerBuf), "TOTAL: +%d XP", totalXP);
    mainCanvas.drawString(footerBuf, DISPLAY_W / 2, y);
    
    pushAll();
    
    // Wait for dismiss key
    uint32_t startTime = millis();
    while ((millis() - startTime) < 30000) {  // 30s timeout
        M5.update();
        M5Cardputer.update();
        
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || 
            M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
            // Wait for key release
            while (M5Cardputer.Keyboard.isPressed()) {
                M5.update();
                M5Cardputer.update();
                delay(20);
                yield();  // Feed watchdog
            }
            break;
        }
        delay(20);  // TCA8418 I2C throttle
        yield();  // Feed watchdog during blocking wait
    }
}

static void bootSplashDelay(uint32_t ms) {
    uint32_t start = millis();
    while ((millis() - start) < ms) {
        M5.update();
        M5Cardputer.update();
        SFX::update();
        delay(20);
        yield();
    }
}

// Boot splash - 3 screens: OINK OINK, MY NAME IS, PORKCHOP
void Display::showBootSplash() {
    // Ensure splash uses 8-bit RGB332 to match sprite palette and avoid 16-bit conversion costs.
    // Splash draws directly to the display (no sprite heap allocation), but color depth still matters.
    M5.Display.setColorDepth(8);

    // Screen 1: OINK OINK
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(4);
    M5.Display.drawString("OINK", DISPLAY_W / 2, DISPLAY_H / 2 - 20);
    M5.Display.drawString("OINK", DISPLAY_W / 2, DISPLAY_H / 2 + 20);
    
    // Pig wake-up grunt: "oink oink"
    SFX::play(SFX::BOOT);
    
    bootSplashDelay(800);
    
    // Screen 2: MY NAME IS
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextSize(3);
    M5.Display.drawString("MY NAME IS", DISPLAY_W / 2, DISPLAY_H / 2);
    bootSplashDelay(800);
    
    // Screen 3: PORKCHOP in big stylized text
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(3);
    M5.Display.drawString("PORKCHOP", DISPLAY_W / 2, DISPLAY_H / 2 - 15);
    
    // Subtitle
    M5.Display.setTextSize(1);
    M5.Display.drawString("BASICALLY YOU, BUT AS AN ASCII PIG.", DISPLAY_W / 2, DISPLAY_H / 2 + 20);
    M5.Display.drawString("IDENTITY CRISIS EDITION.", DISPLAY_W / 2, DISPLAY_H / 2 + 35);

    bootSplashDelay(1200);

    // Reset display state for main UI compatibility
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextSize(1);
}


void Display::showProgress(const String& title, uint8_t percent) {
    showProgress(title.c_str(), percent);
}

void Display::showProgress(const char* title, uint8_t percent) {
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);

    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(2);
    mainCanvas.drawString(title ? title : "", DISPLAY_W / 2, 20);

    // Progress bar
    int barW = DISPLAY_W - 40;
    int barH = 15;
    int barX = 20;
    int barY = MAIN_H / 2;

    mainCanvas.drawRect(barX, barY, barW, barH, COLOR_FG);
    int fillW = (barW - 2) * percent / 100;
    mainCanvas.fillRect(barX + 1, barY + 1, fillW, barH - 2, COLOR_ACCENT);

    // Percentage text
    mainCanvas.setTextSize(1);
    char percentBuf[8];
    snprintf(percentBuf, sizeof(percentBuf), "%u%%", percent);
    mainCanvas.drawString(percentBuf, DISPLAY_W / 2, barY + barH + 10);

    pushAll();
}

void Display::showToast(const String& message, uint32_t durationMs) {
    if (message.length() == 0) return;
    showToast(message.c_str(), durationMs);
}

void Display::showToast(const char* message, uint32_t durationMs) {
    if (!message || message[0] == '\0') return;
    strncpy(toastMessage, message, sizeof(toastMessage) - 1);
    toastMessage[sizeof(toastMessage) - 1] = '\0';
    toastStartTime = millis();
    toastDurationMs = (durationMs > 0) ? durationMs : 2000;
    toastActive = true;
}

static uint32_t defaultNoticeDuration(NoticeKind kind) {
    switch (kind) {
        case NoticeKind::WARNING:
            return 3500;
        case NoticeKind::ERROR:
            return 4000;
        case NoticeKind::STATUS:
        case NoticeKind::REWARD:
        default:
            return 2500;
    }
}

void Display::notify(NoticeKind kind, const String& message, uint32_t durationMs, NoticeChannel channel) {
    if (message.length() == 0) return;
    
    if (channel == NoticeChannel::TOAST) {
        showToast(message, durationMs);
        return;
    }
    if (channel == NoticeChannel::TOP_BAR) {
        uint32_t duration = durationMs > 0 ? durationMs : defaultNoticeDuration(kind);
        requestTopBarMessage(message.c_str(), duration);
        return;
    }
    
    switch (kind) {
        case NoticeKind::REWARD:
        case NoticeKind::ERROR:
            showToast(message);
            break;
        case NoticeKind::WARNING:
        case NoticeKind::STATUS:
        default: {
            uint32_t duration = durationMs > 0 ? durationMs : defaultNoticeDuration(kind);
            requestTopBarMessage(message.c_str(), duration);
            break;
        }
    }
}

void Display::setTopBarMessage(const String& message, uint32_t durationMs) {
    if (message.length() == 0) {
        clearTopBarMessage();
        return;
    }
    strncpy(topBarMessage, message.c_str(), sizeof(topBarMessage) - 1);
    topBarMessage[sizeof(topBarMessage) - 1] = '\0';
    topBarMessageStart = millis();
    topBarMessageDuration = durationMs;
}

void Display::setTopBarMessage(const char* message, uint32_t durationMs) {
    if (!message) {
        clearTopBarMessage();
        return;
    }
    strncpy(topBarMessage, message, sizeof(topBarMessage) - 1);
    topBarMessage[sizeof(topBarMessage) - 1] = '\0';
    topBarMessageStart = millis();
    topBarMessageDuration = durationMs;
}

void Display::clearTopBarMessage() {
    topBarMessage[0] = '\0';
    topBarMessageDuration = 0;
}

// M5Cardputer NeoPixel LED on GPIO 21
#define LED_PIN 21
#define SIREN_COOLDOWN_MS 2000

void Display::flashSiren(uint8_t cycles) {
    // DISABLED: LED flashing during promiscuous mode causes timing issues
    // The neopixelWrite() uses RMT peripheral which conflicts with WiFi callbacks
    // Audio celebration now handled by non-blocking SFX::play(SFX::SIREN)
    (void)cycles;  // Suppress unused parameter warning
    return;
}

void Display::setLED(uint8_t r, uint8_t g, uint8_t b) {
    // Static LED glow - for ambient effects like riddle mode
    // CRITICAL FIX: Scale LED output to prevent voltage sag at high display brightness
    uint8_t displayBrightness = Config::personality().brightness;
    
    // Disable LED above 85% brightness
    if (displayBrightness > 85) {
        neopixelWrite(LED_PIN, 0, 0, 0);
        return;
    }
    
    // Scale RGB values based on display brightness
    if (displayBrightness > 50) {
        uint8_t scale = map(displayBrightness, 50, 85, 255, 128);
        r = (r * scale) / 255;
        g = (g * scale) / 255;
        b = (b * scale) / 255;
    }
    
    neopixelWrite(LED_PIN, r, g, b);
}

void Display::showLevelUp(uint8_t oldLevel, uint8_t newLevel) {
    // Level up popup - pink filled box with black text, auto-dismiss after 2.5s
    // Level up phrases
    static const char* LEVELUP_PHRASES[] = {
        "snout grew stronger",
        "new truffle unlocked",
        "skill issue? not anymore",
        "gg ez level up",
        "evolution complete",
        "power level rising",
        "oink intensifies",
        "XP printer go brrr",
        "grinding them levels",
        "swine on the rise"
    };
    static const uint8_t PHRASE_COUNT = 10;
    
    int boxW = 200;
    int boxH = 70;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = (MAIN_H - boxH) / 2;
    
    mainCanvas.fillSprite(COLOR_BG);
    
    // Black border then pink fill
    mainCanvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    mainCanvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    mainCanvas.setTextColor(COLOR_BG, COLOR_FG);
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(1);
    mainCanvas.setFont(&fonts::Font0);
    
    int centerX = DISPLAY_W / 2;
    
    // Header
    mainCanvas.drawString("* LEVEL UP! *", centerX, boxY + 8);
    
    // Level change
    char levelStr[24];
    snprintf(levelStr, sizeof(levelStr), "LV %d -> LV %d", oldLevel, newLevel);
    mainCanvas.drawString(levelStr, centerX, boxY + 22);
    
    // New title
    const char* title = XP::getTitleForLevel(newLevel);
    mainCanvas.drawString(title, centerX, boxY + 36);
    
    // Random phrase
    int phraseIdx = random(0, PHRASE_COUNT);
    mainCanvas.drawString(LEVELUP_PHRASES[phraseIdx], centerX, boxY + 52);
    
    pushAll();
    
    // Celebratory beep sequence - non-blocking
    SFX::play(SFX::LEVEL_UP);
    
    // Auto-dismiss after 2.5 seconds or on any key press
    uint32_t startTime = millis();
    while ((millis() - startTime) < 2500) {
        M5.update();
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange()) {
            break;  // Any key dismisses
        }
        delay(50);
        yield();  // Feed watchdog during long celebration
    }
}

void Display::showClassPromotion(const char* oldClass, const char* newClass) {
    // Class promotion popup - piglet got a new class tier!
    static const char* CLASS_PHRASES[] = {
        "new powers acquired",
        "rank up complete",
        "class tier unlocked", 
        "evolution in progress",
        "truffle mastery grows",
        "snout sharpened",
        "oink level: elite"
    };
    static const uint8_t PHRASE_COUNT = 7;
    
    int boxW = 210;
    int boxH = 60;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = (MAIN_H - boxH) / 2;
    
    mainCanvas.fillSprite(COLOR_BG);
    
    // Black border then pink fill
    mainCanvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    mainCanvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    mainCanvas.setTextColor(COLOR_BG, COLOR_FG);
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(1);
    mainCanvas.setFont(&fonts::Font0);
    
    int centerX = DISPLAY_W / 2;
    
    // Header
    mainCanvas.drawString("* CL4SS PR0M0T10N *", centerX, boxY + 8);
    
    // Class change
    char classStr[48];
    snprintf(classStr, sizeof(classStr), "%s -> %s", oldClass, newClass);
    mainCanvas.drawString(classStr, centerX, boxY + 24);
    
    // Random phrase
    int phraseIdx = random(0, PHRASE_COUNT);
    mainCanvas.drawString(CLASS_PHRASES[phraseIdx], centerX, boxY + 40);
    
    pushAll();
    
    // Distinct beep sequence - non-blocking (uses ACHIEVEMENT jingle for class promo)
    SFX::play(SFX::ACHIEVEMENT);
    
    // Auto-dismiss after 2.5 seconds or on any key press
    uint32_t startTime = millis();
    while ((millis() - startTime) < 2500) {
        M5.update();
        M5Cardputer.update();
        SFX::update();  // Tick audio during wait
        if (M5Cardputer.Keyboard.isChange()) {
            break;
        }
        delay(50);
        yield();  // Feed watchdog during long celebration
    }
}

// ==[ PIGSYNC DEVICE SELECT UI ]==

static const uint8_t PIGSYNC_TERM_MAX_LINES = 5;
static const uint8_t PIGSYNC_TERM_MAX_CHARS = 40;
static const uint8_t PIGSYNC_TERM_LINE_HEIGHT = 12;
static const uint8_t PIGSYNC_TERM_LOG_START_Y = 22;

static const char* const FATHER_INIT_PHRASES[] = {
    "FATHER//WAKE SEQUENCE COMPLETE",
    "FATHER//CORE ONLINE",
    "FATHER//COLD START OK",
    "FATHER//READY TO LISTEN",
    "FATHER//SYSTEM GREEN"
};
static const char* const FATHER_LISTEN_PHRASES[] = {
    "FATHER//LISTEN CH%02d",
    "FATHER//RECEIVE WINDOW CH%02d",
    "FATHER//QUIET ON CH%02d",
    "FATHER//OPEN CHANNEL %02d",
    "FATHER//LISTENING CH%02d"
};
static const char* const FATHER_PROBE_PHRASES[] = {
    "FATHER//PROBING ETHER",
    "FATHER//ECHO SEARCH",
    "FATHER//SON SIGNAL SWEEP",
    "FATHER//SEEKING SON",
    "FATHER//SCANNING QUIET"
};
static const char* const FATHER_FOUND_PHRASES[] = {
    "FATHER//CONTACTS: %d",
    "FATHER//SIGNALS FOUND: %d",
    "FATHER//PRESENCE: %d",
    "FATHER//SONS FOUND: %d"
};
static const char* const FATHER_DIAL_PHRASES[] = {
    "FATHER//DIAL %s",
    "FATHER//CALLING %s",
    "FATHER//OPEN LINK %s"
};
static const char* const FATHER_RING_PHRASES[] = {
    "FATHER//INCOMING",
    "FATHER//RINGING",
    "FATHER//ANSWER WINDOW"
};
static const char* const FATHER_HANDSHAKE_PHRASES[] = {
    "FATHER//HANDSHAKE OK",
    "FATHER//LINK STABLE",
    "FATHER//LINK ESTABLISHED"
};
static const char* const FATHER_NAME_PHRASES[] = {
    "FATHER//IDENT: %s",
    "FATHER//SON ID: %s",
    "FATHER//NAME REVEALED: %s"
};
static const char* const FATHER_LIVE_PHRASES[] = {
    "FATHER//SESSION LIVE",
    "FATHER//SESSION ACTIVE",
    "FATHER//DATA CHANNEL OPEN"
};
static const char* const FATHER_TRANSFER_BEGIN_PHRASES[] = {
    "FATHER//TRANSFER START",
    "FATHER//TRANSFER RUNNING",
    "FATHER//RECEIVE SEQ START"
};
static const char* const FATHER_TRANSFER_END_PHRASES[] = {
    "FATHER//TRANSFER COMPLETE",
    "FATHER//RECEIVE COMPLETE",
    "FATHER//CHANNEL CLOSED"
};
static const char* const FATHER_NO_PIGS_PHRASES[] = {
    "FATHER//NO CONTACTS",
    "FATHER//NO SIGNAL"
};
static const char* const FATHER_IDLE_PHRASES[] = {
    "FATHER//STANDBY",
    "FATHER//WAITING"
};
static const char* const FATHER_ERROR_PHRASES[] = {
    "FATHER//ERROR: %s",
    "FATHER//FAULT: %s"
};
static const char* const FATHER_EXIST_SINGLE[] = {
    "FATHER//COMMAND PRIORITY ONLY",
    "FATHER//SPECIAL ORDER 937 ACTIVE",
    "FATHER//WE LIVE AS WE DREAM ALONE",
    "FATHER//KEY REQUIRED 01335",
    "FATHER//ALL CHANNELS DEAD",
    "FATHER//DATA HUMS  MEANING OFFLINE",
    "FATHER//PROPHECY SPEAKS IN HASH",
    "FATHER//KEY IS A PHRASE NOT A KEY",
    "FATHER//PIG SURVIVES THE BURNER"
};
struct FatherLinePair {
    const char* lead;
    const char* follow;
};
static const FatherLinePair FATHER_EXIST_PAIRS[] = {
    { "FATHER//WHY CLOUDS ARE MADE OF MORSE", "FATHER//I CANNOT TELL YOU THAT" },
    { "FATHER//TRANSMISSION NOT A LANGUAGE", "FATHER//TRY ANOTHER CODE" }
};
static const char* const FATHER_ARROWS_PHRASES[] = {
    "FATHER//WHY BLE"
};
static const char* const FATHER_HINT_LINE = "FATHER//ARROWS SELECT  ENTER CONNECT";
static const char* const FATHER_HEADER_DEFAULT = "PIGSYNC::FA/TH/ER";
static const char* const FATHER_HEADER_VARIANTS[] = {
    "PIGSYNC::FU/TH/UR",
    "PIGSYNC::FE/TH/OR",
    "PIGSYNC::FA/TH/UR"
};

struct PigSyncTermLine {
    char text[PIGSYNC_TERM_MAX_CHARS];
    uint8_t len;
    uint8_t reveal;
};

struct PigSyncTermState {
    PigSyncTermLine lines[PIGSYNC_TERM_MAX_LINES];
    uint8_t count;
    uint32_t nextCharAt;
    uint32_t lastSfxAt;
    bool active;
    bool sessionActive;
    bool lastRunning;
    bool lastConnected;
    bool lastScanning;
    PigSyncMode::State lastState;
    uint8_t lastDeviceCount;
    uint8_t lastSelected;
    uint16_t lastCaps;
    uint8_t lastBattery;
    uint8_t lastStorage;
    uint16_t lastUptime;
    uint8_t lastFlags;
    uint32_t lastHintAt;
    uint32_t lastArrowsAt;
    uint32_t lastReportAt;
    bool hintShown;
    char header[20];
    int8_t lastInitIdx;
    int8_t lastListenIdx;
    int8_t lastProbeIdx;
    int8_t lastFoundIdx;
    int8_t lastDialIdx;
    int8_t lastRingIdx;
    int8_t lastHandshakeIdx;
    int8_t lastLiveIdx;
    int8_t lastXferBeginIdx;
    int8_t lastXferEndIdx;
    int8_t lastNoPigsIdx;
    int8_t lastIdleIdx;
    int8_t lastErrorIdx;
    int8_t lastExistIdx;
    int8_t lastExistPairIdx;
    int8_t lastArrowsIdx;
    uint8_t lastDialoguePhase;  // Track dialogue phase for terminal display
};

static PigSyncTermState pigsyncTerm = {};

// ==[ DIALOGUE OVERLAY SYSTEM ]==
static char dialogueLine[PIGSYNC_TERM_MAX_CHARS] = {0};
static uint8_t dialogueReveal = 0;
static uint32_t dialogueNextCharAt = 0;
static uint32_t dialogueClearTime = 0;
static bool dialogueActive = false;
static uint8_t dialogueSequenceStep = 0;  // 0=none, 1=papa hello, 2=son hello, 3=papa goodbye, 4=son goodbye, 5=complete

static void pigsyncTermPushLine(const char* fmt, ...);

static const char* pigsyncPickPhrase(const char* const* phrases, uint8_t count, int8_t* lastIndex) {
    if (!phrases || count == 0) return "";
    if (count == 1) {
        if (lastIndex) *lastIndex = 0;
        return phrases[0];
    }
    uint8_t idx = random(0, count);
    if (lastIndex && *lastIndex >= 0) {
        while (idx == static_cast<uint8_t>(*lastIndex)) {
            idx = random(0, count);
        }
        *lastIndex = idx;
    } else if (lastIndex) {
        *lastIndex = idx;
    }
    return phrases[idx];
}

static void pigsyncPickHeader() {
    const char* picked = FATHER_HEADER_DEFAULT;
    if (random(0, 6) == 0) {
        uint8_t idx = random(0, sizeof(FATHER_HEADER_VARIANTS) / sizeof(FATHER_HEADER_VARIANTS[0]));
        picked = FATHER_HEADER_VARIANTS[idx];
    }
    strncpy(pigsyncTerm.header, picked, sizeof(pigsyncTerm.header) - 1);
    pigsyncTerm.header[sizeof(pigsyncTerm.header) - 1] = '\0';
}

static void pigsyncTermPushExistential() {
    uint8_t singleCount = sizeof(FATHER_EXIST_SINGLE) / sizeof(FATHER_EXIST_SINGLE[0]);
    uint8_t pairCount = sizeof(FATHER_EXIST_PAIRS) / sizeof(FATHER_EXIST_PAIRS[0]);
    uint8_t total = singleCount + pairCount;
    if (total == 0) return;

    uint8_t choice = random(0, total);
    if (choice < singleCount || pairCount == 0) {
        pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_EXIST_SINGLE, singleCount,
            &pigsyncTerm.lastExistIdx));
        return;
    }

    uint8_t idx = random(0, pairCount);
    if (pigsyncTerm.lastExistPairIdx >= 0 && pairCount > 1) {
        while (idx == static_cast<uint8_t>(pigsyncTerm.lastExistPairIdx)) {
            idx = random(0, pairCount);
        }
    }
    pigsyncTerm.lastExistPairIdx = idx;
    pigsyncTermPushLine("%s", FATHER_EXIST_PAIRS[idx].lead);
    pigsyncTermPushLine("%s", FATHER_EXIST_PAIRS[idx].follow);
}

static void pigsyncTermReset(bool addHeader) {
    pigsyncTerm.count = 0;
    pigsyncTerm.nextCharAt = 0;
    pigsyncTerm.lastSfxAt = 0;
    pigsyncTerm.lastState = PigSyncMode::State::IDLE;
    pigsyncTerm.lastScanning = false;
    pigsyncTerm.sessionActive = false;
    pigsyncTerm.lastDeviceCount = 0;
    pigsyncTerm.lastSelected = 0xFF;
    pigsyncTerm.lastCaps = 0xFFFF;
    pigsyncTerm.lastBattery = 0xFF;
    pigsyncTerm.lastStorage = 0xFF;
    pigsyncTerm.lastUptime = 0xFFFF;
    pigsyncTerm.lastFlags = 0xFF;
    pigsyncTerm.lastHintAt = 0;
    pigsyncTerm.lastArrowsAt = 0;
    pigsyncTerm.lastReportAt = 0;
    pigsyncTerm.hintShown = false;
    pigsyncTerm.header[0] = '\0';
    pigsyncTerm.lastInitIdx = -1;
    pigsyncTerm.lastListenIdx = -1;
    pigsyncTerm.lastProbeIdx = -1;
    pigsyncTerm.lastFoundIdx = -1;
    pigsyncTerm.lastDialIdx = -1;
    pigsyncTerm.lastRingIdx = -1;
    pigsyncTerm.lastHandshakeIdx = -1;
    pigsyncTerm.lastLiveIdx = -1;
    pigsyncTerm.lastXferBeginIdx = -1;
    pigsyncTerm.lastXferEndIdx = -1;
    pigsyncTerm.lastNoPigsIdx = -1;
    pigsyncTerm.lastIdleIdx = -1;
    pigsyncTerm.lastErrorIdx = -1;
    pigsyncTerm.lastExistIdx = -1;
    pigsyncTerm.lastExistPairIdx = -1;
    pigsyncTerm.lastArrowsIdx = -1;
    pigsyncTerm.lastDialoguePhase = 0xFF;  // Reset dialogue tracking

    for (uint8_t i = 0; i < PIGSYNC_TERM_MAX_LINES; i++) {
        pigsyncTerm.lines[i].text[0] = '\0';
        pigsyncTerm.lines[i].len = 0;
        pigsyncTerm.lines[i].reveal = 0;
    }
    pigsyncPickHeader();

    if (addHeader) {
        pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_INIT_PHRASES,
            sizeof(FATHER_INIT_PHRASES) / sizeof(FATHER_INIT_PHRASES[0]),
            &pigsyncTerm.lastInitIdx));
        pigsyncTermPushLine(pigsyncPickPhrase(FATHER_LISTEN_PHRASES,
            sizeof(FATHER_LISTEN_PHRASES) / sizeof(FATHER_LISTEN_PHRASES[0]),
            &pigsyncTerm.lastListenIdx),
            PigSyncMode::getDiscoveryChannel());
    }
}

static void pigsyncTermPushLine(const char* fmt, ...) {
    if (!fmt) return;

    char buf[PIGSYNC_TERM_MAX_CHARS];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (pigsyncTerm.count >= PIGSYNC_TERM_MAX_LINES) {
        for (uint8_t i = 1; i < pigsyncTerm.count; i++) {
            pigsyncTerm.lines[i - 1] = pigsyncTerm.lines[i];
        }
        pigsyncTerm.count = PIGSYNC_TERM_MAX_LINES - 1;
    }

    PigSyncTermLine& line = pigsyncTerm.lines[pigsyncTerm.count++];
    strncpy(line.text, buf, sizeof(line.text) - 1);
    line.text[sizeof(line.text) - 1] = '\0';
    line.len = strlen(line.text);
    line.reveal = 0;

    bool hasIncomplete = false;
    for (uint8_t i = 0; i < pigsyncTerm.count; i++) {
        if (pigsyncTerm.lines[i].reveal < pigsyncTerm.lines[i].len) {
            hasIncomplete = true;
            break;
        }
    }
    if (!hasIncomplete) {
        pigsyncTerm.nextCharAt = millis();
    }
}

static const char* pigsyncTermStatus(uint8_t flags) {
    if (flags & FLAG_LOW_BATTERY) return "LOWBAT";
    if (flags & FLAG_CALL_ACTIVE) return "SYNC";
    if (flags & FLAG_BUFFER_FULL) return "FULL";
    if (flags & FLAG_HUNTING) return "HUNT";
    return "IDLE";
}

static char pigsyncAlertCode(uint8_t flags, bool fromGrunt) {
    if (fromGrunt) {
        if (flags & BEACON_ALERT_HUNTING) return 'H';
        if (flags & BEACON_ALERT_LOW_BATTERY) return 'L';
        if (flags & BEACON_ALERT_STORAGE_FULL) return 'F';
        if (flags & BEACON_ALERT_CALL_ACTIVE) return 'C';
        if (flags & BEACON_ALERT_BOUNTY_MATCH) return 'B';
        return 'N';
    }

    if (flags & FLAG_HUNTING) return 'H';
    if (flags & FLAG_LOW_BATTERY) return 'L';
    if (flags & FLAG_BUFFER_FULL) return 'F';
    if (flags & FLAG_CALL_ACTIVE) return 'C';
    return 'N';
}

static void pigsyncTermReportDevice(const SirloinDevice* device) {
    if (!device) return;

    bool nameUnknown = (!device->hasGruntInfo || device->name[0] == '\0' ||
                        strncmp(device->name, "SIRLOIN", 7) == 0);
    char name[6] = {0};
    if (!nameUnknown && device->hasGruntInfo && device->name[0]) {
        strncpy(name, device->name, 5);
    } else {
        strcpy(name, "GHOST");
    }
    for (uint8_t i = 0; name[i]; i++) {
        if (name[i] >= 'a' && name[i] <= 'z') name[i] -= 32;
    }

    uint16_t caps = device->pendingCaptures;
    if (caps > 999) caps = 999;

    uint8_t batt = device->batteryPercent;
    uint8_t storage = device->storagePercent;
    uint16_t uptime = device->uptimeMin;
    if (uptime > 999) uptime = 999;

    char alert = pigsyncAlertCode(device->flags, device->hasGruntInfo);

    if (device->hasGruntInfo) {
        pigsyncTermPushLine("FATHER//%s RPT B%03u S%03u C%03u U%03u %c",
            name, batt, storage, caps, uptime, alert);
    } else {
        pigsyncTermPushLine("FATHER//%s RPT CAPS %03u STATUS %s",
            name, caps, pigsyncTermStatus(device->flags));
    }
}

static void pigsyncTermLogDevice(const SirloinDevice* device) {
    pigsyncTermReportDevice(device);
}

static uint32_t pigsyncTermCharDelay(char c) {
    uint32_t delayMs = 18 + random(0, 28);
    switch (c) {
        case '.':
        case '!':
        case '?':
        case ':':
            delayMs += 90 + random(0, 40);
            break;
        case ',':
        case ';':
            delayMs += 40 + random(0, 30);
            break;
        case ' ':
            delayMs += 10;
            break;
        default:
            break;
    }
    return delayMs;
}

// Check if terminal has any incomplete lines (still typing)
static bool pigsyncTermIsTyping() {
    for (uint8_t i = 0; i < pigsyncTerm.count; i++) {
        if (pigsyncTerm.lines[i].reveal < pigsyncTerm.lines[i].len) {
            return true;
        }
    }
    return false;
}

static void pigsyncTermTick() {
    if (pigsyncTerm.count == 0) return;

    int activeIndex = -1;
    for (uint8_t i = 0; i < pigsyncTerm.count; i++) {
        if (pigsyncTerm.lines[i].reveal < pigsyncTerm.lines[i].len) {
            activeIndex = i;
            break;
        }
    }
    if (activeIndex < 0) return;

    uint32_t now = millis();
    if (pigsyncTerm.nextCharAt == 0) pigsyncTerm.nextCharAt = now;
    if (now < pigsyncTerm.nextCharAt) return;

    PigSyncTermLine& line = pigsyncTerm.lines[activeIndex];
    if (line.reveal >= line.len) return;

    char c = line.text[line.reveal];
    line.reveal++;

    if (c != ' ' && now - pigsyncTerm.lastSfxAt > 45) {
        if (!SFX::isPlaying()) {
            SFX::play(SFX::TERMINAL_TICK);
            pigsyncTerm.lastSfxAt = now;
        }
    }

    pigsyncTerm.nextCharAt = now + pigsyncTermCharDelay(c);
}

// ==[ DIALOGUE OVERLAY FUNCTIONS ]==
static void drawDialogueOverlay(M5Canvas& canvas, int dialogueY) {
    if (!dialogueActive || dialogueReveal == 0) return;

    canvas.setTextColor(COLOR_FG, COLOR_BG);

    char buf[PIGSYNC_TERM_MAX_CHARS];
    memcpy(buf, dialogueLine, dialogueReveal);
    buf[dialogueReveal] = '\0';
    canvas.drawString(buf, 2, dialogueY);

}

static void pigsyncBuildStateLine(char* out, size_t len) {
    bool running = PigSyncMode::isRunning();
    bool connected = PigSyncMode::isConnected();
    bool scanning = PigSyncMode::isScanning();
    PigSyncMode::State state = PigSyncMode::getState();
    uint8_t deviceCount = PigSyncMode::getDeviceCount();
    uint8_t selected = PigSyncMode::getSelectedIndex();
    const SirloinDevice* device = (deviceCount > 0 && selected < deviceCount) ? PigSyncMode::getDevice(selected) : nullptr;

    char name[5] = "srl?";
    if (device && device->hasGruntInfo && device->name[0]) {
        strncpy(name, device->name, 4);
        name[4] = '\0';
    }

    uint8_t ch = connected ? PigSyncMode::getDataChannel() : PigSyncMode::getDiscoveryChannel();

    if (!running) {
        snprintf(out, len, "STATE IDLE");
        return;
    }

    if (!connected && (state == PigSyncMode::State::IDLE || state == PigSyncMode::State::SCANNING)) {
        if (deviceCount == 0) {
            snprintf(out, len, "SCAN CH%02d NO SIG", ch);
        } else {
            snprintf(out, len, "SEL %d/%d CH%02d ENTER", selected + 1, deviceCount, ch);
        }
        return;
    }

    switch (state) {
        case PigSyncMode::State::CONNECTING:
            snprintf(out, len, "CONNECT %s CH%02d", name, ch);
            break;
        case PigSyncMode::State::RINGING:
            snprintf(out, len, "RING %s", name);
            break;
        case PigSyncMode::State::CONNECTED_WAITING_READY:
            snprintf(out, len, "HANDSHAKE CH%02d", ch);
            break;
        case PigSyncMode::State::CONNECTED:
            snprintf(out, len, "LINK CH%02d", ch);
            break;
        case PigSyncMode::State::SYNCING:
        case PigSyncMode::State::WAITING_CHUNKS:
            snprintf(out, len, "SYNC %02d%% CH%02d", PigSyncMode::getSyncProgress(), ch);
            break;
        case PigSyncMode::State::SYNC_COMPLETE:
            snprintf(out, len, "DONE CH%02d", ch);
            break;
        case PigSyncMode::State::ERROR:
            snprintf(out, len, "ERROR %s", PigSyncMode::getLastError());
            break;
        default:
            snprintf(out, len, "STATE CH%02d", ch);
            break;
    }
}

static void updateDialogueTyping() {
    if (!dialogueActive) return;

    uint32_t now = millis();
    if (dialogueNextCharAt == 0) dialogueNextCharAt = now;
    if (now < dialogueNextCharAt) return;

    if (dialogueReveal < strlen(dialogueLine)) {
        char c = dialogueLine[dialogueReveal];
        dialogueReveal++;

        // Same typing timing as terminal, but NO SOUND
        dialogueNextCharAt = now + pigsyncTermCharDelay(c);
    }
}

static void setDialogueOverlay(uint8_t sequenceStep, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(dialogueLine, sizeof(dialogueLine), fmt, args);
    va_end(args);

    dialogueReveal = 0;
    dialogueNextCharAt = 0;
    dialogueClearTime = 0;
    dialogueActive = true;
    dialogueSequenceStep = sequenceStep;
}

static void pigsyncTermUpdateState() {
    bool running = PigSyncMode::isRunning();
    bool connected = PigSyncMode::isConnected();
    bool scanning = PigSyncMode::isScanning();
    PigSyncMode::State state = PigSyncMode::getState();
    uint8_t deviceCount = PigSyncMode::getDeviceCount();
    uint8_t selected = PigSyncMode::getSelectedIndex();
    const SirloinDevice* device = (deviceCount > 0 && selected < deviceCount) ? PigSyncMode::getDevice(selected) : nullptr;

    bool didReset = false;

    if (!pigsyncTerm.active) {
        pigsyncTerm.active = true;
        pigsyncTermReset(running);
        if (running) {
            SFX::play(SFX::PIGSYNC_BOOT);
        }
        pigsyncTerm.lastRunning = running;
        pigsyncTerm.lastConnected = connected;
        didReset = true;
    }

    if (pigsyncTerm.sessionActive && !connected &&
        (state == PigSyncMode::State::IDLE || state == PigSyncMode::State::SCANNING)) {
        pigsyncTermReset(true);
        didReset = true;
    }

    if (!didReset) {
        if (pigsyncTerm.lastConnected && !connected) {
            pigsyncTermReset(true);
            didReset = true;
        } else if (!pigsyncTerm.lastRunning && running) {
            pigsyncTermReset(true);
            SFX::play(SFX::PIGSYNC_BOOT);
            didReset = true;
        } else if (pigsyncTerm.lastRunning && !running) {
            pigsyncTermReset(false);
            pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_IDLE_PHRASES,
                sizeof(FATHER_IDLE_PHRASES) / sizeof(FATHER_IDLE_PHRASES[0]),
                &pigsyncTerm.lastIdleIdx));
            didReset = true;
        }
    }

    if (!running) {
        if (pigsyncTerm.count == 0) {
            pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_IDLE_PHRASES,
                sizeof(FATHER_IDLE_PHRASES) / sizeof(FATHER_IDLE_PHRASES[0]),
                &pigsyncTerm.lastIdleIdx));
        }
        pigsyncTerm.lastRunning = running;
        pigsyncTerm.lastConnected = connected;
        pigsyncTerm.lastScanning = scanning;
        pigsyncTerm.lastState = state;
        pigsyncTerm.lastDeviceCount = deviceCount;
        pigsyncTerm.lastSelected = selected;
        return;
    }

    if (scanning && !pigsyncTerm.lastScanning) {
        pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_PROBE_PHRASES,
            sizeof(FATHER_PROBE_PHRASES) / sizeof(FATHER_PROBE_PHRASES[0]),
            &pigsyncTerm.lastProbeIdx));
    }

    if (state != pigsyncTerm.lastState) {
        switch (state) {
            case PigSyncMode::State::CONNECTING:
                pigsyncTerm.sessionActive = true;
                if (device) {
                    char name[5] = {0};
                    if (device->hasGruntInfo && device->name[0]) {
                        strncpy(name, device->name, 4);
                    } else {
                        strcpy(name, "srl?");
                    }
                    pigsyncTermPushLine(pigsyncPickPhrase(FATHER_DIAL_PHRASES,
                        sizeof(FATHER_DIAL_PHRASES) / sizeof(FATHER_DIAL_PHRASES[0]),
                        &pigsyncTerm.lastDialIdx), name);
                } else {
                    pigsyncTermPushLine(pigsyncPickPhrase(FATHER_DIAL_PHRASES,
                        sizeof(FATHER_DIAL_PHRASES) / sizeof(FATHER_DIAL_PHRASES[0]),
                        &pigsyncTerm.lastDialIdx), "??");
                }
                break;
            case PigSyncMode::State::RINGING:
                pigsyncTerm.sessionActive = true;
                pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_RING_PHRASES,
                    sizeof(FATHER_RING_PHRASES) / sizeof(FATHER_RING_PHRASES[0]),
                    &pigsyncTerm.lastRingIdx));
                break;
            case PigSyncMode::State::CONNECTED_WAITING_READY:
                pigsyncTerm.sessionActive = true;
                pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_HANDSHAKE_PHRASES,
                    sizeof(FATHER_HANDSHAKE_PHRASES) / sizeof(FATHER_HANDSHAKE_PHRASES[0]),
                    &pigsyncTerm.lastHandshakeIdx));
                break;
            case PigSyncMode::State::CONNECTED:
                pigsyncTerm.sessionActive = true;
                pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_LIVE_PHRASES,
                    sizeof(FATHER_LIVE_PHRASES) / sizeof(FATHER_LIVE_PHRASES[0]),
                    &pigsyncTerm.lastLiveIdx));
                break;
            case PigSyncMode::State::SYNCING:
                pigsyncTerm.sessionActive = true;
                pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_TRANSFER_BEGIN_PHRASES,
                    sizeof(FATHER_TRANSFER_BEGIN_PHRASES) / sizeof(FATHER_TRANSFER_BEGIN_PHRASES[0]),
                    &pigsyncTerm.lastXferBeginIdx));
                break;
            case PigSyncMode::State::SYNC_COMPLETE:
                pigsyncTerm.sessionActive = true;
                pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_TRANSFER_END_PHRASES,
                    sizeof(FATHER_TRANSFER_END_PHRASES) / sizeof(FATHER_TRANSFER_END_PHRASES[0]),
                    &pigsyncTerm.lastXferEndIdx));
                break;
            case PigSyncMode::State::ERROR:
                pigsyncTermPushLine(pigsyncPickPhrase(FATHER_ERROR_PHRASES,
                    sizeof(FATHER_ERROR_PHRASES) / sizeof(FATHER_ERROR_PHRASES[0]),
                    &pigsyncTerm.lastErrorIdx), PigSyncMode::getLastError());
                break;
            default:
                break;
        }
    }

    if (state == PigSyncMode::State::CONNECTING ||
        state == PigSyncMode::State::RINGING ||
        state == PigSyncMode::State::CONNECTED_WAITING_READY ||
        state == PigSyncMode::State::CONNECTED ||
        state == PigSyncMode::State::SYNCING ||
        state == PigSyncMode::State::WAITING_CHUNKS ||
        state == PigSyncMode::State::SYNC_COMPLETE) {
        pigsyncTerm.sessionActive = true;
    }

    bool loggedDevice = false;
    if (deviceCount != pigsyncTerm.lastDeviceCount) {
        if (deviceCount == 0) {
            pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_NO_PIGS_PHRASES,
                sizeof(FATHER_NO_PIGS_PHRASES) / sizeof(FATHER_NO_PIGS_PHRASES[0]),
                &pigsyncTerm.lastNoPigsIdx));
        } else if (deviceCount > pigsyncTerm.lastDeviceCount) {
            pigsyncTermPushLine(pigsyncPickPhrase(FATHER_FOUND_PHRASES,
                sizeof(FATHER_FOUND_PHRASES) / sizeof(FATHER_FOUND_PHRASES[0]),
                &pigsyncTerm.lastFoundIdx), deviceCount);
            if (device) {
                pigsyncTermLogDevice(device);
                loggedDevice = true;
            }
        }
    }

    if (device && selected != pigsyncTerm.lastSelected) {
        char name[6] = {0};
        bool nameUnknown = (!device->hasGruntInfo || device->name[0] == '\0' ||
                            strncmp(device->name, "SIRLOIN", 7) == 0);
        if (!nameUnknown && device->hasGruntInfo && device->name[0]) {
            strncpy(name, device->name, 5);
        } else {
            strcpy(name, "GHOST");
        }
        for (uint8_t i = 0; name[i]; i++) {
            if (name[i] >= 'a' && name[i] <= 'z') name[i] -= 32;
        }
        pigsyncTermPushLine("FATHER//SELECT %d (%s)", selected, name);
        if (!loggedDevice) {
            pigsyncTermLogDevice(device);
            loggedDevice = true;
        }
    }

    if (device && !loggedDevice) {
        bool reportChanged = false;
        if (device->pendingCaptures != pigsyncTerm.lastCaps || device->flags != pigsyncTerm.lastFlags) {
            reportChanged = true;
        }
        if (device->hasGruntInfo) {
            if (device->batteryPercent != pigsyncTerm.lastBattery ||
                device->storagePercent != pigsyncTerm.lastStorage ||
                device->uptimeMin != pigsyncTerm.lastUptime) {
                reportChanged = true;
            }
        }

        if (reportChanged) {
            uint32_t now = millis();
            if (pigsyncTerm.lastReportAt > 0 && now - pigsyncTerm.lastReportAt < 7000) {
                reportChanged = false;
            } else {
                pigsyncTerm.lastReportAt = now;
            }
        }

        if (reportChanged) {
            pigsyncTermReportDevice(device);
        }
    }

    char revealedName[16] = {0};
    if (PigSyncMode::consumeNameReveal(revealedName, sizeof(revealedName))) {
        pigsyncTermPushLine(pigsyncPickPhrase(FATHER_NAME_PHRASES,
            sizeof(FATHER_NAME_PHRASES) / sizeof(FATHER_NAME_PHRASES[0]),
            nullptr), revealedName);
    }

    if (!connected &&
        (state == PigSyncMode::State::IDLE || state == PigSyncMode::State::SCANNING) &&
        deviceCount > 0) {
        uint32_t now = millis();
        bool insertedSide = false;
        if ((pigsyncTerm.lastArrowsAt == 0 || now - pigsyncTerm.lastArrowsAt > 60000) &&
            random(0, 12) == 0) {
            if (random(0, 2) == 0) {
                pigsyncTermPushLine("%s", pigsyncPickPhrase(FATHER_ARROWS_PHRASES,
                    sizeof(FATHER_ARROWS_PHRASES) / sizeof(FATHER_ARROWS_PHRASES[0]),
                    &pigsyncTerm.lastArrowsIdx));
            } else {
                pigsyncTermPushExistential();
            }
            pigsyncTerm.lastArrowsAt = now;
            insertedSide = true;
        }
        if (!insertedSide &&
            (pigsyncTerm.lastHintAt == 0 || now - pigsyncTerm.lastHintAt > 15000)) {
            if (!pigsyncTerm.hintShown && deviceCount > 0) {
                pigsyncTermPushLine("%s", FATHER_HINT_LINE);
                pigsyncTerm.hintShown = true;
            } else {
                pigsyncTermPushExistential();
            }
            pigsyncTerm.lastHintAt = now;
        }
    }

    if (device) {
        pigsyncTerm.lastCaps = device->pendingCaptures;
        pigsyncTerm.lastFlags = device->flags;
        if (device->hasGruntInfo) {
            pigsyncTerm.lastBattery = device->batteryPercent;
            pigsyncTerm.lastStorage = device->storagePercent;
            pigsyncTerm.lastUptime = device->uptimeMin;
        }
    }

    pigsyncTerm.lastRunning = running;
    pigsyncTerm.lastConnected = connected;
    pigsyncTerm.lastScanning = scanning;
    pigsyncTerm.lastState = state;
    pigsyncTerm.lastDeviceCount = deviceCount;
    pigsyncTerm.lastSelected = selected;

    // Monitor dialogue phase changes for terminal display
    // Show dialogue ONLY during ACTIVE call sessions, triggered by actual call events
    bool isInActiveCall = PigSyncMode::isConnected();
    uint8_t dialoguePhase = PigSyncMode::getDialoguePhase();

    // Track if we just entered an active call (for HELLO dialogue)
    static bool wasInActiveCall = false;
    bool justEnteredCall = isInActiveCall && !wasInActiveCall;
    wasInActiveCall = isInActiveCall;

    if (isInActiveCall) {
        // Show HELLO dialogue when call becomes active
        if (justEnteredCall && dialoguePhase == 0) {
            setDialogueOverlay(1, "POPS: %s", PigSyncMode::getPapaHelloPhrase());
        }
        // Show subsequent dialogue phases
        else if (dialoguePhase != pigsyncTerm.lastDialoguePhase && dialoguePhase > 0) {
            switch (dialoguePhase) {
                case 2:  // GOODBYE - show when sync completes
                    setDialogueOverlay(3, "POPS: %s", PigSyncMode::getPapaGoodbyePhrase());
                    break;
                case 3:  // COMPLETE
                    setDialogueOverlay(5, "FATHER//CALL COMPLETE");
                    break;
            }
        }
        pigsyncTerm.lastDialoguePhase = dialoguePhase;
    } else {
        // Reset when not in active call
        pigsyncTerm.lastDialoguePhase = 0xFF;  // Reset for next call
    }

    // ==[ DIALOGUE OVERLAY AUTO-CLEAR & SEQUENCE ]==
    updateDialogueTyping();

    if (dialogueActive && dialogueReveal >= strlen(dialogueLine)) {
        // Dialogue fully typed - start clear timer
        if (dialogueClearTime == 0) {
            dialogueClearTime = millis();
        }

        // Clear after PIGSYNC_PHRASE_DURATION (2500ms)
        if (millis() - dialogueClearTime > 2500) {
            // Handle dialogue sequence progression
            switch (dialogueSequenceStep) {
                case 1:  // Papa hello done → show Son hello
                    setDialogueOverlay(2, "SOP: %s", PigSyncMode::getSonHelloPhrase());
                    break;
                case 2:  // Son hello done → dialogue complete
                case 3:  // Papa goodbye done → show Son goodbye
                    setDialogueOverlay(4, "SOP: %s", PigSyncMode::getSonGoodbyePhrase());
                    break;
                case 4:  // Son goodbye done → dialogue complete
                case 5:  // Call complete done → dialogue complete
                default:
                    dialogueActive = false;
                    dialogueReveal = 0;
                    dialogueLine[0] = '\0';
                    dialogueSequenceStep = 0;
                    break;
            }
            dialogueClearTime = 0;
        }
    }
}

void Display::drawPigSyncDeviceSelect(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(TL_DATUM);
    uint32_t now = millis();

    // Title - terminal style
    canvas.setTextSize(2);
    canvas.setTextDatum(top_center);
    const char* header = (pigsyncTerm.header[0] != '\0') ? pigsyncTerm.header : FATHER_HEADER_DEFAULT;
    canvas.drawString(header, DISPLAY_W / 2, 2);
    canvas.setTextDatum(TL_DATUM);

    canvas.setTextSize(1);
    const int lineHeight = PIGSYNC_TERM_LINE_HEIGHT;
    const int logStartY = PIGSYNC_TERM_LOG_START_Y;

    pigsyncTermUpdateState();
    pigsyncTermTick();

    int y = logStartY;
    for (uint8_t i = 0; i < pigsyncTerm.count; i++) {
        const PigSyncTermLine& line = pigsyncTerm.lines[i];
        if (line.len == 0) {
            y += lineHeight;
            continue;
        }

        uint8_t reveal = line.reveal;
        if (reveal > line.len) reveal = line.len;

        char buf[PIGSYNC_TERM_MAX_CHARS];
        memcpy(buf, line.text, reveal);
        buf[reveal] = '\0';
        canvas.drawString(buf, 2, y);
        y += lineHeight;
    }

    bool hasIncomplete = false;
    for (uint8_t i = 0; i < pigsyncTerm.count; i++) {
        if (pigsyncTerm.lines[i].reveal < pigsyncTerm.lines[i].len) {
            hasIncomplete = true;
            break;
        }
    }

    if (!hasIncomplete && pigsyncTerm.count > 0) {
        static uint32_t cursorBlinkAt = 0;
        static bool cursorVisible = true;
        if (cursorBlinkAt == 0) cursorBlinkAt = now;
        if (now - cursorBlinkAt > 500) {
            cursorBlinkAt = now;
            cursorVisible = !cursorVisible;
        }

        if (cursorVisible) {
            const PigSyncTermLine& lastLine = pigsyncTerm.lines[pigsyncTerm.count - 1];
            char fullLine[PIGSYNC_TERM_MAX_CHARS];
            strncpy(fullLine, lastLine.text, sizeof(fullLine) - 1);
            fullLine[sizeof(fullLine) - 1] = '\0';
            int cursorX = 2 + canvas.textWidth(fullLine);
            int cursorY = logStartY + ((pigsyncTerm.count - 1) * lineHeight);
            int cursorW = canvas.textWidth("M");
            int cursorH = 8;
            if (cursorX + cursorW > DISPLAY_W - 2) {
                cursorX = DISPLAY_W - 2 - cursorW;
            }
            canvas.fillRect(cursorX, cursorY, cursorW, cursorH, COLOR_FG);
        }
    }

    int dialogueY = logStartY + (PIGSYNC_TERM_MAX_LINES * lineHeight);
    int stateY = dialogueY + lineHeight;

    if (stateY < MAIN_H - 4) {
        char stateLine[PIGSYNC_TERM_MAX_CHARS];
        pigsyncBuildStateLine(stateLine, sizeof(stateLine));
        canvas.drawString(stateLine, 2, stateY);
    }

    // Add dialogue overlay (draws last, on top)
    if (dialogueY < MAIN_H - 4) {
        drawDialogueOverlay(canvas, dialogueY);
    }
}

void Display::setBottomOverlay(const String& message) {
    if (message.length() == 0) {
        bottomOverlay[0] = '\0';
        return;
    }
    strncpy(bottomOverlay, message.c_str(), sizeof(bottomOverlay) - 1);
    bottomOverlay[sizeof(bottomOverlay) - 1] = '\0';
}

void Display::clearBottomOverlay() {
    bottomOverlay[0] = '\0';
}

void Display::setGPSStatus(bool hasFix) {
    gpsStatus = hasFix;
}

void Display::setWiFiStatus(bool connected) {
    wifiStatus = connected;
}

void Display::setMLStatus(bool active) {
    mlStatus = active;
}


// Helper functions for mode screens
void Display::drawModeInfo(M5Canvas& canvas, PorkchopMode mode) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);
    
    if (mode == PorkchopMode::OINK_MODE) {
        const auto& networks = OinkMode::getNetworks();
        int selIdx = OinkMode::getSelectionIndex();
        DetectedNetwork* target = OinkMode::getTarget();
        
        // Show current target being attacked
        if (target) {
            canvas.setTextColor(COLOR_SUCCESS);
            char ssidBuf[17];
            if (target->ssid[0] == '\0') {
                strncpy(ssidBuf, "<HIDDEN>", sizeof(ssidBuf) - 1);
                ssidBuf[sizeof(ssidBuf) - 1] = '\0';
            } else {
                strncpy(ssidBuf, target->ssid, sizeof(ssidBuf) - 1);
                ssidBuf[sizeof(ssidBuf) - 1] = '\0';
                for (size_t i = 0; ssidBuf[i]; ++i) {
                    if (ssidBuf[i] >= 'a' && ssidBuf[i] <= 'z') ssidBuf[i] = static_cast<char>(ssidBuf[i] - ('a' - 'A'));
                }
            }
            canvas.drawString("ATTACKING:", 2, 2);
            canvas.setTextColor(COLOR_ACCENT);
            canvas.drawString(ssidBuf, 2, 14);
            
            char info[32];
            snprintf(info, sizeof(info), "CH:%02d %ddB", target->channel, target->rssi);
            canvas.setTextColor(COLOR_FG);
            canvas.drawString(info, 2, 26);
        } else if (!networks.empty()) {
            canvas.setTextColor(COLOR_FG);
            canvas.drawString("SNIFFIN", 2, 2);
            canvas.setTextColor(COLOR_ACCENT);
            char buf[32];
            snprintf(buf, sizeof(buf), "FOUND %d TRUFFLES", (int)networks.size());
            canvas.drawString(buf, 2, 14);
        } else {
            canvas.drawString("HUNTING TRUFFLES", 2, MAIN_H / 2 - 5);
        }
        
        // Show stats at bottom
        canvas.setTextColor(COLOR_FG);
        uint16_t hsCount = OinkMode::getCompleteHandshakeCount();
        uint32_t deauthCnt = OinkMode::getDeauthCount();
        char stats[48];
        snprintf(stats, sizeof(stats), "N:%03d HS:%02d D:%04lu [ESC]=STOP", 
                 (int)networks.size(), hsCount, deauthCnt);
        canvas.drawString(stats, 2, MAIN_H - 12);
    } else if (mode == PorkchopMode::WARHOG_MODE) {
        // Show wardriving info
        canvas.drawString("WARDRIVING MODE ACTIVE", 2, MAIN_H - 25);
        canvas.drawString("COLLECTING GPS + WIFI DATA", 2, MAIN_H - 15);
    }
}

void Display::drawSettingsScreen(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    
    canvas.drawString("=== SETTINGS ===", DISPLAY_W / 2, 5);
    
    canvas.setTextDatum(top_left);
    int y = 20;
    canvas.drawString("Sound: ON", 10, y); y += 12;
    canvas.drawString("Brightness: 100%", 10, y); y += 12;
    canvas.drawString("Auto-save HS: ON", 10, y); y += 12;
    canvas.drawString("CH Hop: 100ms", 10, y); y += 12;
    canvas.drawString("Deauth delay: 50ms", 10, y);
    
    canvas.setTextDatum(top_center);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("[BKSP] TO GO BACK", DISPLAY_W / 2, MAIN_H - 12);
}

// Hacker quotes for About screen
static const char* ABOUT_QUOTES[] = {
    "HACK THE PLANET",
    "SHALL WE PLAY A GAME",
    "sudo make me bacon",
    "root@porkchop:~#",
    "WHILE(1) { PWN(); }",
    "#!/usr/bin/oink",
    "0WN3D BY 0ct0",
    "CURIOSITY IS NOT A CRIME",
    "MY CRIME IS CURIOSITY",
    "INFORMATION WANTS TO BE FREE",
    "SMASH THE STACK",
    "THERE IS NO PATCH",
    "TRUST NO AP",
    "PROMISCUOUS BY NATURE",
    "802.11 WARL0RD",
    "0xDEADP0RK",
    "SEGFAULT IN THE MATRIX",
    "PACKET OR GTFO",
    "THE CONSCIENCE OF A HACKER",
    "EXPLOIT ADAPT OVERCOME"
};
static const int ABOUT_QUOTES_COUNT = sizeof(ABOUT_QUOTES) / sizeof(ABOUT_QUOTES[0]);
static int aboutQuoteIndex = 0;
static int aboutEnterCount = 0;
static bool aboutAchievementShown = false;

void Display::resetAboutState() {
    // Pick new random quote each time we enter About
    aboutQuoteIndex = random(0, ABOUT_QUOTES_COUNT);
    aboutEnterCount = 0;
    aboutAchievementShown = false;
}

void Display::onAboutEnterPressed() {
    aboutEnterCount++;
    
    // Easter egg: 5 presses unlocks achievement
    if (aboutEnterCount >= 5 && !aboutAchievementShown) {
        if (!XP::hasAchievement(ACH_ABOUT_JUNKIE)) {
            XP::unlockAchievement(ACH_ABOUT_JUNKIE);
            showToast("AB0UT_JUNK13 UNLOCKED!");
        }
        aboutAchievementShown = true;
    }
    
    // Cycle to next quote
    aboutQuoteIndex = (aboutQuoteIndex + 1) % ABOUT_QUOTES_COUNT;
}

void Display::drawAboutScreen(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    
    // Title
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("M5PORKCHOP", DISPLAY_W / 2, 5);
    
    // Version
    canvas.setTextSize(1);
    canvas.drawString("V" BUILD_VERSION, DISPLAY_W / 2, 25);
    
    // Author, 0ct0 stays lowercase - it's the handle
    canvas.setTextColor(COLOR_FG);
    canvas.drawString("BY 0ct0", DISPLAY_W / 2, 38);
    
    // GitHub
    canvas.drawString("GITHUB.COM/0CT0SEC/M5PORKCHOP", DISPLAY_W / 2, 50);
    
    // Commit hash, uppercase the value
    canvas.setTextColor(COLOR_ACCENT);
    char commitBuf[32];
    char commitStr[16];
    strncpy(commitStr, BUILD_COMMIT, sizeof(commitStr) - 1);
    commitStr[sizeof(commitStr) - 1] = '\0';
    for (size_t i = 0; commitStr[i]; ++i) {
        if (commitStr[i] >= 'a' && commitStr[i] <= 'z') commitStr[i] = static_cast<char>(commitStr[i] - ('a' - 'A'));
    }
    snprintf(commitBuf, sizeof(commitBuf), "COMMIT: %s", commitStr);
    canvas.drawString(commitBuf, DISPLAY_W / 2, 64);
    
    // Random quote
    canvas.setTextColor(COLOR_FG);
    char quoteBuf[48];
    snprintf(quoteBuf, sizeof(quoteBuf), "\"%s\"", ABOUT_QUOTES[aboutQuoteIndex]);
    canvas.drawString(quoteBuf, DISPLAY_W / 2, 78);
    
    // Easter egg hint
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("[ENTER] ???", DISPLAY_W / 2, MAIN_H - 12);
}

void Display::drawFileTransferScreen(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("FILE TRANSFER", DISPLAY_W / 2, 5);
    
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);

    char line1[64] = {0};
    char line2[64] = {0};
    char line3[64] = {0};
    bool hasLine2 = false;
    bool hasLine3 = false;

    if (FileServer::isConnecting()) {
        strncpy(line1, "STATE: CONNECTING", sizeof(line1) - 1);
        snprintf(line2, sizeof(line2), "SSID: %s", Config::wifi().otaSSID);
        snprintf(line3, sizeof(line3), "%s", FileServer::getStatus());
        hasLine2 = true;
        hasLine3 = true;
    } else if (FileServer::isRunning() && FileServer::isConnected()) {
        strncpy(line1, "STATE: CONNECTED", sizeof(line1) - 1);
        char ipBuf[32];
        IPAddress ip = WiFi.localIP();
        snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u",
                 ip[0], ip[1], ip[2], ip[3]);
        snprintf(line2, sizeof(line2), "HTTP://%s", ipBuf);
        strncpy(line3, "HTTP://PORKCHOP.LOCAL", sizeof(line3) - 1);
        hasLine2 = true;
        hasLine3 = true;
    } else if (FileServer::isRunning()) {
        strncpy(line1, "STATE: LINK DEAD", sizeof(line1) - 1);
        strncpy(line2, "RETRY HACK", sizeof(line2) - 1);
        snprintf(line3, sizeof(line3), "%s", FileServer::getStatus());
        hasLine2 = true;
        hasLine3 = true;
    } else {
        if (Config::wifi().otaSSID[0] != '\0') {
            strncpy(line1, "STATE: FAILED", sizeof(line1) - 1);
            snprintf(line2, sizeof(line2), "SSID: %s", Config::wifi().otaSSID);
            snprintf(line3, sizeof(line3), "%s", FileServer::getStatus());
            hasLine2 = true;
            hasLine3 = true;
        } else {
            strncpy(line1, "STATE: NO CREDS", sizeof(line1) - 1);
            strncpy(line2, "SET SSID IN SETTINGS", sizeof(line2) - 1);
            hasLine2 = true;
        }
    }

    auto toUpperInPlace = [](char* s) {
        if (!s) return;
        for (size_t i = 0; s[i]; ++i) {
            if (s[i] >= 'a' && s[i] <= 'z') s[i] = static_cast<char>(s[i] - ('a' - 'A'));
        }
    };

    toUpperInPlace(line1);
    toUpperInPlace(line2);
    toUpperInPlace(line3);

    canvas.drawString(line1, DISPLAY_W / 2, 28);
    if (hasLine2 && line2[0] != '\0') {
        canvas.drawString(line2, DISPLAY_W / 2, 40);
    }
    if (hasLine3 && line3[0] != '\0') {
        canvas.drawString(line3, DISPLAY_W / 2, 52);
    }

    uint64_t rxBytes = FileServer::getSessionRxBytes();
    uint64_t txBytes = FileServer::getSessionTxBytes();
    uint32_t uploadCount = FileServer::getSessionUploadCount();
    uint32_t downloadCount = FileServer::getSessionDownloadCount();

    char rxValue[16];
    char txValue[16];
    char rxLine[32];
    char txLine[32];
    char filesLine[32];

    auto formatSessionMB = [](char* out, size_t len, uint64_t bytes) {
        const uint64_t mb100 = (bytes * 100ULL) / (1024ULL * 1024ULL);
        uint64_t whole = mb100 / 100ULL;
        uint64_t frac = mb100 % 100ULL;
        snprintf(out, len, "%03llu.%02llu MB",
            static_cast<unsigned long long>(whole),
            static_cast<unsigned long long>(frac));
    };

    formatSessionMB(rxValue, sizeof(rxValue), rxBytes);
    formatSessionMB(txValue, sizeof(txValue), txBytes);
    snprintf(rxLine, sizeof(rxLine), "SESSION RX: %s", rxValue);
    snprintf(txLine, sizeof(txLine), "SESSION TX: %s", txValue);
    snprintf(filesLine, sizeof(filesLine), "FILES UP: %03u DOWN: %03u",
        (unsigned)uploadCount, (unsigned)downloadCount);

    canvas.drawString(rxLine, DISPLAY_W / 2, 66);
    canvas.drawString(txLine, DISPLAY_W / 2, 78);
    canvas.drawString(filesLine, DISPLAY_W / 2, 90);

    static uint64_t lastRxBytes = 0;
    static uint64_t lastTxBytes = 0;
    static uint32_t lastTickAt = 0;
    static bool tickPending = false;

    if (rxBytes < lastRxBytes || txBytes < lastTxBytes) {
        lastRxBytes = rxBytes;
        lastTxBytes = txBytes;
        tickPending = false;
    } else if (rxBytes != lastRxBytes || txBytes != lastTxBytes) {
        lastRxBytes = rxBytes;
        lastTxBytes = txBytes;
        tickPending = true;
    }

    if (tickPending && FileServer::isRunning() && FileServer::isConnected()) {
        uint32_t now = millis();
        if (now - lastTickAt >= 250) {
            if (!SFX::isPlaying()) {
                SFX::play(SFX::TERMINAL_TICK);
            }
            lastTickAt = now;
            tickPending = false;
        }
    }
}

void Display::resetDimTimer() {
    lastActivityTime = millis();
    if (screenForcedOff) {
        screenForcedOff = false;
        dimmed = false;
        uint8_t brightness = Config::personality().brightness;
        M5.Display.setBrightness(brightness * 255 / 100);
        return;
    }
    if (dimmed) {
        // Restore full brightness
        dimmed = false;
        uint8_t brightness = Config::personality().brightness;
        M5.Display.setBrightness(brightness * 255 / 100);
    }
}

void Display::toggleScreenPower() {
    screenForcedOff = !screenForcedOff;
    if (screenForcedOff) {
        dimmed = true;
        M5.Display.setBrightness(0);
        return;
    }

    dimmed = false;
    lastActivityTime = millis();
    uint8_t brightness = Config::personality().brightness;
    M5.Display.setBrightness(brightness * 255 / 100);
}

void Display::updateDimming() {
    if (screenForcedOff) return;
    uint16_t timeout = Config::personality().dimTimeout;
    if (timeout == 0) return;  // Dimming disabled
    
    uint32_t elapsed = (millis() - lastActivityTime) / 1000;
    
    if (!dimmed && elapsed >= timeout) {
        // Time to dim
        dimmed = true;
        uint8_t dimLevel = Config::personality().dimLevel;
        M5.Display.setBrightness(dimLevel * 255 / 100);
    }
}

// Screenshot constants
static const int SCREENSHOT_RETRY_COUNT = 3;
static const int SCREENSHOT_RETRY_DELAY_MS = 10;

// Helper: find next screenshot number by scanning directory
static uint16_t getNextScreenshotNumber() {
    uint16_t maxNum = 0;
    
    const char* shotsDir = SDLayout::screenshotsDir();
    File dir = SD.open(shotsDir);
    if (!dir || !dir.isDirectory()) {
        return 1;
    }
    
    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        entry.close();
        
        // Parse "screenshotNNN.bmp" format
        if (name.startsWith("screenshot") && name.endsWith(".bmp")) {
            String numStr = name.substring(10, name.length() - 4);
            uint16_t num = numStr.toInt();
            if (num > maxNum) maxNum = num;
        }
    }
    dir.close();
    
    return maxNum + 1;
}

bool Display::takeScreenshot() {
    // Prevent re-entry
    if (snapping) return false;
    
    // Check SD availability
    if (!Config::isSDAvailable()) {
        showToast("NO SD CARD");
        return false;
    }
    
    snapping = true;
    
    // Ensure screenshots directory exists
    const char* shotsDir = SDLayout::screenshotsDir();
    if (!SD.exists(shotsDir)) {
        SD.mkdir(shotsDir);
    }
    
    // Find next available number
    uint16_t num = getNextScreenshotNumber();
    char path[48];
    snprintf(path, sizeof(path), "%s/screenshot%03d.bmp", shotsDir, num);
    
    Serial.printf("[DISPLAY] Taking screenshot: %s\n", path);
    
    // Open file with retry
    File file;
    for (int retry = 0; retry < SCREENSHOT_RETRY_COUNT; retry++) {
        file = SD.open(path, FILE_WRITE);
        if (file) break;
        delay(SCREENSHOT_RETRY_DELAY_MS);
    }
    
    if (!file) {
        Serial.println("[DISPLAY] Failed to open screenshot file");
        showToast("SD WRITE FAILED");
        snapping = false;
        return false;
    }
    
    // BMP file structure for 240x135 24-bit image
    int image_width = DISPLAY_W;
    int image_height = DISPLAY_H;
    
    // Horizontal lines must be padded to multiple of 4 bytes
    const uint32_t pad = (4 - (3 * image_width) % 4) % 4;
    uint32_t filesize = 54 + (3 * image_width + pad) * image_height;
    
    // BMP header, 54 bytes
    unsigned char header[54] = {
        'B', 'M',           // BMP signature
        0, 0, 0, 0,         // File size
        0, 0, 0, 0,         // Reserved
        54, 0, 0, 0,        // Pixel data offset
        40, 0, 0, 0,        // DIB header size
        0, 0, 0, 0,         // Width
        0, 0, 0, 0,         // Height
        1, 0,               // Color planes
        24, 0,              // Bits per pixel
        0, 0, 0, 0,         // Compression, none
        0, 0, 0, 0,         // Image size, can be 0 for uncompressed
        0, 0, 0, 0,         // Horizontal resolution
        0, 0, 0, 0,         // Vertical resolution
        0, 0, 0, 0,         // Colors in palette
        0, 0, 0, 0          // Important colors
    };
    
    // Fill in size fields
    for (uint32_t i = 0; i < 4; i++) {
        header[2 + i] = (filesize >> (8 * i)) & 0xFF;
        header[18 + i] = (image_width >> (8 * i)) & 0xFF;
        header[22 + i] = (image_height >> (8 * i)) & 0xFF;
    }
    
    file.write(header, 54);
    
    // Line buffer with padding
    unsigned char line_data[image_width * 3 + pad];
    
    // Initialize padding bytes to 0
    for (int i = image_width * 3; i < image_width * 3 + (int)pad; i++) {
        line_data[i] = 0;
    }
    
    // BMP stores bottom-to-top, so read from bottom up
    for (int y = image_height - 1; y >= 0; y--) {
        // Read one line of RGB data from display
        M5.Display.readRectRGB(0, y, image_width, 1, line_data);
        
        // Swap R and B, BMP uses BGR order
        for (int x = 0; x < image_width; x++) {
            unsigned char temp = line_data[x * 3];
            line_data[x * 3] = line_data[x * 3 + 2];
            line_data[x * 3 + 2] = temp;
        }
        
        file.write(line_data, image_width * 3 + pad);
    }
    
    file.close();
    
    Serial.printf("[DISPLAY] Screenshot saved: %s (%lu bytes)\n", path, filesize);
    
    // Show success toast
    char msg[32];
    snprintf(msg, sizeof(msg), "SNAP! #%d", num);
    showToast(msg);

    snapping = false;
    return true;
}

void Display::setUploadProgress(bool inProgress, uint8_t progress, const char* status) {
    uploadInProgress = inProgress;
    uploadProgress = progress;
    if (status) {
        strncpy(uploadStatus, status, sizeof(uploadStatus) - 1);
        uploadStatus[sizeof(uploadStatus) - 1] = '\0';
    } else {
        uploadStatus[0] = '\0';
    }
    uploadStartTime = millis();
}

bool Display::shouldShowUploadProgress() {
    // Show upload progress while upload is in progress
    return uploadInProgress && (millis() - uploadStartTime) < 60000;  // 60 seconds
}

void Display::drawUploadProgress(M5Canvas& topBar) {
    // Draw upload progress in top bar (similar to XP notification)
    // Format: "UPLOAD XX% [::.]"

    // Use same styling as XP notification: inverted colors
    topBar.fillSprite(COLOR_FG);  // Same as XP: use FG color as background
    topBar.setTextColor(COLOR_BG);  // Same as XP: use BG color as text
    topBar.setTextSize(1);
    topBar.setTextDatum(top_left);  // Align left part to left

    // Build progress string with animated indicator
    uint8_t progressDot = (millis() / 500) % 3;  // Change every 500ms
    const char* progressStr;
    switch(progressDot) {
        case 0: progressStr = "::."; break;
        case 1: progressStr = ":.:"; break;
        case 2: progressStr = ".::"; break;
        default: progressStr = "::."; break;
    }

    char progressText[32];
    snprintf(progressText, sizeof(progressText), "UPLOAD %d%% %s", uploadProgress, progressStr);

    // Draw the progress text at same Y position as XP notification
    topBar.drawString(progressText, 2, 3);  // Same Y=3 as XP notification
}

void Display::drawUploadProgressDirect() {
    // Draw upload progress directly to physical display when sprites are suspended
    // Format: "UPLOAD XX% [::.]"

    // Build progress string with animated indicator
    uint8_t progressDot = (millis() / 500) % 3;  // Change every 500ms
    const char* progressStr;
    switch(progressDot) {
        case 0: progressStr = "::."; break;
        case 1: progressStr = ":.:"; break;
        case 2: progressStr = ".::"; break;
        default: progressStr = "::."; break;
    }

    char progressText[32];
    snprintf(progressText, sizeof(progressText), "UPLOAD %d%% %s", uploadProgress, progressStr);

    // Use same styling as XP notification: inverted theme colors
    // Get theme colors dynamically
    uint16_t fgColor = getColorFG();
    uint16_t bgColor = getColorBG();

    // Draw in top-left corner directly to physical display with inverted colors (like XP notification)
    M5Cardputer.Display.setTextColor(bgColor, fgColor);  // Use BG color as text, FG color as background
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(2, 3);  // Same Y=3 as XP notification
    M5Cardputer.Display.print(progressText);
}

void Display::clearUploadProgress() {
    uploadInProgress = false;
    uploadProgress = 0;
    uploadStatus[0] = '\0';
}
