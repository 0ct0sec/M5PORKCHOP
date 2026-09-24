#include "heap_health.h"
#include "heap_policy.h"
#include "config.h"
#include "sd_layout.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>

namespace HeapHealth {

static uint8_t heapHealthPct = 100;
static uint32_t lastSampleMs = 0;
static uint32_t toastStartMs = 0;
static uint32_t lastToastMs = 0;
static uint8_t toastDelta = 0;
static bool toastImproved = false;
static bool toastActive = false;
static size_t peakFree = 0;
static size_t peakLargest = 0;
static size_t minFree = 0;
static size_t minLargest = 0;
static bool conditionPending = false;
static uint32_t lastConditionMs = 0;
static uint8_t stableHealthPct = 100;
static float displayPctF = 100.0f;  // EMA-smoothed value for UI (float for precision)
static bool pendingToast = false;
static uint32_t pendingToastMs = 0;

// Graduated pressure level with hysteresis
static HeapPressureLevel pressureLevel = HeapPressureLevel::Normal;
static uint32_t lastPressureChangeMs = 0;
static uint8_t escalationCount = 0;
static HeapPressureLevel escalationTarget = HeapPressureLevel::Normal;

// Knuth's Rule metric: free_blocks / allocated_blocks
static float knuthRatio = 0.0f;
// Only compute when diagnostics is viewing (saves ~50us/sec of heap enumeration)
static bool knuthEnabled = false;

static uint8_t computePercent(size_t freeHeap, size_t largestBlock, bool updatePeaks) {
    if (updatePeaks) {
        if (freeHeap > peakFree) peakFree = freeHeap;
        if (largestBlock > peakLargest) peakLargest = largestBlock;
    }

    float freeNorm = peakFree > 0 ? (float)freeHeap / (float)peakFree : 0.0f;
    float contigNorm = peakLargest > 0 ? (float)largestBlock / (float)peakLargest : 0.0f;
    float thresholdNorm = 1.0f;
    if (HeapPolicy::kMinHeapForTls > 0 && HeapPolicy::kMinContigForTls > 0) {
        float freeGate = (float)freeHeap / (float)HeapPolicy::kMinHeapForTls;
        float contigGate = (float)largestBlock / (float)HeapPolicy::kMinContigForTls;
        thresholdNorm = (freeGate < contigGate) ? freeGate : contigGate;
    }

    float health = freeNorm < contigNorm ? freeNorm : contigNorm;
    if (thresholdNorm < health) health = thresholdNorm;

    float fragRatio = freeHeap > 0 ? (float)largestBlock / (float)freeHeap : 0.0f;
    float fragPenalty = fragRatio / HeapPolicy::kHealthFragPenaltyScale;
    if (fragPenalty < 0.0f) fragPenalty = 0.0f;
    if (fragPenalty > 1.0f) fragPenalty = 1.0f;
    health *= fragPenalty;

    if (health < 0.0f) health = 0.0f;
    if (health > 1.0f) health = 1.0f;

    int pct = (int)(health * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

// Compute adaptive conditioning cooldown based on current heap state.
// When heap is critical (largestBlock much smaller than TLS threshold),
// allow more frequent conditioning. When healthy, back off.
static uint32_t adaptiveCooldownMs(size_t largestBlock) {
    if (HeapPolicy::kMinContigForTls == 0) return HeapPolicy::kConditionCooldownBaseMs;
    float ratio = (float)largestBlock / (float)HeapPolicy::kMinContigForTls;
    uint32_t cooldown = (uint32_t)((float)HeapPolicy::kConditionCooldownBaseMs * ratio);
    if (cooldown < HeapPolicy::kConditionCooldownMinMs) cooldown = HeapPolicy::kConditionCooldownMinMs;
    if (cooldown > HeapPolicy::kConditionCooldownMaxMs) cooldown = HeapPolicy::kConditionCooldownMaxMs;
    return cooldown;
}

// Compute pressure level from raw heap metrics.
// Uses the more severe signal (free heap OR frag ratio) to determine level.
static HeapPressureLevel computePressureLevel(size_t freeHeap, float fragRatio) {
    // Check from most severe to least
    if (freeHeap < HeapPolicy::kPressureLevel3Free || fragRatio < HeapPolicy::kPressureLevel3Frag)
        return HeapPressureLevel::Critical;
    if (freeHeap < HeapPolicy::kPressureLevel2Free || fragRatio < HeapPolicy::kPressureLevel2Frag)
        return HeapPressureLevel::Warning;
    if (freeHeap < HeapPolicy::kPressureLevel1Free || fragRatio < HeapPolicy::kPressureLevel1Frag)
        return HeapPressureLevel::Caution;
    return HeapPressureLevel::Normal;
}

void update() {
    uint32_t now = millis();
    if (now - lastSampleMs < HeapPolicy::kHealthSampleIntervalMs) {
        return;
    }
    lastSampleMs = now;

    size_t freeHeap = ESP.getFreeHeap();
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (peakFree == 0 || peakLargest == 0) {
        peakFree = freeHeap;
        peakLargest = largestBlock;
    }
    if (minFree == 0 || freeHeap < minFree) minFree = freeHeap;
    if (minLargest == 0 || largestBlock < minLargest) minLargest = largestBlock;
    uint8_t newPct = computePercent(freeHeap, largestBlock, true);
    heapHealthPct = newPct;

    // On first sample, snap display to actual value (skip EMA convergence from 100%)
    static bool firstSample = true;
    if (firstSample) {
        displayPctF = (float)newPct;
        stableHealthPct = newPct;
        firstSample = false;
    } else {
        // Asymmetric EMA for display: slow to drop (absorbs transient dips), moderate recovery
        float alpha = (newPct < displayPctF)
            ? HeapPolicy::kDisplayEmaAlphaDown
            : HeapPolicy::kDisplayEmaAlphaUp;
        displayPctF += alpha * ((float)newPct - displayPctF);
    }

    float fragRatio = freeHeap > 0 ? (float)largestBlock / (float)freeHeap : 0.0f;

    // --- Knuth's Rule metric (Fifty Percent Rule) ---
    // Only computed when diagnostics is active (saves ~50us/sec heap enumeration)
    if (knuthEnabled) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);
        if (info.allocated_blocks > 0) {
            knuthRatio = (float)info.free_blocks / (float)info.allocated_blocks;
        }
    }

    // --- Graduated pressure level with hysteresis ---
    HeapPressureLevel newLevel = computePressureLevel(freeHeap, fragRatio);
    if (newLevel != pressureLevel) {
        if (newLevel > pressureLevel) {
            // Escalating: require 2 consecutive samples at same target (except Critical = immediate)
            if (newLevel != escalationTarget) {
                escalationCount = 0;
                escalationTarget = newLevel;
            }
            escalationCount++;
            uint8_t threshold = (newLevel == HeapPressureLevel::Critical) ? 1 : 2;
            if (escalationCount >= threshold) {
                pressureLevel = newLevel;
                lastPressureChangeMs = now;
                escalationCount = 0;
            }
        } else if ((now - lastPressureChangeMs) >= HeapPolicy::kPressureHysteresisMs) {
            // De-escalating: step down one level at a time after hysteresis
            pressureLevel = (HeapPressureLevel)((uint8_t)pressureLevel - 1);
            lastPressureChangeMs = now;
            escalationCount = 0;
        }
    } else {
        escalationCount = 0;
    }

    // --- Adaptive conditioning trigger ---
    bool contigLow = largestBlock < HeapPolicy::kProactiveTlsConditioning;
    bool pctLow = newPct <= HeapPolicy::kHealthConditionTriggerPct;
    uint32_t cooldown = adaptiveCooldownMs(largestBlock);
    if (!conditionPending) {
        if (pctLow && contigLow &&
            (lastConditionMs == 0 || (now - lastConditionMs) >= cooldown)) {
            conditionPending = true;
        }
    } else {
        bool pctRecovered = newPct >= HeapPolicy::kHealthConditionClearPct;
        bool contigRecovered = largestBlock >= HeapPolicy::kProactiveTlsConditioning;
        if (pctRecovered && contigRecovered) {
            conditionPending = false;
        }
    }

    // Debounced toast: use smoothed display value so transient spikes
    // that the EMA absorbs don't trigger user-visible notifications
    uint8_t smoothedPct = (uint8_t)(displayPctF + 0.5f);
    int netDelta = (int)smoothedPct - (int)stableHealthPct;
    uint8_t netDeltaAbs = (netDelta < 0) ? (uint8_t)(-netDelta) : (uint8_t)netDelta;

    if (netDeltaAbs >= HeapPolicy::kHealthToastMinDelta) {
        if (!pendingToast) {
            pendingToast = true;
            pendingToastMs = now;
        }
        if ((now - pendingToastMs >= HeapPolicy::kHealthToastSettleMs) &&
            (now - lastToastMs >= HeapPolicy::kHealthToastDurationMs)) {
            toastDelta = netDeltaAbs;
            toastImproved = netDelta > 0;
            toastActive = true;
            toastStartMs = now;
            lastToastMs = now;
            stableHealthPct = smoothedPct;
            pendingToast = false;
        }
    } else {
        pendingToast = false;
        stableHealthPct = smoothedPct;
    }
}

uint8_t getPercent() {
    return heapHealthPct;
}

uint8_t getDisplayPercent() {
    int pct = (int)(displayPctF + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

HeapPressureLevel getPressureLevel() {
    return pressureLevel;
}

float getKnuthRatio() {
    return knuthRatio;
}

void resetPeaks(bool suppressToast) {
    peakFree = ESP.getFreeHeap();
    peakLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    // NOTE: Do NOT reset minFree/minLargest here. Session watermarks must track
    // the true session-worst values. Resetting them mid-brew would corrupt them
    // with transient values (WiFi buffers eat 35KB during conditioning).
    heapHealthPct = computePercent(peakFree, peakLargest, false);
    conditionPending = false;
    lastConditionMs = millis();

    stableHealthPct = heapHealthPct;
    displayPctF = (float)heapHealthPct;
    pendingToast = false;

    if (suppressToast) {
        toastActive = false;
        toastDelta = 0;
        toastImproved = false;
        lastToastMs = millis();
        lastSampleMs = millis();
    }
}

bool shouldShowToast() {
    if (!toastActive) return false;
    if (millis() - toastStartMs >= HeapPolicy::kHealthToastDurationMs) {
        toastActive = false;
        return false;
    }
    return true;
}

bool isToastImproved() {
    return toastImproved;
}

uint8_t getToastDelta() {
    return toastDelta;
}

uint32_t getMinFree() {
    return (uint32_t)minFree;
}

uint32_t getMinLargest() {
    return (uint32_t)minLargest;
}

bool consumeConditionRequest() {
    if (!conditionPending) return false;
    conditionPending = false;
    return true;
}

// --- Watermark persistence ---
// Binary file format: magic(4) + record(20) bytes total (packed).
// Overwritten each save, read at boot for previous session comparison.
static constexpr uint32_t kWatermarkMagic = 0x48574D4B; // 'HWMK'
static uint32_t lastWatermarkSaveMs = 0;
static uint32_t prevSessionMinFree = 0;
static uint32_t prevSessionMinLargest = 0;

struct __attribute__((packed)) WatermarkRecord {
    uint32_t magic;
    uint32_t uptimeSec;
    uint32_t minFreeVal;
    uint32_t minLargestVal;
    uint8_t  minHealthPct;
    uint8_t  maxPressureSeen;
    uint16_t reserved;
};

static uint8_t sessionMinHealthPct = 100;
static uint8_t sessionMaxPressure = 0;

void loadPreviousSession() {
    if (!Config::isSDAvailable()) return;
    const char* path = SDLayout::heapWatermarksPath();
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    WatermarkRecord rec;
    if (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) == sizeof(rec)) {
        if (rec.magic == kWatermarkMagic) {
            prevSessionMinFree = rec.minFreeVal;
            prevSessionMinLargest = rec.minLargestVal;
            Serial.printf("[HEAP] Previous session: minFree=%u minLargest=%u uptime=%us pressure=%u\n",
                          rec.minFreeVal, rec.minLargestVal, rec.uptimeSec, rec.maxPressureSeen);
        }
    }
    f.close();
}

void persistWatermarks() {
    uint32_t now = millis();
    if (now - lastWatermarkSaveMs < HeapPolicy::kWatermarkSaveIntervalMs) return;
    lastWatermarkSaveMs = now;
    // Block SD writes at Warning+ pressure — file ops allocate FAT/handle buffers
    if (static_cast<uint8_t>(pressureLevel) > HeapPolicy::kMaxPressureLevelForSDWrite) return;
    if (!Config::isSDAvailable()) return;

    // Track session extremes
    if (heapHealthPct < sessionMinHealthPct) sessionMinHealthPct = heapHealthPct;
    uint8_t pl = static_cast<uint8_t>(pressureLevel);
    if (pl > sessionMaxPressure) sessionMaxPressure = pl;

    const char* path = SDLayout::heapWatermarksPath();
    const char* diagDir = SDLayout::diagnosticsDir();
    if (strcmp(diagDir, "/") != 0 && !SD.exists(diagDir)) {
        SD.mkdir(diagDir);
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    WatermarkRecord rec;
    rec.magic = kWatermarkMagic;
    rec.uptimeSec = now / 1000;
    rec.minFreeVal = (uint32_t)minFree;
    rec.minLargestVal = (uint32_t)minLargest;
    rec.minHealthPct = sessionMinHealthPct;
    rec.maxPressureSeen = sessionMaxPressure;
    rec.reserved = 0;
    f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
    f.close();
}

uint32_t getPrevMinFree() {
    return prevSessionMinFree;
}

uint32_t getPrevMinLargest() {
    return prevSessionMinLargest;
}

void setKnuthEnabled(bool enable) {
    knuthEnabled = enable;
    if (!enable) {
        knuthRatio = 0.0f;
    }
}

}  // namespace HeapHealth
