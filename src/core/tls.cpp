// Tls implementation — arena + paced upload.

#include "tls.h"
#include "heap_policy.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <cstdlib>
#include <cstring>
#include <multi_heap.h>
#include <mbedtls/platform.h>

namespace {
    multi_heap_handle_t g_heap = nullptr;
    uint8_t* g_lo = nullptr;
    uint8_t* g_hi = nullptr;

    // mbedTLS allocator: serve from the static arena first, fall back to the
    // system heap when the arena is full. memset because mbedtls_calloc semantics
    // require zeroed memory.
    void* arena_calloc(size_t n, size_t size) {
        size_t need = n * size;
        if (size != 0 && need / size != n) return nullptr;  // multiply overflow
        if (g_heap) {
            void* p = multi_heap_malloc(g_heap, need);
            if (p) { memset(p, 0, need); return p; }
        }
        return calloc(n, size);
    }

    void arena_free(void* p) {
        if (!p) return;
        if ((uint8_t*)p >= g_lo && (uint8_t*)p < g_hi) {
            multi_heap_free(g_heap, p);
            return;
        }
        free(p);
    }
}

namespace Tls {

void arenaBegin(void* buf, size_t size) {
    if (g_heap || !buf || size < 1024) return;  // already active, or unusable

    g_heap = multi_heap_register(buf, size);
    if (!g_heap) {
        Serial.println("[TLS] arena register failed; mbedTLS uses heap only");
        return;
    }
    g_lo = (uint8_t*)buf;
    g_hi = g_lo + size;
    mbedtls_platform_set_calloc_free(arena_calloc, arena_free);
    Serial.printf("[TLS] arena begin: arena=%uB usable=%uB\n",
                  (unsigned)size, (unsigned)multi_heap_free_size(g_heap));
}

void arenaEnd() {
    if (!g_heap) return;
    // Restore the stock allocator. With CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC off, the
    // ESP-IDF default mbedtls_calloc/free route to libc calloc/free (the heap),
    // so restoring those is equivalent to the original behaviour.
    mbedtls_platform_set_calloc_free(calloc, free);
    g_heap = nullptr;
    g_lo = g_hi = nullptr;
    Serial.println("[TLS] arena end");
}

bool streamFile(WiFiClientSecure& client, File& file, size_t fileSize,
                const char* tag, char* outError, size_t outErrorLen) {
    const size_t CHUNK_SIZE = 2048;  // fewer TLS records than tiny chunks
    uint8_t chunk[CHUNK_SIZE];
    size_t bytesRemaining = fileSize;
    size_t bytesSent = 0;
    size_t nextLogAt = 0;  // heap trace every 32KB

    Serial.printf("[%s] write start: size=%u free=%u largest=%u\n",
                  tag, (unsigned)fileSize, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    while (bytesRemaining > 0) {
        // Periodic heap trace so a stalled/failed upload shows the curve.
        if (bytesSent >= nextLogAt) {
            Serial.printf("[%s] write progress: sent=%u/%u free=%u largest=%u\n",
                          tag, (unsigned)bytesSent, (unsigned)fileSize,
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            nextLogAt = bytesSent + 32768;
        }

        // Heap pacing: free heap dipped -> drain the send queue before feeding
        // more; clean-abort if it can't recover. (See header for the why.)
        if (ESP.getFreeHeap() < HeapPolicy::kTlsWriteSoftFloor) {
            client.flush();
            uint32_t drainStart = millis();
            while (ESP.getFreeHeap() < HeapPolicy::kTlsWriteSoftFloor &&
                   millis() - drainStart < HeapPolicy::kTlsWriteDrainMs) {
                if (!client.connected()) break;
                delay(20);
                yield();
            }
            if (ESP.getFreeHeap() < HeapPolicy::kTlsWriteHardFloor) {
                snprintf(outError, outErrorLen, "HEAP LOW @%uB", (unsigned)bytesSent);
                Serial.printf("[%s] Heap floor hit mid-upload: sent=%u/%u free=%u — aborting\n",
                              tag, (unsigned)bytesSent, (unsigned)fileSize,
                              (unsigned)ESP.getFreeHeap());
                file.close();
                client.stop();
                return false;
            }
        }

        if (!client.connected()) {
            char tlsErr[64] = {0};
            int errCode = client.lastError(tlsErr, sizeof(tlsErr) - 1);
            snprintf(outError, outErrorLen, "CONN LOST @%uB: %d", (unsigned)bytesSent, errCode);
            Serial.printf("[%s] Connection lost: sent=%u/%u, err=%d (%s)\n",
                          tag, (unsigned)bytesSent, (unsigned)fileSize, errCode, tlsErr);
            file.close();
            client.stop();
            return false;
        }

        size_t toRead = (bytesRemaining > CHUNK_SIZE) ? CHUNK_SIZE : bytesRemaining;
        size_t bytesRead = file.read(chunk, toRead);
        if (bytesRead == 0) {
            snprintf(outError, outErrorLen, "SD READ @%uB", (unsigned)bytesSent);
            Serial.printf("[%s] SD read failed at %u/%u\n",
                          tag, (unsigned)bytesSent, (unsigned)fileSize);
            file.close();
            client.stop();
            return false;
        }

        size_t written = client.write(chunk, bytesRead);
        if (written != bytesRead) {
            char tlsErr[64] = {0};
            int errCode = client.lastError(tlsErr, sizeof(tlsErr) - 1);
            snprintf(outError, outErrorLen, "TLS WRITE: %d @%uB", errCode, (unsigned)bytesSent);
            Serial.printf("[%s] TLS write failed: wrote=%u/%u, sent=%u/%u, err=%d (%s), conn=%d\n",
                          tag, (unsigned)written, (unsigned)bytesRead,
                          (unsigned)bytesSent, (unsigned)fileSize,
                          errCode, tlsErr, client.connected());
            file.close();
            client.stop();
            return false;
        }

        bytesSent += bytesRead;
        bytesRemaining -= bytesRead;
        yield();  // let the WiFi stack breathe
    }

    file.close();
    return true;
}

}  // namespace Tls
