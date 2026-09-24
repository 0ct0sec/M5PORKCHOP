// CoreDump Viewer Menu
#pragma once

#include <Arduino.h>
#include <vector>
#include <M5Unified.h>

class CoreDumpViewer {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static bool isActive() { return active; }
    static void draw(M5Canvas& canvas);
    static void getStatusLine(char* out, size_t len);

    struct LogLine { char text[80]; };

private:
    struct CrashEntry {
        char path[64];
        time_t timestamp = 0;
    };
    static bool active;
    static std::vector<CrashEntry> crashFiles;
    static std::vector<LogLine> fileLines;
    static uint16_t listScroll;
    static uint16_t fileScroll;
    static uint16_t totalLines;
    static uint8_t selectedIndex;
    static bool fileViewActive;
    static bool nukeConfirmActive;
    static bool keyWasPressed;
    static char activeFile[64];

    static void scanCrashFiles();
    static void loadCrashFile(const char* path);
    static void drawList(M5Canvas& canvas);
    static void drawFile(M5Canvas& canvas);
    static void drawNukeConfirm(M5Canvas& canvas);
    static void nukeCrashFiles();
};
