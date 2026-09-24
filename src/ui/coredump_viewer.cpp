// CoreDump Viewer Menu implementation

#include "coredump_viewer.h"
#include "display.h"
#include "../core/config.h"
#include "../core/sd_layout.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <algorithm>
#include <time.h>
#include <string.h>

static void formatDisplayName(const char* path, char* out, size_t len);
static void truncateWithEllipsis(char* text, size_t maxLen);
static void formatTimeLine(time_t t, char* out, size_t len);

static const size_t MAX_CRASH_FILES = 32;

bool CoreDumpViewer::active = false;
std::vector<CoreDumpViewer::CrashEntry> CoreDumpViewer::crashFiles;
std::vector<CoreDumpViewer::LogLine> CoreDumpViewer::fileLines;
uint16_t CoreDumpViewer::listScroll = 0;
uint16_t CoreDumpViewer::fileScroll = 0;
uint16_t CoreDumpViewer::totalLines = 0;
uint8_t CoreDumpViewer::selectedIndex = 0;
bool CoreDumpViewer::fileViewActive = false;
bool CoreDumpViewer::nukeConfirmActive = false;
bool CoreDumpViewer::keyWasPressed = false;
char CoreDumpViewer::activeFile[64] = {0};

static const uint16_t MAX_LOG_LINES = 120;
static const uint8_t VISIBLE_LINES = 9;
static const uint8_t LINE_HEIGHT = 11;

void CoreDumpViewer::init() {
    crashFiles.clear();
    fileLines.clear();
    listScroll = 0;
    fileScroll = 0;
    totalLines = 0;
    selectedIndex = 0;
    fileViewActive = false;
    nukeConfirmActive = false;
    activeFile[0] = '\0';
}

void CoreDumpViewer::scanCrashFiles() {
    crashFiles.clear();
    crashFiles.reserve(MAX_CRASH_FILES);  // Cap at 32 — avoids unbounded growth
    selectedIndex = 0;
    listScroll = 0;

    if (!Config::isSDAvailable()) {
        return;
    }

    const char* crashDir = SDLayout::crashDir();
    if (!SD.exists(crashDir)) {
        return;
    }

    File dir = SD.open(crashDir);
    if (!dir) {
        return;
    }

    uint8_t yieldCounter = 0;
    while (crashFiles.size() < MAX_CRASH_FILES) {
        File entry = dir.openNextFile();
        if (!entry) break;

        if (!entry.isDirectory()) {
            const char* name = entry.name();
            time_t lastWrite = entry.getLastWrite();
            entry.close();

            // Check .txt extension without String
            size_t nameLen = strlen(name);
            if (nameLen < 4 || strcmp(name + nameLen - 4, ".txt") != 0) {
                continue;
            }

            // Extract basename without String
            const char* base = strrchr(name, '/');
            base = base ? base + 1 : name;

            CrashEntry entryInfo;
            memset(&entryInfo, 0, sizeof(entryInfo));
            snprintf(entryInfo.path, sizeof(entryInfo.path), "%s/%s", crashDir, base);
            entryInfo.timestamp = lastWrite;
            crashFiles.push_back(entryInfo);
        } else {
            entry.close();
        }
        
        // Yield every 10 files to prevent WDT timeout
        if (++yieldCounter >= 10) {
            yieldCounter = 0;
            yield();
        }
    }

    dir.close();

    std::sort(crashFiles.begin(), crashFiles.end(), [](const CrashEntry& a, const CrashEntry& b) {
        return a.timestamp > b.timestamp;
    });
}

static void pushLogLine(std::vector<CoreDumpViewer::LogLine>& lines, const char* text) {
    CoreDumpViewer::LogLine entry;
    strncpy(entry.text, text, sizeof(entry.text) - 1);
    entry.text[sizeof(entry.text) - 1] = '\0';
    lines.push_back(entry);
}

