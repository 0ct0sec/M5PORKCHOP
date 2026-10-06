#include "heap_gates.h"
#include "heap_policy.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace HeapGates {

TlsGateStatus checkTlsGates() {
    GateStatus gate = checkGate(HeapPolicy::kMinHeapForTls,
                                HeapPolicy::kMinContigForTls);
    return {gate.freeHeap, gate.largestBlock, gate.failure};
}

bool canTls(const TlsGateStatus& status, char* outError, size_t outErrorLen) {
    if (status.failure == TlsGateFailure::None) {
        return true;
    }
    if (!outError || outErrorLen == 0) {
        return false;
    }
    if (status.failure == TlsGateFailure::Fragmented) {
        snprintf(outError, outErrorLen, "FRAG: %uKB/%uKB",
                 (unsigned int)(status.largestBlock / 1024),
                 (unsigned int)(status.freeHeap / 1024));
    } else {
        snprintf(outError, outErrorLen, "LOW HEAP: %uKB",
                 (unsigned int)(status.freeHeap / 1024));
    }
    return false;
}

GateStatus checkGate(size_t minFree, size_t minContig) {
    size_t freeHeap = ESP.getFreeHeap();
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    TlsGateFailure failure = TlsGateFailure::None;
    if (minContig > 0 && largestBlock < minContig) {
        failure = TlsGateFailure::Fragmented;
    } else if (minFree > 0 && freeHeap < minFree) {
        failure = TlsGateFailure::LowHeap;
    }
    return {freeHeap, largestBlock, minFree, minContig, failure};
}

bool canMeet(const GateStatus& status, char* outError, size_t outErrorLen) {
    if (status.failure == TlsGateFailure::None) {
        return true;
    }
    if (!outError || outErrorLen == 0) {
        return false;
    }
    if (status.failure == TlsGateFailure::Fragmented) {
        snprintf(outError, outErrorLen, "FRAG: %uKB/%uKB",
                 (unsigned int)(status.largestBlock / 1024),
                 (unsigned int)(status.freeHeap / 1024));
    } else {
        snprintf(outError, outErrorLen, "LOW HEAP: %uKB",
                 (unsigned int)(status.freeHeap / 1024));
    }
    return false;
}

bool shouldProactivelyCondition(const TlsGateStatus& status) {
    return (status.largestBlock < HeapPolicy::kProactiveTlsConditioning &&
            status.largestBlock >= HeapPolicy::kMinContigForTls);
}

HeapSnapshot snapshot() {
    size_t freeHeap = ESP.getFreeHeap();
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    float fragRatio = freeHeap > 0 ? (float)largestBlock / (float)freeHeap : 0.0f;
    return {freeHeap, largestBlock, fragRatio};
}

bool canGrow(const HeapSnapshot& status, size_t minFreeHeap, float minFragRatio) {
    if (status.freeHeap < minFreeHeap) {
        return false;
    }
    if (minFragRatio > 0.0f && status.fragRatio < minFragRatio) {
        return false;
    }
    return true;
}

bool canGrow(size_t minFreeHeap, float minFragRatio) {
    return canGrow(snapshot(), minFreeHeap, minFragRatio);
}

void waitForLwipCleanup() {
    size_t prevFree = ESP.getFreeHeap();
    size_t prevLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t waitStart = millis();

    while ((millis() - waitStart) < HeapPolicy::kLwipCleanupWaitMaxMs) {
        delay(HeapPolicy::kLwipCleanupPollMs);
        yield();
        size_t curFree = ESP.getFreeHeap();
        size_t curLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (curFree == prevFree && curLargest == prevLargest) break;
        prevFree = curFree;
        prevLargest = curLargest;
    }
}

}  // namespace HeapGates
