#pragma once

#include <cstddef>
#include <cstdint>

namespace HeapPolicy {
    // TLS gating thresholds
    static constexpr size_t kMinHeapForTls = 35000;
    static constexpr size_t kMinContigForTls = 35000;
    static constexpr size_t kProactiveTlsConditioning = 45000;

    // General allocation safety thresholds
    static constexpr size_t kMinHeapForOinkNetworkAdd = 30000;
    static constexpr size_t kMinHeapForHandshakeAdd = 60000;
    static constexpr size_t kMinHeapForReconGrowth = 20000;
    static constexpr size_t kMinHeapForSpectrumGrowth = 20000;

    // Heap stabilization / recovery thresholds
    static constexpr size_t kHeapStableThreshold = 50000;
    static constexpr size_t kXferServerMinHeap = 40000;
    static constexpr size_t kXferServerMinLargest = 30000;
    static constexpr size_t kXferServerLogThreshold = 60000;
    static constexpr size_t kXferServerUiMinFree = 12000;
    static constexpr size_t kXferServerUiMinLargest = 8000;

    // Allocation slack (allocator overhead / fragmentation cushion)
    static constexpr size_t kReserveSlackSmall = 256;
    static constexpr size_t kReserveSlackLarge = 1024;
    static constexpr size_t kPmkidAllocSlack = 256;
    static constexpr size_t kHandshakeAllocSlack = 1024;

    // Mode-specific thresholds
    static constexpr size_t kDnhInjectMinHeap = 80000;
    static constexpr size_t kPigSyncMinContig = 26000;

    // Heap health sampling/tuning
    static constexpr uint32_t kHealthSampleIntervalMs = 1000;
    static constexpr uint32_t kHealthToastDurationMs = 5000;
    static constexpr uint8_t kHealthToastMinDelta = 5;
    static constexpr uint32_t kHealthToastSettleMs = 3000;
    static constexpr uint8_t kHealthConditionTriggerPct = 65;
    static constexpr uint8_t kHealthConditionClearPct = 75;
    static constexpr float kHealthFragPenaltyScale = 0.60f;

    // Display EMA smoothing (asymmetric to absorb transient spikes)
    static constexpr float kDisplayEmaAlphaDown = 0.10f;  // Slow to drop (absorb transients)
    static constexpr float kDisplayEmaAlphaUp   = 0.20f;  // Moderate recovery

    // Adaptive conditioning cooldown (replaces fixed 30s)
    // Formula: cooldown = clamp(min, max, base * (largestBlock / kMinContigForTls))
    // When heap is stressed (largestBlock << kMinContigForTls), cooldown hits 15s floor
    // When heap is healthy (largestBlock > kMinContigForTls), cooldown is long (60s)
    static constexpr uint32_t kConditionCooldownMinMs = 15000;
    static constexpr uint32_t kConditionCooldownMaxMs = 60000;
    static constexpr uint32_t kConditionCooldownBaseMs = 30000;

    // Memory pressure levels (graduated degradation)
    // Level 0 (Normal): all features enabled
    // Level 1 (Caution): reduce non-essential features
    // Level 2 (Warning): aggressive memory shedding
    // Level 3 (Critical): freeze state, auto-brew, graceful recovery
    static constexpr size_t kPressureLevel1Free = 80000;    // Below = caution
    static constexpr size_t kPressureLevel2Free = 50000;    // Below = warning
    static constexpr size_t kPressureLevel3Free = 30000;    // Below = critical
    static constexpr float kPressureLevel1Frag = 0.60f;     // Below = caution
    static constexpr float kPressureLevel2Frag = 0.40f;     // Below = warning
    static constexpr float kPressureLevel3Frag = 0.25f;     // Below = critical
    static constexpr uint32_t kPressureHysteresisMs = 3000;  // Min time before level decrease

    // Pressure level gates for expensive operations
    // Auto-brew blocked at Critical (brew needs 35KB transient, critical has <30KB)
    static constexpr uint8_t kMaxPressureLevelForAutoBrew = 2;  // Warning
    // SD writes blocked at Warning+ (file ops allocate FAT/handle buffers)
    static constexpr uint8_t kMaxPressureLevelForSDWrite = 1;   // Caution

    // Watermark persistence interval (auto-save to SD)
    static constexpr uint32_t kWatermarkSaveIntervalMs = 60000;

    // Knuth's Rule monitoring (free_blocks / allocated_blocks ratio)
    // By the Fifty Percent Rule, this should be ~0.5 at steady state.
    // Values significantly above 0.7 indicate pathological fragmentation.
    static constexpr float kKnuthRatioWarning = 0.70f;

    // Growth gating (fragmentation-aware)
    static constexpr float kMinFragRatioForGrowth = 0.40f;

    // Stress test guardrail
    static constexpr size_t kStressMinHeap = 70000;

    // Runtime conditioning dwell times (used by OINK Bounce / brewHeap)
    static constexpr uint32_t kConditioningDwellMs = 3000;
    static constexpr uint32_t kConditioningStepMs = 100;
    static constexpr uint32_t kConditioningWarmupMs = 1000;
    static constexpr uint32_t kConditioningLogIntervalMs = 1000;
    static constexpr uint32_t kConditioningFinalDelayMs = 50;
    static constexpr uint32_t kBrewDefaultDwellMs = 1000;
    static constexpr uint32_t kBrewAutoDwellMs = 1200;
    // LWIP async TCP cleanup polling
    // lwip_close() returns immediately but TCP PCB/buffers are freed by LWIP
    // timer task on a 250-500ms cycle. Poll until heap stabilizes.
    static constexpr uint32_t kLwipCleanupWaitMaxMs = 500;
    static constexpr uint32_t kLwipCleanupPollMs = 50;

    // WiFi/BLE settle delays used during conditioning/reset
    static constexpr uint32_t kWiFiModeDelayMs = 50;
    static constexpr uint32_t kWiFiDisconnectDelayMs = 50;
    static constexpr uint32_t kWiFiShutdownDelayMs = 80;
    static constexpr uint32_t kBleStopDelayMs = 50;
    static constexpr uint32_t kBleDeinitDelayMs = 100;

    // NTP sync policy
    static constexpr int kNtpRssiMinDbm = -60;
    static constexpr uint32_t kNtpTimeoutMs = 6000;
    static constexpr uint32_t kNtpMinFreeHeap = 20000;
    static constexpr uint32_t kNtpMinContig = 8000;
    static constexpr uint32_t kNtpRetryCooldownMs = 60000;

}
