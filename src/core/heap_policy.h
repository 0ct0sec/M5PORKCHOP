#pragma once

#include <cstddef>

namespace HeapPolicy {
    // TLS gating thresholds
    static constexpr size_t kMinHeapForTls = 35000;
    static constexpr size_t kMinContigForTls = 35000;
    static constexpr size_t kProactiveTlsConditioning = 45000;

    // General allocation safety thresholds
    static constexpr size_t kMinHeapForOinkNetworkAdd = 30000;
    static constexpr size_t kMinHeapForHandshakeAdd = 60000;
    static constexpr size_t kMinHeapForReconGrowth = 20000;
    static constexpr size_t kMinHeapForDnhGrowth = 40000;
    static constexpr size_t kMinHeapForSpectrumGrowth = 20000;

    // Heap stabilization / recovery thresholds
    static constexpr size_t kHeapStableThreshold = 50000;
    static constexpr size_t kFileServerRecoveryThreshold = 50000;
    static constexpr size_t kFileServerMinHeap = 40000;
    static constexpr size_t kFileServerMinLargest = 30000;

    // Mode-specific thresholds
    static constexpr size_t kWarhogHeapWarning = 40000;
    static constexpr size_t kWarhogHeapCritical = 25000;

    // Stress test guardrail
    static constexpr size_t kStressMinHeap = 70000;
}
