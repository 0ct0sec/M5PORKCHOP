// m5porkchop
// Main entry point
// by 0ct0

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <SD.h>
#include <WiFi.h>              // <-- PATCH: init WiFi early (before heap fragmentation)
#include <esp_heap_caps.h>     // For heap conditioning
#include <string.h>            // For memset
#include "core/porkchop.h"
#include "core/config.h"
#include "core/xp.h"
#include "core/sdlog.h"
#include "core/wifi_utils.h"
#include "core/heap_policy.h"
#include "core/heap_health.h"
#include "core/network_recon.h"
#include "ui/display.h"
#include "gps/gps.h"
#include "piglet/avatar.h"
#include "piglet/mood.h"
#include "modes/oink.h"
#include "modes/warhog.h"
#include "core/janus_hog.h"
#include "audio/sfx.h"

Porkchop porkchop;

// --- PATCH: Pre-init WiFi driver early to avoid later esp_wifi_init() failures
// Some reconnect flows (and some Arduino/M5 stacks) end up deinit/reinit WiFi later.
// If heap is fragmented by display sprites / big allocations, esp_wifi_init() may fail with:
//   "Expected to init 4 rx buffer, actual is X" and "wifiLowLevelInit(): esp_wifi_init 257"
static void preInitWiFiDriverEarly() {
    WiFi.persistent(false);

    // Force driver/buffers allocation while heap is still clean/contiguous
    WiFi.mode(WIFI_STA);

    // Stop radio but keep driver initialized (buffers stay allocated).
    // Signature: disconnect(bool wifioff, bool eraseap)
    WiFi.disconnect(true /* wifioff */, false /* eraseap */);

    // No modem sleep to reduce odd timing/latency during TLS + UI load
    WiFi.setSleep(false);

    delay(HeapPolicy::kWiFiModeDelayMs);
}

// Reservation Fence: Force WiFi driver allocations to the TOP of heap,
// leaving a large contiguous region below for application use.
//
// Why this works: TLSF's good-fit strategy allocates from the lowest
// available block. By occupying the bottom 80KB with a fence, the WiFi
// driver's ~35KB of permanent DMA/RX buffers land above the fence.
// When we free the fence, the bottom 80KB is contiguous free space.
//
// This replaces the old 5-phase alloc/free conditioning dance with a
// deterministic, 3-line pattern that's both simpler and more effective.
static void setupHeapLayout() {
    size_t beforeFree = ESP.getFreeHeap();
    size_t beforeLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[BOOT] Pre-fence heap: free=%u largest=%u\n",
                  (unsigned)beforeFree, (unsigned)beforeLargest);

    // Allocate fence to push WiFi driver allocations high in the heap
    static constexpr size_t kFenceSize = 80000;
    void* fence = heap_caps_malloc(kFenceSize, MALLOC_CAP_8BIT);
    if (fence) {
        Serial.printf("[BOOT] Fence allocated: %u bytes at %p\n",
                      (unsigned)kFenceSize, fence);
    } else {
        Serial.println("[BOOT] WARNING: Fence allocation failed, falling back to direct init");
    }

    // WiFi driver allocates its permanent DMA/RX buffers ABOVE the fence
    preInitWiFiDriverEarly();

    // Release the fence — leaves large contiguous space below WiFi driver
    if (fence) {
        heap_caps_free(fence);
    }

    size_t afterFree = ESP.getFreeHeap();
    size_t afterLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[BOOT] Post-fence heap: free=%u largest=%u\n",
                  (unsigned)afterFree, (unsigned)afterLargest);
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n=== PORKCHOP STARTING ===");

    // Deassert CapLoRa SX1262 CS BEFORE SD init. The SX1262 shares
    // MOSI(G14)/MISO(G39)/SCK(G40) with the SD card. If its CS floats low
    // the SX1262 responds on the bus and SD.begin() fails with f_mount(3).
    // MUST happen before M5Cardputer.begin() — GPIO5 is a keyboard matrix
    // input on v1.1 and begin() needs to reconfigure it as INPUT_PULLUP.
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);

    // Init M5Cardputer hardware
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);   // enableKeyboard = true

    // Configure G0 button (GPIO0) as input with pullup
    pinMode(0, INPUT_PULLUP);

    // Reservation fence: push WiFi driver allocations high in heap, then free
    // the fence to leave large contiguous space at the bottom.
    // Replaces the old 5-phase boot conditioning with a deterministic layout.
    setupHeapLayout();
    yield();

    // Load configuration from SD
    if (!Config::init()) {
        Serial.println("[MAIN] Config init failed, using defaults");
    }
    Serial.println("[BOOT] Config done");
    yield();

    // Init SD logging (will be enabled via settings if user wants)
    SDLog::init();

    // Load previous session watermarks before resetting peaks
    HeapHealth::loadPreviousSession();

    // TLS reserve disabled: browser handles TLS, keep heap for UI/file transfer.

    // Init display system
    Display::init();
    Serial.println("[BOOT] Display done");
    yield();

    // Init audio early so boot sound plays
    SFX::init();

    // Show boot splash (3 screens: OINK OINK, MY NAME IS, PORKCHOP)
    Display::showBootSplash();

    // Apply saved brightness
    M5.Display.setBrightness(Config::personality().brightness * 255 / 100);

    // Initialize piglet personality
    Avatar::init();
    Mood::init();
    yield();

    // Initialize GPS (if enabled)
    if (Config::gps().enabled) {
        // Hardware detection: warn if Cap LoRa GPS selected on non-ADV hardware
        if (Config::gps().source == GPSSource::CAP_LORA) {
            auto board = M5.getBoard();
            if (board != m5::board_t::board_M5CardputerADV) {
                Serial.println("[GPS] WARNING: Cap LoRa868 GPS selected but hardware is not Cardputer ADV!");
                Serial.println("[GPS] Cap LoRa868 requires Cardputer ADV EXT bus. Check config.");
            }
            // Quiesce SX1262 and clear G13 FSPIQ IOMUX before GPS UART init.
            // CapLoRa shares MOSI/MISO/SCK with SD; G13 is default FSPIQ pin.
            Config::prepareCapLoraGpio();
        }
        GPS::init(Config::gps().rxPin, Config::gps().txPin, Config::gps().baudRate);

        // Re-verify SD after CapLoRa GPS UART init (UART on G13 may disturb FSPI bus)
        if (Config::gps().source == GPSSource::CAP_LORA) {
            Serial.println("[GPS] Re-verifying SD card after CapLoRa GPS UART init...");
            if (!Config::reinitSD()) {
                Serial.println("[GPS] WARNING: SD card re-init failed after CapLoRa GPS init");
            }
        }
    }
    Serial.println("[BOOT] GPS done");
    yield();

    // Initialize JanusHog coprocessor (JANUS HOG) — before modes, after GPS
    JanusHog::init();
    Serial.println("[BOOT] C5 done");
    yield();

    // Initialize modes
    OinkMode::init();
    WarhogMode::init();
    porkchop.init();
    yield();

    Serial.println("=== PORKCHOP READY ===");
    Serial.printf("Piglet: %s\n", Config::personality().name);
    
    // #region agent log
    // [DEBUG] H1: Log heap after init to check static pool impact (~13KB expected reduction)
    Serial.printf("[DBG-HEAP] After init: free=%u largest=%u\n", 
                  (unsigned)ESP.getFreeHeap(), 
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // #endregion
    
    // Start background network reconnaissance service
    // This stabilizes heap by running WiFi promiscuous mode early
    // and provides shared network data for OINK/DONOHAM/SPECTRUM modes
    NetworkRecon::start();

    // Reset heap health baseline to post-init state so the health bar
    // starts at the REAL value, not 100%. Without this, the EMA slowly
    // converges from 100% to reality, looking like a steady decline.
    HeapHealth::resetPeaks(true);
}