void CoreDumpViewer::loadCrashFile(const char* path) {
    fileLines.clear();
    fileScroll = 0;
    totalLines = 0;
    strncpy(activeFile, path, sizeof(activeFile) - 1);
    activeFile[sizeof(activeFile) - 1] = '\0';

    File f = SD.open(path, FILE_READ);
    if (!f) {
        pushLogLine(fileLines, "FAILED TO OPEN");
        char displayName[32];
        formatDisplayName(path, displayName, sizeof(displayName));
        pushLogLine(fileLines, displayName);
        totalLines = fileLines.size();
        return;
    }

    // Read last MAX_LOG_LINES non-empty lines using ring buffer approach
    fileLines.reserve(MAX_LOG_LINES);
    char lineBuf[80];
    uint16_t lineCount = 0;
    while (f.available()) {
        size_t len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
        lineBuf[len] = '\0';
        while (len > 0 && (lineBuf[len - 1] == '\r' || lineBuf[len - 1] == ' ')) {
            lineBuf[--len] = '\0';
        }
        if (len == 0) continue;

        if (lineCount < MAX_LOG_LINES) {
            pushLogLine(fileLines, lineBuf);
        } else {
            // Shift left by 1 and replace last
            for (uint16_t i = 1; i < fileLines.size(); i++) {
                fileLines[i - 1] = fileLines[i];
            }
            strncpy(fileLines.back().text, lineBuf, sizeof(LogLine::text) - 1);
            fileLines.back().text[sizeof(LogLine::text) - 1] = '\0';
        }
        lineCount++;
    }
    f.close();

    totalLines = fileLines.size();

    if (fileLines.empty()) {
        pushLogLine(fileLines, "EMPTY FILE");
        totalLines = 1;
    }
}

void CoreDumpViewer::show() {
    active = true;
    keyWasPressed = true;
    fileViewActive = false;
    nukeConfirmActive = false;
    activeFile[0] = '\0';
    fileLines.clear();
    scanCrashFiles();
}

void CoreDumpViewer::hide() {
    active = false;
    crashFiles.clear();
    fileLines.clear();
    crashFiles.shrink_to_fit();
    fileLines.shrink_to_fit();
    fileViewActive = false;
    nukeConfirmActive = false;
    activeFile[0] = '\0';
    Display::clearBottomOverlay();
}

void CoreDumpViewer::nukeCrashFiles() {
    const char* crashDir = SDLayout::crashDir();
    if (!SD.exists(crashDir)) {
        return;
    }

    File dir = SD.open(crashDir);
    if (!dir) {
        return;
    }

    uint8_t yieldCounter = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;

        if (!entry.isDirectory()) {
            const char* name = entry.name();
            entry.close();

            const char* base = strrchr(name, '/');
            base = base ? (base + 1) : name;
            char path[80];
            snprintf(path, sizeof(path), "%s/%s", crashDir, base);
            size_t plen = strlen(path);

            if ((plen > 4 && strcmp(path + plen - 4, ".txt") == 0) ||
                (plen > 4 && strcmp(path + plen - 4, ".elf") == 0)) {
                SD.remove(path);
            }
        } else {
            entry.close();
        }
        
        // Yield every 10 files to prevent WDT timeout
        if (++yieldCounter >= 10) {
            yieldCounter = 0;
            yield();
        }
    }

    dir.close();
}

