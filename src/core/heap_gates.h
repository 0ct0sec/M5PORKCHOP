#pragma once

#include <cstddef>
#include <cstdint>

namespace HeapGates {
    enum class TlsGateFailure : uint8_t {
        None = 0,
        Fragmented,
        LowHeap
    };

    struct TlsGateStatus {
        size_t freeHeap;
        size_t largestBlock;
        TlsGateFailure failure;
    };

    struct GateStatus {
        size_t freeHeap;
        size_t largestBlock;
        size_t minFree;
        size_t minContig;
        TlsGateFailure failure;
    };

    struct HeapSnapshot {
        size_t freeHeap;
        size_t largestBlock;
        float fragRatio;
    };

    // Snapshot current heap and evaluate TLS gating status.
    TlsGateStatus checkTlsGates();

    // Return true if TLS can proceed, and optionally format an error string.
    bool canTls(const TlsGateStatus& status, char* outError, size_t outErrorLen);

    // True when we are above the TLS gate but below the proactive threshold.
    bool shouldProactivelyCondition(const TlsGateStatus& status);

    // Generic gate checks (free + contiguous).
    GateStatus checkGate(size_t minFree, size_t minContig);
    bool canMeet(const GateStatus& status, char* outError, size_t outErrorLen);

    // Snapshot heap metrics for growth gating (free, largest, frag ratio).
    HeapSnapshot snapshot();

    // Fragmentation-aware growth gate.
    bool canGrow(const HeapSnapshot& status, size_t minFreeHeap, float minFragRatio);
    bool canGrow(size_t minFreeHeap, float minFragRatio);

    // Wait for LWIP async TCP cleanup after TLS connection close.
    // Polls heap metrics until they stabilize or timeout (500ms max).
    void waitForLwipCleanup();
}
