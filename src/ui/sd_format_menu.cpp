// SD Format Menu - destructive SD card formatting UI
// Aligned with project UI patterns (settings_menu, menu.cpp)

#include "sd_format_menu.h"
#include "display.h"
#include "../core/config.h"
#include <M5Cardputer.h>

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
SdFormatMenu::State SdFormatMenu::state = SdFormatMenu::State::IDLE;
SDFormat::Result SdFormatMenu::lastResult = {};
bool SdFormatMenu::hasResult = false;
SDFormat::FormatMode SdFormatMenu::formatMode = SDFormat::FormatMode::QUICK;
uint8_t SdFormatMenu::progressPercent = 0;
char SdFormatMenu::progressStage[16] = "";
uint8_t SdFormatMenu::hintIndex = 0;

// ============================================================================
// PUBLIC API
// ============================================================================

void SdFormatMenu::show() {
    active = true;
    keyWasPressed = true;  // Ignore the Enter that brought us here
    state = State::IDLE;
    hasResult = false;
    formatMode = SDFormat::FormatMode::QUICK;
    progressPercent = 0;
    progressStage[0] = '\0';
    hintIndex = esp_random() % HINT_COUNT;
    Display::clearBottomOverlay();
}

void SdFormatMenu::hide() {
    active = false;
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
        case State::IDLE:
            return "ENTER TO FORMAT SD CARD";
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

    auto keys = M5Cardputer.Keyboard.keysState();
    bool up = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');
    bool back = M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE);

    // ---- CONFIRM STATE ----
    if (state == State::CONFIRM) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            Display::clearBottomOverlay();
            state = State::WORKING;
        } else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') || back) {
            Display::clearBottomOverlay();
            state = State::SELECT;
        }
        return;
    }

    // ---- RESULT STATE ----
    if (state == State::RESULT) {
        if (keys.enter || back) {
            hide();
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
        if (keys.enter) {
            Display::clearBottomOverlay();
            state = State::CONFIRM;
            Display::setBottomOverlay("[Y] CONFIRM  [N] CANCEL");
            return;
        }
        if (back) {
            Display::clearBottomOverlay();
            state = State::IDLE;
            return;
        }
        return;
    }

    // ---- IDLE STATE ----
    if (keys.enter) {
        if (!Config::isSDAvailable()) {
            Display::notify(NoticeKind::WARNING, "SD NOT MOUNTED");
            return;
        }
        state = State::SELECT;
        return;
    }

    if (back) {
        hide();
    }
}

void SdFormatMenu::startFormat() {
    lastResult = SDFormat::formatCard(formatMode, true, onFormatProgress);
    hasResult = true;
    state = State::RESULT;
}

void SdFormatMenu::onFormatProgress(const char* stage, uint8_t percent) {
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
// DRAWING
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
    canvas.drawString("SD FORMAT SD", DISPLAY_W / 2, 2);
    canvas.drawLine(10, 20, DISPLAY_W - 10, 20, fg);

    if (state == State::WORKING) {
        drawWorking(canvas);
        return;
    }

    if (state == State::RESULT && hasResult) {
        drawResult(canvas);
    } else if (state == State::SELECT) {
        drawSelect(canvas);
    } else {
        drawIdle(canvas);
    }

    if (state == State::CONFIRM) {
        drawConfirm(canvas);
    }
}

void SdFormatMenu::drawIdle(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);

    int y = 26;
    const int lineH = 18;

    // SD Status row (highlighted style)
    canvas.drawString("SD:", 8, y);
    const char* sdStatus = Config::isSDAvailable() ? "MOUNTED" : "NOT FOUND";
    canvas.setTextDatum(top_right);
    canvas.drawString(sdStatus, DISPLAY_W - 8, y);
    canvas.setTextDatum(top_left);
    y += lineH;

    // Separator
    canvas.drawLine(20, y + 2, DISPLAY_W - 20, y + 2, fg);
    y += 10;

    // Info text
    canvas.setTextSize(1);
    canvas.drawString("ERASES ALL DATA ON SD", 8, y);
    y += 12;
    canvas.drawString("FAT32 QUICK OR FULL", 8, y);
    y += 16;

    // Controls hint
    canvas.setTextSize(2);
    canvas.setTextDatum(top_center);
    canvas.drawString("ENTER=START", DISPLAY_W / 2, y);
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
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    
    const char* modeHint = fullSelected
        ? "ZERO-FILLS CARD BEFORE FORMAT"
        : "FORMAT ONLY (PRESERVES WEAR)";
    canvas.drawString(modeHint, DISPLAY_W / 2, y);
    y += 14;

    canvas.setTextSize(2);
    canvas.drawString(";/. NAV  ENTER=OK", DISPLAY_W / 2, y);
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
    canvas.setTextSize(2);
    canvas.drawString("ENTER TO EXIT", DISPLAY_W / 2, y);
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
