#pragma once

#include <Arduino.h>
#include <M5GFX.h>

class PigChatMode {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isRunning() { return running; }

private:
    static bool running;
};
