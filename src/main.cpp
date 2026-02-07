// m5porkchop
// Main entry point
// by 0ct0

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <SD.h>
#include <WiFi.h>              // <-- PATCH: init WiFi early (before heap fragmentation)
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>     // For heap conditioning
#include <string.h>            // For memset
#include "core/porkchop.h"
#include "core/config.h"
#include "core/xp.h"
#include "core/sd_layout.h"
#include "core/sdlog.h"
#include "core/wifi_utils.h"
#include "core/heap_policy.h"
#include "core/network_recon.h"
#include "ui/display.h"
#include "gps/gps.h"
#include "piglet/avatar.h"
#include "piglet/mood.h"
#include "modes/oink.h"
#include "modes/warhog.h"
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

static void exportCoreDumpToSD() {
    if (!Config::isSDAvailable()) {
        Serial.println("[COREDUMP] SD not available, skipping export");
        return;
    }

    esp_err_t check = esp_core_dump_image_check();
    if (check != ESP_OK) {
        return;  // No core dump to export
    }

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        nullptr
    );
    if (!part) {
        Serial.println("[COREDUMP] No coredump partition found");
        return;
    }

    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        Serial.println("[COREDUMP] No coredump image available");
        return;
    }

    size_t partOffset = 0;
    if (addr >= part->address) {
        partOffset = addr - part->address;
    }
    if (partOffset + size > part->size) {
        Serial.println("[COREDUMP] Image bounds exceed partition size");
        return;
    }

    const char* crashDir = SDLayout::crashDir();
    if (!SD.exists(crashDir)) {
        SD.mkdir(crashDir);
    }

    uint32_t stamp = millis();
    char dumpPath[64];
    snprintf(dumpPath, sizeof(dumpPath), "%s/coredump_%lu.elf",
             crashDir, static_cast<unsigned long>(stamp));

    File out = SD.open(dumpPath, FILE_WRITE);
    if (!out) {
        Serial.println("[COREDUMP] Failed to open output file");
        return;
    }

    const size_t kChunkSize = 512;
    uint8_t buf[kChunkSize];
    size_t remaining = size;
    size_t offset = 0;
    uint8_t yieldCounter = 0;

    while (remaining > 0) {
        size_t toRead = remaining > kChunkSize ? kChunkSize : remaining;
        if (esp_partition_read(part, partOffset + offset, buf, toRead) != ESP_OK) {
            Serial.println("[COREDUMP] Read failed");
            break;
        }
        out.write(buf, toRead);
        remaining -= toRead;
        offset += toRead;
        
        // Yield every 8 chunks (4KB) to prevent WDT timeout
        if (++yieldCounter >= 8) {
            yieldCounter = 0;
            yield();
        }
    }
    out.close();

    if (remaining == 0) {
        Serial.printf("[COREDUMP] Exported %u bytes to %s\n",
                      static_cast<unsigned int>(size), dumpPath);
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
        esp_core_dump_summary_t summary;
        if (esp_core_dump_get_summary(&summary) == ESP_OK) {
            char sumPath[64];
                snprintf(sumPath, sizeof(sumPath), "%s/coredump_%lu.txt",
                     crashDir, static_cast<unsigned long>(stamp));
            File sum = SD.open(sumPath, FILE_WRITE);
            if (sum) {
                sum.printf("task=%s\n", summary.exc_task);
                sum.printf("pc=0x%08lx\n", static_cast<unsigned long>(summary.exc_pc));
                sum.printf("bt_depth=%u\n", summary.exc_bt_info.depth);
                sum.printf("bt_corrupted=%u\n", summary.exc_bt_info.corrupted ? 1 : 0);
                for (uint32_t i = 0; i < summary.exc_bt_info.depth; i++) {
                    sum.printf("bt%lu=0x%08lx\n",
                               static_cast<unsigned long>(i),
                               static_cast<unsigned long>(summary.exc_bt_info.bt[i]));
                }
                sum.printf("exc_cause=0x%08lx\n", static_cast<unsigned long>(summary.ex_info.exc_cause));
                sum.printf("exc_vaddr=0x%08lx\n", static_cast<unsigned long>(summary.ex_info.exc_vaddr));
                sum.printf("elf_sha256=%s\n", summary.app_elf_sha256);
                sum.close();
            }
        }
#endif
        esp_core_dump_image_erase();
    } else {
        Serial.println("[COREDUMP] Export incomplete, keeping core dump in flash");
    }
}

