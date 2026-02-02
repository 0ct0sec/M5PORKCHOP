// SD Format Menu - destructive SD card formatting UI
// CRITICAL MODE: Stops all system operations, requires reboot after
// Runs bar-less to maximize RAM for disk operations

#include "sd_format_menu.h"
#include "display.h"
#include "../core/config.h"
#include "../core/network_recon.h"
#include "../web/fileserver.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

// ============================================================================
// CONSTANTS
// ============================================================================
static const uint16_t DIALOG_WIDTH = 220;
static const uint16_t DIALOG_HEIGHT = 90;
static const uint32_t REBOOT_DELAY_MS = 2000;  // Show "REBOOTING" for 2s

// ============================================================================
// HINT POOL (flash-resident, project style)
// ============================================================================
static const char* const H_SD_FORMAT[] = {
    "FAT32 OR BUST. NO EXCEPTIONS.",
    "WIPE THE PAST. FORMAT THE FUTURE.",
    "SD CARD REBORN. HEAP UNAFFECTED.",
    "ERASING: THERAPEUTIC. REBUILDING: OPTIONAL.",
    "CLEAN SLATE. DIRTY HANDS."
};
const char* const SdFormatMenu::HINTS[] = {
    H_SD_FORMAT[0], H_SD_FORMAT[1], H_SD_FORMAT[2], H_SD_FORMAT[3], H_SD_FORMAT[4]
};
const uint8_t SdFormatMenu::HINT_COUNT = sizeof(H_SD_FORMAT) / sizeof(H_SD_FORMAT[0]);

// ============================================================================
// STATIC MEMBER INITIALIZATION
// ============================================================================
bool SdFormatMenu::active = false;
bool SdFormatMenu::keyWasPressed = false;
SdFormatMenu::State SdFormatMenu::state = SdFormatMenu::State::CONFIRM_ENTRY;
SDFormat::Result SdFormatMenu::lastResult = {};
bool SdFormatMenu::hasResult = false;
SDFormat::FormatMode SdFormatMenu::formatMode = SDFormat::FormatMode::QUICK;
uint8_t SdFormatMenu::progressPercent = 0;
char SdFormatMenu::progressStage[32] = "";
uint8_t SdFormatMenu::hintIndex = 0;
bool SdFormatMenu::barsHidden = false;
bool SdFormatMenu::systemStopped = false;

// ============================================================================
// PUBLIC API
// ============================================================================

void SdFormatMenu::show() {
    active = true;
    keyWasPressed = true;  // Ignore the Enter that brought us here
    state = State::CONFIRM_ENTRY;  // Start with entry warning
    hasResult = false;
    formatMode = SDFormat::FormatMode::QUICK;
    progressPercent = 0;
    progressStage[0] = '\0';
    hintIndex = esp_random() % HINT_COUNT;
    barsHidden = true;   // Hide bars immediately - full screen mode from start
    systemStopped = false;
    Display::clearBottomOverlay();
}

void SdFormatMenu::hide() {
    // If we entered the format mode (past CONFIRM_ENTRY), we MUST reboot
    if (systemStopped) {
        doReboot();
        return;  // Never reached
    }
    
    active = false;
    barsHidden = false;
    Display::clearBottomOverlay();
}

void SdFormatMenu::update() {
    if (!active) return;
    if (state == State::WORKING) {
        startFormat();
        return;
    }
    handleInput();
}

const char* SdFormatMenu::getSelectedDescription() {
    if (!active) return "";
    
    switch (state) {
        case State::CONFIRM_ENTRY:
            return "CRITICAL: SYSTEM WILL STOP";
        case State::SELECT:
            return (formatMode == SDFormat::FormatMode::FULL)
                ? "FULL: ZERO FILL + FORMAT (SLOW)"
                : "QUICK: FORMAT ONLY (FAST)";
        case State::CONFIRM:
            return "!! ALL DATA WILL BE LOST !!";
        case State::WORKING:
            return "DO NOT REMOVE SD CARD";
        case State::RESULT:
            return lastResult.success ? "FORMAT COMPLETE" : "FORMAT FAILED";
        default:
            return HINTS[hintIndex];
    }
}

// ============================================================================
// SYSTEM CONTROL
// ============================================================================

void SdFormatMenu::stopEverything() {
    if (systemStopped) return;
    
    // Stop FileServer if running
    if (FileServer::isRunning()) {
        FileServer::stop();
    }
    
    // Stop background network reconnaissance
    NetworkRecon::stop();
    
    // Full WiFi shutdown to reclaim heap
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);  // Allow WiFi stack to settle
    
    systemStopped = true;
    barsHidden = true;  // Hide bars to save RAM
    
    Serial.println("[SD_FORMAT] System stopped for format operation");
}

