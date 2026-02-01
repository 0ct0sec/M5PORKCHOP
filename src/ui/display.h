// Display management for M5Cardputer
#pragma once

#include <M5Unified.h>

// Forward declarations
enum class PorkchopMode : uint8_t;

// Display layout constants (240x135 screen)
#define DISPLAY_W 240
#define DISPLAY_H 135
#define TOP_BAR_H 14
#define BOTTOM_BAR_H 14
#define MAIN_H (DISPLAY_H - TOP_BAR_H - BOTTOM_BAR_H)

// Theme structure
struct PorkTheme {
    const char* name;
    uint16_t fg;
    uint16_t bg;
};

enum class NoticeKind : uint8_t {
    REWARD,
    STATUS,
    WARNING,
    ERROR
};

enum class NoticeChannel : uint8_t {
    AUTO,
    TOAST,
    TOP_BAR
};

// Theme count and extern declaration (actual array in display.cpp)
static const uint8_t THEME_COUNT = 15;
extern const PorkTheme THEMES[THEME_COUNT];

// Dynamic color getters (use these instead of macros)
uint16_t getColorFG();
uint16_t getColorBG();

// Compatibility macros - redirect to getters
#define COLOR_BG getColorBG()
#define COLOR_FG getColorFG()
#define COLOR_ACCENT COLOR_FG
#define COLOR_WARNING COLOR_FG
#define COLOR_DANGER COLOR_FG
#define COLOR_SUCCESS COLOR_FG

class Display {
public:
    static void init();
    static void update();
    static void clear();

    // Upload progress tracking
    static bool uploadInProgress;
    static uint8_t uploadProgress;
    static char uploadStatus[64];
    static uint32_t uploadStartTime;
    static void setUploadProgress(bool inProgress, uint8_t progress, const char* status);
    static void clearUploadProgress();
    static bool shouldShowUploadProgress();
    static void drawUploadProgress(M5Canvas& topBar);
    static void drawUploadProgressDirect();

    // Top bar status messaging (single-line)
    static void setTopBarMessage(const String& message, uint32_t durationMs = 0);
    static void setTopBarMessage(const char* message, uint32_t durationMs = 0);
    static void clearTopBarMessage();
    static void requestTopBarMessage(const char* message, uint32_t durationMs = 0);

    // Canvas access for direct drawing
    static M5Canvas& getTopBar() { return topBar; }
    static M5Canvas& getMain() { return mainCanvas; }
    static M5Canvas& getBottomBar() { return bottomBar; }
    
    // Helper functions
    static void pushAll();
    static void showBootSplash();  // 3-screen boot animation
    static void showInfoBox(const String& title, const String& line1, 
                           const String& line2 = "", bool blocking = true);
    static bool showConfirmBox(const String& title, const String& message);
    static void showProgress(const String& title, uint8_t percent);
    static void showProgress(const char* title, uint8_t percent);
    static void showToast(const String& message, uint32_t durationMs = 2000);  // Quick non-blocking message
    static void showToast(const char* message, uint32_t durationMs = 2000);    // Literal-friendly overload
    static void notify(NoticeKind kind, const String& message,
                       uint32_t durationMs = 0,
                       NoticeChannel channel = NoticeChannel::AUTO);
    static void showLevelUp(uint8_t oldLevel, uint8_t newLevel);  // RPG level up popup

    // Mode-specific UI functions
    static void drawPigSyncDeviceSelect(M5Canvas& canvas);  // PigSync device selection UI
    static void showClassPromotion(const char* oldClass, const char* newClass);  // Class tier promotion popup
    static void showChallenges();  // Session challenges overlay (press '1')
    
    // LED effects (NeoPixel on GPIO 21)
    static void flashSiren(uint8_t cycles = 3);  // Red/blue alternating flash
    static void setLED(uint8_t r, uint8_t g, uint8_t b);  // Static LED glow
    
    // PWNED banner (shown in top bar for 1 minute after capture)
    static void showLoot(const String& ssid);
    
    // Bottom bar overlay (for confirmation dialogs)
    static void setBottomOverlay(const String& message);  // Set custom bottom bar text
    static void clearBottomOverlay();                     // Clear overlay, restore normal
    
    // Status indicators
    static void setGPSStatus(bool hasFix);
    static void setWiFiStatus(bool connected);
    static void setMLStatus(bool active);
    
    // Screen dimming
    static void resetDimTimer();      // Call on any user input
    static void updateDimming();      // Call in update loop
    static bool isDimmed() { return dimmed; }
    static void toggleScreenPower(); // Toggle screen on/off

    // Screenshot
    static bool takeScreenshot();     // Save screen to SD card, returns success
    static bool isSnapping() { return snapping; }  // True during screenshot save
    
private:
    static M5Canvas topBar;
    static M5Canvas mainCanvas;
    static M5Canvas bottomBar;
    
    static bool gpsStatus;
    static bool wifiStatus;
    static bool mlStatus;
    
    // Dimming state
    static uint32_t lastActivityTime;
    static bool dimmed;
    static bool screenForcedOff;
    
    // Screenshot state
    static bool snapping;

    // Toast state
    static char toastMessage[160];
    static uint32_t toastStartTime;
    static uint32_t toastDurationMs;
    static bool toastActive;
    static char topBarMessage[96];
    static uint32_t topBarMessageStart;
    static uint32_t topBarMessageDuration;
    static bool topBarMessageTwoLineActive;

    // Bottom bar overlay
    static char bottomOverlay[96];
    static volatile bool pendingTopBarMessage;
    static char pendingTopBarMessageBuf[96];
    static uint32_t pendingTopBarDurationMs;
    
    static void drawTopBar();
    static void drawBottomBar();
    static void drawTopBarMessageTwoLineDirect();
    static void drawModeInfo(M5Canvas& canvas, PorkchopMode mode);
    static void drawSettingsScreen(M5Canvas& canvas);
    static void drawAboutScreen(M5Canvas& canvas);
    static void drawFileTransferScreen(M5Canvas& canvas);
    
public:
    // About screen easter egg handlers (called from porkchop.cpp)
    static void onAboutEnterPressed();
    static void resetAboutState();
};
