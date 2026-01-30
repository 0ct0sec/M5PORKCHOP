#pragma once

#include <cstdint>

namespace HeapHealth {
    // Update heap health state (rate-limited).
    void update();

    // Current heap health percent (0-100).
    uint8_t getPercent();

    // Reset peak baseline to current heap values.
    void resetPeaks(bool suppressToast = true);

    // Toast helpers
    bool shouldShowToast();
    bool isToastImproved();
    uint8_t getToastDelta();
}