void loop() {
    M5Cardputer.update();
    
    // #region agent log
    // [DEBUG] H1/H3: Periodic heap monitoring (every 5 seconds)
    static uint32_t lastHeapLog = 0;
    if (millis() - lastHeapLog > 5000) {
        lastHeapLog = millis();
        Serial.printf("[DBG-HEAP-LOOP] free=%u largest=%u minFree=%u\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (unsigned)ESP.getMinFreeHeap());
    }
    // #endregion

    // Persist session watermarks to SD (rate-limited to 60s internally)
    HeapHealth::persistWatermarks();

    {
        static bool bakedActive = false;
        static uint32_t bakedStartMs = 0;
        static uint32_t bakedDurationMs = 0;
        static bool bakedTriggered = false;
        static uint32_t lastBakedCheck = 0;

        if (bakedActive) {
            if (millis() - bakedStartMs >= bakedDurationMs) {
                bakedActive = false;
            } else {
                yield();
                return;
            }
        }

        if (!bakedTriggered && XP::hasUnlockable(3) && millis() - lastBakedCheck > 1000) {
            lastBakedCheck = millis();
            time_t now = time(nullptr);
            if (now > 1600000000) {
                int8_t tzOffset = Config::gps().timezoneOffset;
                now += (int32_t)tzOffset * 3600;
                struct tm timeinfo;
                gmtime_r(&now, &timeinfo);
                if ((timeinfo.tm_hour == 4 || timeinfo.tm_hour == 16) && timeinfo.tm_min == 20) {
                    bakedActive = true;
                    bakedStartMs = millis();
                    bakedDurationMs = random(120000, 420001);
                    bakedTriggered = true;
                }
            }
        }
    }

    // Update GPS
    if (Config::gps().enabled) {
        GPS::update();
    }

    // Update JanusHog coprocessor (non-blocking UART drain + state machine)
    JanusHog::update();

    // Update mood system
    Mood::update();

    // Update main controller (handles modes, input, state)
    porkchop.update();

    // Update display
    Display::update();

    // Slower update rate for smoother animation
}