// Perform gentle heap conditioning to create larger contiguous blocks for TLS
// This addresses the issue where TLS operations fail due to fragmented heap after boot
void performBootHeapConditioning() {
    Serial.println("[BOOT] Performing aggressive heap conditioning...");

    // Log initial heap state
    size_t initialLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t initialFree = ESP.getFreeHeap();
    Serial.printf("[BOOT] Initial heap: free=%u largest=%u\n", initialFree, initialLargest);

    // Phase 1: Create fragmentation pattern (mimics Oink mode startup)
    const int FRAG_BLOCKS = HeapPolicy::kBootFragBlocks;
    const size_t FRAG_SIZE = HeapPolicy::kBootFragBlockSize; // 1KB blocks
    void* fragBlocks[FRAG_BLOCKS] = {nullptr};
    size_t fragAllocated = 0;

    Serial.println("[BOOT] Phase 1: Creating fragmentation...");
    for (int i = 0; i < FRAG_BLOCKS; i++) {
        fragBlocks[i] = malloc(FRAG_SIZE);
        if (fragBlocks[i]) {
            memset(fragBlocks[i], 0xAA, FRAG_SIZE);
            fragAllocated++;
        }
        if (i % HeapPolicy::kBootFragYieldEvery == 0) {
            delay(HeapPolicy::kBootFragYieldDelayMs); // Periodic yield
        }
    }
    Serial.printf("[BOOT] Fragmentation: %u/%u blocks (%uKB)\n", fragAllocated, FRAG_BLOCKS, (fragAllocated * FRAG_SIZE) / 1024);

    // Phase 2: Add larger blocks (like network structures in Oink)
    const int STRUCT_BLOCKS = HeapPolicy::kBootStructBlocks;
    const size_t STRUCT_SIZE = HeapPolicy::kBootStructBlockSize; // ~3KB (like DetectedNetwork + clients)
    void* structBlocks[STRUCT_BLOCKS] = {nullptr};
    size_t structAllocated = 0;

    Serial.println("[BOOT] Phase 2: Adding structure blocks...");
    for (int i = 0; i < STRUCT_BLOCKS; i++) {
        structBlocks[i] = malloc(STRUCT_SIZE);
        if (structBlocks[i]) {
            memset(structBlocks[i], 0xBB, STRUCT_SIZE);
            structAllocated++;
        }
        delay(HeapPolicy::kBootStructAllocDelayMs);
    }
    Serial.printf("[BOOT] Structures: %u/%u blocks (%uKB)\n", structAllocated, STRUCT_BLOCKS, (structAllocated * STRUCT_SIZE) / 1024);

    // Phase 3: Free in consolidation-friendly pattern
    Serial.println("[BOOT] Phase 3: Consolidating memory...");

    // Free structure blocks first (creates large holes)
    for (int i = 0; i < STRUCT_BLOCKS; i++) {
        if (structBlocks[i]) {
            free(structBlocks[i]);
            structBlocks[i] = nullptr;
        }
    }

    // Free fragmentation blocks in mixed order
    for (int i = FRAG_BLOCKS - 1; i >= 0; i -= 2) { // Free every other block backwards
        if (fragBlocks[i]) {
            free(fragBlocks[i]);
            fragBlocks[i] = nullptr;
        }
        delay(HeapPolicy::kBootFreeDelayMs);
    }

    // Free remaining fragmentation blocks
    for (int i = 0; i < FRAG_BLOCKS; i++) {
        if (fragBlocks[i]) {
            free(fragBlocks[i]);
            fragBlocks[i] = nullptr;
        }
    }

    // Phase 4: Test TLS-sized allocations
    Serial.println("[BOOT] Phase 4: Testing TLS compatibility...");
    const size_t* TLS_SIZES = HeapPolicy::kBootTlsTestSizes; // 26KB, 32KB, 40KB
    const char* TLS_NAMES[] = {"26KB", "32KB", "40KB"};

    for (int i = 0; i < 3; i++) {
        void* tlsTest = malloc(TLS_SIZES[i]);
        if (tlsTest) {
            memset(tlsTest, 0xCC, TLS_SIZES[i]);
            Serial.printf("[BOOT] ✓ %s allocation successful\n", TLS_NAMES[i]);
            free(tlsTest);
        } else {
            Serial.printf("[BOOT] ❌ %s allocation failed\n", TLS_NAMES[i]);
        }
        delay(HeapPolicy::kBootTlsTestDelayMs);
    }

    // Phase 5: Final consolidation with longer delay
    Serial.println("[BOOT] Phase 5: Final consolidation...");
    delay(HeapPolicy::kBootFinalDelayMs);
    yield();

    // Log final heap state
    size_t finalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t finalFree = ESP.getFreeHeap();
    float improvement = finalLargest > initialLargest ? (float)finalLargest / initialLargest : 1.0f;

    Serial.printf("[BOOT] Final heap: free=%u largest=%u (%.2fx improvement)\n",
                  finalFree, finalLargest, improvement);

    // Check TLS compatibility
    const size_t tlsGate = HeapPolicy::kMinContigForTls;
    if (finalLargest >= tlsGate) {
        Serial.println("[BOOT] ✓ Heap conditioning successful - TLS operations should work");
    } else if (improvement > 1.0f) {
        Serial.printf("[BOOT] ⚠ Partial improvement - largest block=%u (need %u for TLS)\n",
                      (unsigned)finalLargest, (unsigned)tlsGate);
    } else {
        Serial.printf("[BOOT] ❌ No improvement - largest block=%u (need %u for TLS)\n",
                      (unsigned)finalLargest, (unsigned)tlsGate);
    }

    Serial.printf("[BOOT] Total conditioned: ~%u KB\n",
                  (fragAllocated * FRAG_SIZE + structAllocated * STRUCT_SIZE) / 1024);
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

    // --- PATCH: Initialize WiFi driver BEFORE config/display allocate big chunks
    // This dramatically reduces "esp_wifi_init 257" failures on reconnect later.
    preInitWiFiDriverEarly();

    // Load configuration from SD
    if (!Config::init()) {
        Serial.println("[MAIN] Config init failed, using defaults");
    }

    // Init SD logging (will be enabled via settings if user wants)
    SDLog::init();

    // Export any stored core dump to SD (if present)
    exportCoreDumpToSD();

    // Perform heap conditioning to consolidate memory (like Oink mode does)
    // This creates larger contiguous blocks needed for TLS operations
    performBootHeapConditioning();

    // TLS reserve disabled: browser handles TLS, keep heap for UI/file transfer.

    // Init display system
    Display::init();

    // Init audio early so boot sound plays
    SFX::init();

    // Show boot splash (3 screens: OINK OINK, MY NAME IS, PORKCHOP)
    Display::showBootSplash();

    // Apply saved brightness
    M5.Display.setBrightness(Config::personality().brightness * 255 / 100);

    // Initialize piglet personality
    Avatar::init();
    Mood::init();

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

    // Initialize modes
    OinkMode::init();
    WarhogMode::init();
    porkchop.init();

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

    // Update mood system
    Mood::update();

    // Update main controller (handles modes, input, state)
    porkchop.update();

    // Update display
    Display::update();

    // Slower update rate for smoother animation
}
