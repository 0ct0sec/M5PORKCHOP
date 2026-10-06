// UART marker helpers for parsing JanOS/projectZero CLI output.
// Kept dependency-light so we can unit test it on native.
#pragma once

#include <string.h>

namespace C5UartMarkers {

inline bool isHandshakeCapturedLine(const char* line) {
    return line && (strstr(line, "Handshake captured for") != nullptr);
}

inline bool isHandshakeCompleteLine(const char* line) {
    if (!line) return false;

    // Legacy (older docs/firmware variants).
    if (strstr(line, "Handshake attack completed") != nullptr) return true;

    // projectZero JanOS (external/).
    if (strstr(line, "Handshake attack task finished") != nullptr) return true;
    if (strstr(line, "Handshake attack cleanup complete") != nullptr) return true;

    // Selected-mode summary lines (may be wrapped with symbols in some builds).
    // Examples:
    //   "All selected networks have been captured! Attack complete."
    //   "✓✓✓ All selected networks captured! Attack complete. ✓✓✓"
    if (strstr(line, "selected networks") != nullptr &&
        strstr(line, "Attack complete") != nullptr) {
        return true;
    }

    return false;
}

} // namespace C5UartMarkers

