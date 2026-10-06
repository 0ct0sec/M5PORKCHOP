// JanusHog - ESP32-C5 Coprocessor Service (JANUS HOG)
// Peripheral service like GPS — always-on when enabled, feeds 5GHz data
// into NetworkRecon pipeline. Modes don't need to know it exists until
// they route a 5GHz attack.
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>

#include "network_source.h"
#include "../gps/gps.h"  // GPSData struct

enum class C5State : uint8_t {
    OFF,            // Config disabled or not initialized
    DISCONNECTED,   // Probing for C5 on UART (ping every 2s)
    CONNECTED,      // Idle, ready for commands
    SCANNING,       // scan_networks in progress
    ATTACKING,      // handshake/sae/deauth running on C5
    MONITORING,     // channel_view or packet_monitor running
    TRANSFERRING,   // SD file transfer (handshake import)
    ERROR           // Backoff retry (exponential: 5/10/20/30s)
};

enum class C5Op : uint8_t {
    NONE,
    SCAN,
    HANDSHAKE,
    SAE_OVERFLOW,
    CHANNEL_VIEW,
    PACKET_MONITOR,
    SNIFFER_DOG,
    BLACKOUT,
    IMPORT_HANDSHAKES
};

enum class HandshakeResult : uint8_t {
    IDLE,
    IN_PROGRESS,
    CAPTURED,
    FAILED
};

// Internal scan cache entry (maps to JanOS CSV output)
struct C5ScanEntry {
    uint8_t  bssid[6];
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;      // wifi_auth_mode_t mapped from JanOS string
    uint8_t  c5Index;       // 1-based index for select_networks command
};

// Channel utilization counts from channel_view
struct C5ChannelCounts {
    uint16_t ch24[14];      // channels 1-14 (2.4GHz)
    uint16_t ch5[25];       // channels 36,40,...,165 (5GHz)
    bool valid;
};

// Capabilities/version probe result for the connected C5 firmware.
// Filled opportunistically; valid=false means "unknown" (assume minimal feature set).
struct C5Capabilities {
    bool    valid;
    bool    hasPorkCaps;    // responds to pork_caps
    bool    hasFileGet;     // supports file_get (base64 framed)
    uint8_t proto;          // protocol version for pork_caps/file_get framing
    char    fw[24];         // short firmware identifier/tag (best-effort)
};

namespace JanusHog {

// ============================================================================
// Lifecycle (called from main.cpp, like GPS)
// ============================================================================

void init();            // After Config load, before modes
void update();          // Every main loop iteration. Non-blocking.
void shutdown();        // Send stop, release UART, resume GPS.
void reinit();          // Shutdown + re-init from current Config (settings change)

// ============================================================================
// State Queries
// ============================================================================

C5State getState();
C5Op    getCurrentOp();
bool    isConnected();  // CONNECTED or any active state
bool    isEnabled();    // Config::c5().enabled
bool    isBusy();       // Any active op (scan/attack/monitor/transfer)
bool    isReady();      // CONNECTED + not busy (safe for new commands)
const C5Capabilities& getCapabilities();
bool    getTransferProgress(uint32_t* outBytes, uint32_t* outTotal);

// ============================================================================
// Active Commands (called by modes)
// ============================================================================

bool requestScan();                             // Scan 2.4+5GHz on C5
bool requestHandshake(const uint8_t* bssid);    // Full sequence: stop->scan->select->attack
bool requestSaeOverflow(const uint8_t* bssid);
bool requestChannelView();                      // Continuous channel utilization
bool requestPacketMonitor(uint8_t channel);     // PPS on specific channel
bool requestImportNewestHandshake();            // Pull newest C5 /sdcard/lab/handshakes/*.pcap to Porkchop SD
void requestStop();                             // Stop current C5 operation

// ============================================================================
// Result Polling (checked by modes in their update())
// ============================================================================

HandshakeResult getHandshakeResult();
void clearHandshakeResult();

const C5ChannelCounts& getChannelCounts();
uint32_t getPacketsPerSecond();

// ============================================================================
// Scan Cache Access (for command sequencer BSSID lookup)
// ============================================================================

uint8_t getScanCount();
const C5ScanEntry* getScanEntry(uint8_t index);

// ============================================================================
// C5 GPS Forwarding (start_gps_raw passthrough)
// ============================================================================

bool    hasC5GPSFix();          // True when C5 is forwarding valid GPS data
GPSData getC5GPSData();         // Latest GPS fix from C5 (zeroed if no fix)
bool    isGPSForwarding();      // True when start_gps_raw is active

// ============================================================================
// Status String (for UI overlay)
// ============================================================================

void getStatusString(char* buf, uint8_t len);

} // namespace JanusHog