void CoreDumpViewer::drawList(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG, COLOR_BG);
    canvas.setTextSize(1);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(TL_DATUM);

    if (crashFiles.empty()) {
        canvas.drawString("NO CRASH FILES", 2, 8);
        canvas.drawString("CHECK CRASH DIR", 2, 20);
        return;
    }

    uint16_t count = crashFiles.size();
    uint8_t y = 2;
    const int timeX = 150;

    for (uint8_t i = 0; i < VISIBLE_LINES && (listScroll + i) < count; i++) {
        uint8_t idx = listScroll + i;
        char displayLine[32];
        formatDisplayName(crashFiles[idx].path, displayLine, sizeof(displayLine));
        size_t nameLen = strlen(displayLine);
        if (nameLen > 22 && sizeof(displayLine) > 22) {
            displayLine[21] = '~';
            displayLine[22] = '\0';
        }
        char timeLine[16];
        formatTimeLine(crashFiles[idx].timestamp, timeLine, sizeof(timeLine));

        bool selected = (idx == selectedIndex);
        if (selected) {
            canvas.fillRect(0, y - 1, DISPLAY_W, LINE_HEIGHT, COLOR_FG);
            canvas.setTextColor(COLOR_BG, COLOR_FG);
        } else {
            canvas.setTextColor(COLOR_FG, COLOR_BG);
        }

        canvas.drawString(displayLine, 2, y);
        canvas.drawString(timeLine, timeX, y);
        y += LINE_HEIGHT;
    }

    if (count > VISIBLE_LINES) {
        int barHeight = MAIN_H - 14;
        int barY = 12;
        int thumbHeight = max(10, (int)(barHeight * VISIBLE_LINES / count));
        int thumbY = barY + (barHeight - thumbHeight) * listScroll / (count - VISIBLE_LINES);

        canvas.fillRect(DISPLAY_W - 4, barY, 3, barHeight, COLOR_BG);
        canvas.fillRect(DISPLAY_W - 4, thumbY, 3, thumbHeight, COLOR_FG);
    }
}

void CoreDumpViewer::drawFile(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG, COLOR_BG);
    canvas.setTextSize(1);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(TL_DATUM);

    uint8_t y = 2;

    for (uint8_t i = 0; i < VISIBLE_LINES && (fileScroll + i) < totalLines; i++) {
        const LogLine& line = fileLines[fileScroll + i];
        char displayLine[48];
        strncpy(displayLine, line.text, sizeof(displayLine) - 1);
        displayLine[sizeof(displayLine) - 1] = '\0';
        size_t lineLen = strlen(displayLine);
        if (lineLen > 39 && sizeof(displayLine) > 39) {
            displayLine[38] = '~';
            displayLine[39] = '\0';
        }
        canvas.drawString(displayLine, 2, y);
        y += LINE_HEIGHT;
    }

    if (totalLines > VISIBLE_LINES) {
        int barHeight = MAIN_H - 14;
        int barY = 12;
        int thumbHeight = max(10, (int)(barHeight * VISIBLE_LINES / totalLines));
        int thumbY = barY + (barHeight - thumbHeight) * fileScroll / (totalLines - VISIBLE_LINES);

        canvas.fillRect(DISPLAY_W - 4, barY, 3, barHeight, COLOR_BG);
        canvas.fillRect(DISPLAY_W - 4, thumbY, 3, thumbHeight, COLOR_FG);
    }
}