void SdFormatMenu::doReboot() {
    // Full-screen reboot message
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(getColorFG());
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(2);
    M5.Display.drawString("REBOOTING...", M5.Display.width() / 2, M5.Display.height() / 2);
    
    delay(REBOOT_DELAY_MS);
    ESP.restart();
    // Never reached
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

void SdFormatMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    if (keyWasPressed) return;
    keyWasPressed = true;

    bool up = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');
    bool back = M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE);
    auto keys = M5Cardputer.Keyboard.keysState();

    // ---- CONFIRM_ENTRY STATE ----
    // Entry warning dialog: Y to enter, N to bail
    if (state == State::CONFIRM_ENTRY) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            // User confirmed entry - stop everything and proceed
            stopEverything();
            state = State::SELECT;
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') || back) {
            // User cancelled - return to menu (nothing stopped)
            active = false;
            barsHidden = false;
            Display::clearBottomOverlay();
            return;
        }
        return;  // Ignore other keys
    }

    // ---- CONFIRM STATE (format confirmation) ----
    if (state == State::CONFIRM) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            state = State::WORKING;
        } else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') || back) {
            state = State::SELECT;
        }
        return;
    }

    // ---- RESULT STATE ----
    if (state == State::RESULT) {
        // Any key triggers reboot
        if (keys.enter || back || anyPressed) {
            doReboot();  // Never returns
        }
        return;
    }

    // ---- SELECT STATE ----
    if (state == State::SELECT) {
        // Navigation toggles format mode (list-style selection)
        if (up || down) {
            formatMode = (formatMode == SDFormat::FormatMode::QUICK)
                ? SDFormat::FormatMode::FULL
                : SDFormat::FormatMode::QUICK;
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
            // Check SD availability before confirming
            if (!Config::isSDAvailable()) {
                Display::notify(NoticeKind::WARNING, "SD NOT MOUNTED");
                return;
            }
            state = State::CONFIRM;
            return;
        }
        // Backspace in SELECT means exit - but we must reboot since system is stopped
        if (back) {
            // Show warning before reboot - user may have pressed by accident
            Display::notify(NoticeKind::WARNING, "REBOOT REQUIRED", 1500);
            delay(1500);
            doReboot();  // Never returns
        }
        return;
    }
}

void SdFormatMenu::startFormat() {
    lastResult = SDFormat::formatCard(formatMode, true, onFormatProgress);
    hasResult = true;
    state = State::RESULT;
}

void SdFormatMenu::onFormatProgress(const char* stage, uint8_t percent) {
    // Reset watchdog - SPI transfers during display update can take time
    esp_task_wdt_reset();
    
    progressPercent = percent;
    if (stage && stage[0]) {
        strncpy(progressStage, stage, sizeof(progressStage) - 1);
        progressStage[sizeof(progressStage) - 1] = '\0';
    } else {
        strncpy(progressStage, "WORKING", sizeof(progressStage) - 1);
    }
    Display::showProgress(progressStage, percent);
}

// ============================================================================
// DRAWING - Full screen (no bars) for maximum RAM
// ============================================================================

void SdFormatMenu::draw(M5Canvas& canvas) {
    if (!active) return;

    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    canvas.fillSprite(bg);
    canvas.setTextColor(fg);

    // Title with icon (matching menu.cpp style)
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    
    // Different title based on state
    if (state == State::CONFIRM_ENTRY) {
        canvas.drawString("!! WARNING !!", DISPLAY_W / 2, 2);
    } else {
        canvas.drawString("SD FORMAT SD", DISPLAY_W / 2, 2);
    }
    canvas.drawLine(10, 20, DISPLAY_W - 10, 20, fg);

    // Dispatch to state-specific drawing
    switch (state) {
        case State::CONFIRM_ENTRY:
            drawConfirmEntry(canvas);
            break;
        case State::SELECT:
            drawSelect(canvas);
            break;
        case State::CONFIRM:
            // Draw SELECT as background, then CONFIRM overlay
            drawSelect(canvas);
            drawConfirm(canvas);
            break;
        case State::WORKING:
            drawWorking(canvas);
            break;
        case State::RESULT:
            drawResult(canvas);
            break;
    }
}

void SdFormatMenu::drawConfirmEntry(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    canvas.setTextDatum(top_center);
    int centerX = DISPLAY_W / 2;
    int y = 26;

    // Warning message
    canvas.setTextSize(1);
    canvas.drawString("THIS WILL STOP ALL", centerX, y);
    y += 12;
    canvas.drawString("SYSTEM OPERATIONS:", centerX, y);
    y += 14;
    
    canvas.drawString("- WIFI SHUTDOWN", centerX, y);
    y += 10;
    canvas.drawString("- NETWORK SCAN STOP", centerX, y);
    y += 10;
    canvas.drawString("- FILESERVER STOP", centerX, y);
    y += 14;
    
    canvas.setTextSize(2);
    canvas.setTextColor(fg);
    canvas.drawString("REBOOT REQUIRED", centerX, y);
    y += 20;
    
    // Controls
    canvas.setTextSize(1);
    canvas.drawString("[Y] ENTER  [N] CANCEL", centerX, y);
}

