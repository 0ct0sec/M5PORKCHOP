// Tls - one home for the board's TLS plumbing.
//
//  * Arena: lend a fixed static buffer to mbedTLS during a sync so the ~16KB
//    TLS record buffer comes from reserved static memory instead of the heap
//    (avoids heap exhaustion + fragmentation on this PSRAM-less board).
//  * streamFile: paced upload of an open file over a connected TLS client,
//    shared by the WiGLE / WPA-SEC uploaders.
#pragma once

#include <cstddef>
#include <FS.h>  // fs::File

class WiFiClientSecure;

namespace Tls {
    // Install `buf` (size bytes) as mbedTLS's primary allocator for the duration
    // of a TLS operation. Allocations are served from the arena first; when it is
    // exhausted they fall back to the system heap (transient — freed on connection
    // close, so they coalesce and don't permanently fragment).
    //
    // MUST be paired with arenaEnd(). Not reentrant. Call from a single task with
    // no other concurrent mbedTLS user (NetworkRecon is paused during sync). The
    // buffer must NOT be read/written by anything else between begin and end.
    void arenaBegin(void* buf, size_t size);

    // Restore the default mbedTLS allocator. Safe to call if arenaBegin was a no-op.
    void arenaEnd();

    // Stream `fileSize` bytes from already-open `file` to connected `client`,
    // with heap pacing: a large upload feeds the TLS/TCP send path faster than
    // the WiFi link drains it, so un-acked data piles up in heap. When free heap
    // dips we drain the send queue before feeding more, and clean-abort if it
    // can't recover (instead of letting mbedTLS OOM-kill the connection).
    //
    // `tag` is a short label for the serial trace (e.g. "WIGLE"). Always closes
    // `file`. On success leaves `client` open (caller sends trailer); on failure
    // stops `client` and writes a short reason to outError. Returns bytes-sent ==
    // fileSize.
    bool streamFile(WiFiClientSecure& client, File& file, size_t fileSize,
                    const char* tag, char* outError, size_t outErrorLen);
}
