#include "heap_health.h"
#include "heap_policy.h"
#include <Arduino.h>

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

static const uint32_t SAMPLE_INTERVAL_MS = 1000;
static const uint32_t TOAST_DURATION_MS = 5000;  // Match XP top bar duration
static const uint8_t TOAST_MIN_DELTA = 5;

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
    float fragPenalty = fragRatio / 0.60f;  // Penalize fragmentation when largest << total free
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

void update() {
    uint32_t now = millis();
    if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
        return;
    }
    lastSampleMs = now;

    size_t freeHeap = ESP.getFreeHeap();
    size_t largestBlock = ESP.getMaxAllocHeap();
    if (peakFree == 0 || peakLargest == 0) {
        peakFree = freeHeap;
        peakLargest = largestBlock;
    }
    if (minFree == 0 || freeHeap < minFree) minFree = freeHeap;
    if (minLargest == 0 || largestBlock < minLargest) minLargest = largestBlock;
    uint8_t newPct = computePercent(freeHeap, largestBlock, true);

    int delta = (int)newPct - (int)heapHealthPct;
    uint8_t deltaAbs = (delta < 0) ? (uint8_t)(-delta) : (uint8_t)delta;
    heapHealthPct = newPct;

    if (delta != 0 && deltaAbs >= TOAST_MIN_DELTA) {
        if (now - lastToastMs >= TOAST_DURATION_MS) {
            toastDelta = deltaAbs;
            toastImproved = delta > 0;
            toastActive = true;
            toastStartMs = now;
            lastToastMs = now;
        }
    }
}

uint8_t getPercent() {
    return heapHealthPct;
}

void resetPeaks(bool suppressToast) {
    peakFree = ESP.getFreeHeap();
    peakLargest = ESP.getMaxAllocHeap();
    minFree = peakFree;
    minLargest = peakLargest;
    heapHealthPct = computePercent(peakFree, peakLargest, false);

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
    if (millis() - toastStartMs >= TOAST_DURATION_MS) {
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

}  // namespace HeapHealth
