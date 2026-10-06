// DiagData Menu - System status snapshot
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class DiagDataMenu {
public:
    static void show();
    static void hide();
    static void update();
    static bool isActive() { return active; }
    static void draw(M5Canvas& canvas);

private:
    static bool active;
    static bool keyWasPressed;
    static uint16_t cachedWpaCracked;
    static uint16_t cachedWigleUploaded;
    static uint32_t lastStatRefreshMs;
    static uint32_t statRefreshIntervalMs;
    static void saveSnapshot();
    static void resetWiFi();
    static void logHeapSnapshot();
    static void collectGarbage();
    static void refreshStats();
};