void CoreDumpViewer::drawNukeConfirm(M5Canvas& canvas) {
    const int boxW = 200;
    const int boxH = 70;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;

    // Match Captures nuke style
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);

    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);

    int centerX = canvas.width() / 2;

    canvas.drawString("!! SCORCHED EARTH !!", centerX, boxY + 8);
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/*", SDLayout::crashDir());
    canvas.drawString(cmd, centerX, boxY + 22);
    canvas.drawString("THIS KILLS THE DUMPS.", centerX, boxY + 36);
    canvas.drawString("[Y] DO IT  [N] ABORT", centerX, boxY + 54);
}

void CoreDumpViewer::update() {
    if (!active) return;

    if (!M5Cardputer.Keyboard.isPressed()) {
        keyWasPressed = false;
        return;
    }

    if (keyWasPressed) return;
    keyWasPressed = true;

    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();

    if (nukeConfirmActive) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            nukeCrashFiles();
            nukeConfirmActive = false;
            Display::clearBottomOverlay();
            fileViewActive = false;
            fileLines.clear();
            scanCrashFiles();
        } else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') ||
                   M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.enter) {
            nukeConfirmActive = false;
            Display::clearBottomOverlay();
        }
        return;
    }

    if (fileViewActive) {
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (fileScroll > 0) {
                fileScroll--;
            }
        } else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if (totalLines > VISIBLE_LINES && fileScroll < totalLines - VISIBLE_LINES) {
                fileScroll++;
            }
        } else if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.enter) {
            fileViewActive = false;
            fileLines.clear();
            totalLines = 0;
            activeFile[0] = '\0';
        }
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < listScroll) {
                listScroll = selectedIndex;
            }
        }
    } else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (selectedIndex + 1 < crashFiles.size()) {
            selectedIndex++;
            if (selectedIndex >= listScroll + VISIBLE_LINES) {
                listScroll = selectedIndex - VISIBLE_LINES + 1;
            }
        }
    } else if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) {
        if (!crashFiles.empty()) {
            nukeConfirmActive = true;
            Display::setBottomOverlay("PERMANENT | NO UNDO");
        }
    } else if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
    } else if (keys.enter) {
        if (!crashFiles.empty() && selectedIndex < crashFiles.size()) {
            loadCrashFile(crashFiles[selectedIndex].path);
            fileViewActive = true;
        }
    }
}

void CoreDumpViewer::draw(M5Canvas& canvas) {
    if (!active) return;

    if (fileViewActive) {
        drawFile(canvas);
    } else {
        drawList(canvas);
    }

    if (nukeConfirmActive) {
        drawNukeConfirm(canvas);
    }
}

static void formatDisplayName(const char* path, char* out, size_t len) {
    if (!out || len == 0) return;
    if (!path || path[0] == '\0') {
        out[0] = '\0';
        return;
    }
    const char* name = strrchr(path, '/');
    name = name ? (name + 1) : path;
    size_t nlen = strlen(name);
    if (nlen >= len) nlen = len - 1;
    memcpy(out, name, nlen);
    out[nlen] = '\0';

    size_t outLen = strlen(out);
    if (outLen >= 4 && strcmp(out + outLen - 4, ".txt") == 0) {
        out[outLen - 4] = '\0';
    }
}

static void truncateWithEllipsis(char* text, size_t maxLen) {
    if (!text) return;
    size_t len = strlen(text);
    if (len <= maxLen) return;
    if (maxLen < 3) {
        text[maxLen] = '\0';
        return;
    }
    size_t cut = maxLen - 3;
    text[cut] = '.';
    text[cut + 1] = '.';
    text[cut + 2] = '.';
    text[cut + 3] = '\0';
}

static void formatTimeLine(time_t t, char* out, size_t len) {
    if (!out || len == 0) return;
    if (t == 0) {
        strncpy(out, "-- -- --:--", len - 1);
        out[len - 1] = '\0';
        return;
    }

    struct tm* timeinfo = localtime(&t);
    if (!timeinfo) {
        strncpy(out, "-- -- --:--", len - 1);
        out[len - 1] = '\0';
        return;
    }

    // Format: "Dec 06 14:32"
    strftime(out, len, "%b %d %H:%M", timeinfo);
}

void CoreDumpViewer::getStatusLine(char* out, size_t len) {
    if (!out || len == 0) return;
    out[0] = '\0';
    if (!active) return;

    const char* path = nullptr;
    if (fileViewActive && activeFile[0] != '\0') {
        path = activeFile;
    } else if (crashFiles.empty()) {
        snprintf(out, len, "NO CRASH FILES");
        return;
    } else if (selectedIndex < crashFiles.size()) {
        path = crashFiles[selectedIndex].path;
    } else {
        snprintf(out, len, "CRASH FILES");
        return;
    }

    formatDisplayName(path, out, len);
    truncateWithEllipsis(out, (len - 1 < 24) ? (len - 1) : 24);
}
