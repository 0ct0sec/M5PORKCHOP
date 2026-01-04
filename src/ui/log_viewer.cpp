// Log Viewer Menu implementation

#include "log_viewer.h"
#include "display.h"
#include "../core/config.h"
#include <M5Cardputer.h>
#include <SD.h>

// Static members
bool LogViewer::active = false;
std::vector<String> LogViewer::logLines;
uint16_t LogViewer::scrollOffset = 0;
uint16_t LogViewer::totalLines = 0;
bool LogViewer::keyWasPressed = false;

static const uint16_t MAX_LOG_LINES = 100;
static const uint8_t VISIBLE_LINES = 9;  // Lines visible on screen (no header now)

void LogViewer::init() {
    logLines.clear();
    scrollOffset = 0;
    totalLines = 0;
}

String LogViewer::findLatestLogFile() {
    if (!Config::isSDAvailable()) {
        Serial.println("[LOGVIEW] SD not available");
        return "";
    }
    
    // Use fixed filename - always the same file
    const char* logFile = "/M5PORKCHOP/logs/porkchop.log";
    
    if (SD.exists(logFile)) {
        Serial.printf("[LOGVIEW] Found: %s\n", logFile);
        return String(logFile);
    }
    
    Serial.println("[LOGVIEW] Log file not found");
    return "";
}

void LogViewer::loadLogFile() {
    logLines.clear();
    
    String filename = findLatestLogFile();
    if (filename.length() == 0) {
        logLines.push_back("No log files found");
        logLines.push_back("Enable SD Log in Settings");
        totalLines = logLines.size();
        Serial.printf("[LOGVIEW] No log files, totalLines=%d\n", totalLines);
        return;
    }
    
    Serial.printf("[LOGVIEW] Opening: %s\n", filename.c_str());
    File f = SD.open(filename.c_str(), FILE_READ);
    if (!f) {
        logLines.push_back("Failed to open log file");
        logLines.push_back(filename);
        totalLines = logLines.size();
        Serial.printf("[LOGVIEW] Failed to open, totalLines=%d\n", totalLines);
        return;
    }
    
    Serial.printf("[LOGVIEW] File size: %d bytes\n", f.size());
    
    // Read all lines into a circular buffer (keep last MAX_LOG_LINES)
    std::vector<String> allLines;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            allLines.push_back(line);
            // Keep only last MAX_LOG_LINES
            if (allLines.size() > MAX_LOG_LINES) {
                allLines.erase(allLines.begin());
            }
        }
    }
    f.close();
    
    Serial.printf("[LOGVIEW] Read %d lines\n", allLines.size());
    
    logLines = allLines;
    totalLines = logLines.size();
    
    // Start scrolled to bottom (most recent)
    if (totalLines > VISIBLE_LINES) {
        scrollOffset = totalLines - VISIBLE_LINES;
    } else {
        scrollOffset = 0;
    }
    
    if (logLines.empty()) {
        logLines.push_back("Log file is empty");
        totalLines = 1;
    }
}

void LogViewer::show() {
    active = true;
    keyWasPressed = true;  // Ignore the key that opened us
    loadLogFile();
    render();
}

void LogViewer::hide() {
    active = false;
    logLines.clear();
    logLines.shrink_to_fit();  // Release vector memory
}

void LogViewer::render() {
    M5Canvas& canvas = Display::getMain();
    
    // Clear and setup canvas
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG, COLOR_BG);
    canvas.setTextSize(1);
    canvas.setFont(&fonts::Font0);
    
    // Draw log lines (title is in top bar now)
    canvas.setTextDatum(TL_DATUM);
    uint8_t y = 2;
    uint8_t lineHeight = 11;
    
    Serial.printf("[LOGVIEW] Rendering %d lines from offset %d\n", totalLines, scrollOffset);
    
    for (uint8_t i = 0; i < VISIBLE_LINES && (scrollOffset + i) < totalLines; i++) {
        String& line = logLines[scrollOffset + i];
        
        // Truncate long lines to fit screen
        String displayLine = line;
        if (displayLine.length() > 39) {
            displayLine = displayLine.substring(0, 38) + "~";
        }
        
        canvas.drawString(displayLine, 2, y);
        y += lineHeight;
    }
    
    // Scroll indicator
    if (totalLines > VISIBLE_LINES) {
        int barHeight = MAIN_H - 14;
        int barY = 12;
        int thumbHeight = max(10, (int)(barHeight * VISIBLE_LINES / totalLines));
        int thumbY = barY + (barHeight - thumbHeight) * scrollOffset / (totalLines - VISIBLE_LINES);
        
        canvas.fillRect(DISPLAY_W - 4, barY, 3, barHeight, 0x2104);  // Dark gray track
        canvas.fillRect(DISPLAY_W - 4, thumbY, 3, thumbHeight, COLOR_FG);  // Pink thumb
    }
    
    // Instructions in bottom bar
    M5Canvas& bottom = Display::getBottomBar();
    bottom.fillSprite(COLOR_BG);
    bottom.setTextSize(1);
    bottom.setTextColor(COLOR_FG);
    bottom.setTextDatum(TL_DATUM);
    char info[24];
    snprintf(info, sizeof(info), "L:%d", totalLines);
    bottom.drawString(info, 2, 3);
    bottom.setTextDatum(TR_DATUM);
    bottom.drawString(";/. `/Ent", DISPLAY_W - 2, 3);
    
    Display::pushAll();
}

void LogViewer::update() {
    if (!active) return;
    
    // Note: M5Cardputer.update() is called in main loop, don't call it again
    
    if (!M5Cardputer.Keyboard.isPressed()) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;  // Debounce
    
    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    
    bool needsRender = false;
    
    for (auto key : keys.word) {
        keyWasPressed = true;
        
        if (key == ';') {
            // Scroll up
            if (scrollOffset > 0) {
                scrollOffset--;
                needsRender = true;
            }
        } else if (key == '.') {
            // Scroll down - only if we have more lines than visible
            if (totalLines > VISIBLE_LINES && scrollOffset < totalLines - VISIBLE_LINES) {
                scrollOffset++;
                needsRender = true;
            }
        } else if (key == '`' || key == 0x1B) {
            // Exit
            hide();
            return;
        }
    }
    
    // Also check for Enter to exit
    if (keys.enter) {
        keyWasPressed = true;
        hide();
        return;
    }
    
    if (needsRender) {
        render();
    }
}