void SdFormatMenu::drawSelect(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);

    int y = 26;
    const int lineH = 18;
    const int itemPadX = 6;

    // QUICK option
    bool quickSelected = (formatMode == SDFormat::FormatMode::QUICK);
    if (quickSelected) {
        canvas.fillRect(itemPadX, y, DISPLAY_W - itemPadX * 2, lineH, fg);
        canvas.setTextColor(bg);
    } else {
        canvas.setTextColor(fg);
    }
    canvas.drawString(quickSelected ? "> QUICK" : "  QUICK", 10, y);
    canvas.setTextDatum(top_right);
    canvas.drawString("FAST", DISPLAY_W - 10, y);
    canvas.setTextDatum(top_left);
    y += lineH;

    // Reset colors
    canvas.setTextColor(fg);

    // FULL option
    bool fullSelected = (formatMode == SDFormat::FormatMode::FULL);
    if (fullSelected) {
        canvas.fillRect(itemPadX, y, DISPLAY_W - itemPadX * 2, lineH, fg);
        canvas.setTextColor(bg);
    } else {
        canvas.setTextColor(fg);
    }
    canvas.drawString(fullSelected ? "> FULL" : "  FULL", 10, y);
    canvas.setTextDatum(top_right);
    canvas.drawString("SLOW", DISPLAY_W - 10, y);
    canvas.setTextDatum(top_left);
    y += lineH + 8;

    // Reset colors and show hint
    canvas.setTextColor(fg);
    canvas.setTextDatum(top_center);
    
    // Mode description - larger text for important info
    canvas.setTextSize(2);
    const char* modeHint = fullSelected
        ? "ZERO-FILL + FORMAT"
        : "FORMAT ONLY";
    canvas.drawString(modeHint, DISPLAY_W / 2, y);
    y += 20;

    // Nav hints - smaller text
    canvas.setTextSize(1);
    canvas.drawString("^v NAV  ENTER=OK", DISPLAY_W / 2, y);
}

void SdFormatMenu::drawWorking(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);

    int y = 30;

    // Stage label
    if (progressStage[0]) {
        canvas.drawString(progressStage, DISPLAY_W / 2, y);
    } else {
        canvas.drawString("FORMATTING", DISPLAY_W / 2, y);
    }
    y += 20;

    // Progress bar
    const int barX = 20;
    const int barY = y;
    const int barW = DISPLAY_W - 40;
    const int barH = 14;

    canvas.drawRect(barX, barY, barW, barH, fg);
    int fillW = (barW - 4) * progressPercent / 100;
    if (fillW > 0) {
        canvas.fillRect(barX + 2, barY + 2, fillW, barH - 4, fg);
    }
    y += barH + 8;

    // Percentage
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", progressPercent);
    canvas.drawString(pctBuf, DISPLAY_W / 2, y);
    y += 18;

    // Warning
    canvas.setTextSize(1);
    canvas.drawString("DO NOT POWER OFF", DISPLAY_W / 2, y);
}

void SdFormatMenu::drawResult(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);

    int y = 28;

    // Result status
    canvas.drawString(lastResult.success ? "SUCCESS" : "FAILED", DISPLAY_W / 2, y);
    y += 22;

    // Message
    canvas.setTextSize(1);
    if (lastResult.message[0] != '\0') {
        canvas.drawString(lastResult.message, DISPLAY_W / 2, y);
        y += 14;
    }
    if (lastResult.usedFallback) {
        canvas.drawString("(FALLBACK WIPE USED)", DISPLAY_W / 2, y);
        y += 14;
    }

    y += 8;
    
    // Reboot notice
    canvas.setTextSize(2);
    canvas.drawString("PRESS ANY KEY", DISPLAY_W / 2, y);
    y += 18;
    canvas.setTextSize(1);
    canvas.drawString("TO REBOOT DEVICE", DISPLAY_W / 2, y);
}

void SdFormatMenu::drawConfirm(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    // Modal dimensions (matching menu.cpp modal style: 220x90)
    const int boxW = 220;
    const int boxH = 90;
    const int boxX = (DISPLAY_W - boxW) / 2;
    const int boxY = (MAIN_H - boxH) / 2 - 5;
    const int radius = 6;

    // Background with border (inverted colors like menu modal)
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, radius, fg);
    canvas.drawRoundRect(boxX, boxY, boxW, boxH, radius, bg);

    canvas.setTextColor(bg);
    canvas.setTextDatum(top_center);
    int centerX = DISPLAY_W / 2;

    // Title
    canvas.setTextSize(2);
    canvas.drawString("!! FORMAT SD !!", centerX, boxY + 6);
    canvas.drawLine(boxX + 10, boxY + 24, boxX + boxW - 10, boxY + 24, bg);

    // Mode info
    canvas.setTextSize(1);
    const char* modeLabel = (formatMode == SDFormat::FormatMode::FULL) ? "FULL FORMAT" : "QUICK FORMAT";
    canvas.drawString(modeLabel, centerX, boxY + 30);

    // Warning
    canvas.setTextSize(2);
    canvas.drawString("ALL DATA LOST", centerX, boxY + 46);

    // Controls
    canvas.setTextSize(1);
    canvas.drawString("[Y] DO IT    [N] ABORT", centerX, boxY + 70);
}
