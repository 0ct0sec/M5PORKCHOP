#pragma once

// Pure helpers for OINK capture gating/filters.
// Kept header-only so it can be unit tested in env:native without hardware deps.

#include <cstdint>
#include <cstddef>

#include "../core/heap_health.h"

namespace OinkCaptureFilters {

inline bool isUnicastMac(const uint8_t* mac) {
    return mac && ((mac[0] & 0x01) == 0);
}

inline bool shouldMarkDeauthSuccessOnM1(bool stationIsOurs, bool stationIsUnicast) {
    // PMKID probing can cause AP->STA M1 retransmits to our own station MAC.
    // Those must not be treated as "deauth success" (they were not induced by our target deauth).
    return (!stationIsOurs) && stationIsUnicast;
}

inline bool shouldStoreHandshakeForStation(bool stationIsOurs) {
    // Don't store probe-induced (station==our MAC) EAPOL frames as "handshakes".
    // PMKID extraction happens separately and remains enabled.
    return !stationIsOurs;
}

enum class HandshakeCreateBlockReason : uint8_t {
    None = 0,
    MaxHandshakes,
    Pressure,
    FreeHeap,
    Fragmentation,
};

struct HandshakeCreateGateResult {
    bool allowCreate = false;
    bool allowBeaconCopy = false;
    HandshakeCreateBlockReason blockReason = HandshakeCreateBlockReason::None;
};

inline HandshakeCreateGateResult evaluateHandshakeCreateGate(size_t size, size_t capacity,
                                                             size_t maxHandshakes,
                                                             HeapPressureLevel pressure,
                                                             size_t freeHeap, size_t minFreeHeap,
                                                             size_t largestBlock,
                                                             size_t minAllocBlock) {
    HandshakeCreateGateResult r{};
    r.allowBeaconCopy = (pressure < HeapPressureLevel::Warning);

    if (size >= maxHandshakes) {
        r.blockReason = HandshakeCreateBlockReason::MaxHandshakes;
        return r;
    }

    const bool needGrow = size >= capacity;
    if (needGrow) {
        // Only apply aggressive heap/pressure shedding when we'd allocate/grow.
        if (pressure >= HeapPressureLevel::Warning) {
            r.blockReason = HandshakeCreateBlockReason::Pressure;
            return r;
        }
        if (freeHeap < minFreeHeap) {
            r.blockReason = HandshakeCreateBlockReason::FreeHeap;
            return r;
        }
        if (largestBlock < minAllocBlock) {
            r.blockReason = HandshakeCreateBlockReason::Fragmentation;
            return r;
        }
    }

    r.allowCreate = true;
    r.blockReason = HandshakeCreateBlockReason::None;
    return r;
}

}  // namespace OinkCaptureFilters

