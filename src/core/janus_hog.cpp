// JanusHog - ESP32-C5 Coprocessor Service (JANUS HOG)
// UART bridge to JanOS/projectZero running on the Janus Hog (JanusHog) board.
// Non-blocking, zero dynamic allocations, UART drained in bounded chunks
// (idle small; transfer mode larger to avoid RX overflow).

#include "janus_hog.h"
#include "c5_uart_markers.h"
#include "config.h"
#include "sd_layout.h"
#include "xp.h"
#include "heap_gates.h"
#include "network_recon.h"
#include "../gps/gps.h"
#include "../gps/gps_quality.h"
#include <TinyGPSPlus.h>
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../ui/display.h"
#include <SD.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <ctype.h>

// NOTE: Per user request, JANUS HOG uses SERIAL logging only (no SDLog).
#define C5_LOGF(fmt, ...) Serial.printf("[C5] " fmt "\n", ##__VA_ARGS__)

// ============================================================================
// Internal State
// ============================================================================

// Command sequencer steps (multi-step JanOS operations)
enum class SeqStep : uint8_t {
    IDLE,
    STOPPING,           // Sent "stop", waiting for quiet
    SCAN_REQUESTED,     // Sent "scan_networks"
    SCAN_SHOW_RESULTS,  // Sent "show_scan_results"
    TARGET_SELECTING,   // Matching BSSID in C5 scan results
    SAE_SELECT_SENT,    // Sent "select_networks", waiting for confirmation
    ATTACK_SENT,        // Sent "start_handshake <idx>"
    ATTACK_MONITORING,  // Parsing progress
    CHANNEL_VIEW_ACTIVE,
    PACKET_MONITOR_ACTIVE,
    IMPORT_LISTING,     // list_dir in progress (tracking newest .pcap)
    IMPORT_FILE_GET,    // file_get requested; waiting for start/chunks/end
    DONE
};

// All static state — zero heap allocations
static C5State          state = C5State::OFF;
static C5Op             currentOp = C5Op::NONE;
static SeqStep          seqStep = SeqStep::IDLE;
static HandshakeResult  hsResult = HandshakeResult::IDLE;

// UART line buffer
static char     lineBuf[1024];  // JanOS line buffer is 1024B (see docs/C5_UART_INTEGRATION_SPEC.txt)
static uint16_t linePos = 0;
static bool     lineOverflow = false;  // Drop oversize lines safely until newline

// Scan cache
static constexpr uint8_t SCAN_CACHE_MAX = 64;
// Double-buffered: keep last completed scan visible during an active scan.
// This prevents Spectrum's 5GHz overlay from flickering to "no data" between scans.
static C5ScanEntry  scanCacheA[SCAN_CACHE_MAX];
static C5ScanEntry  scanCacheB[SCAN_CACHE_MAX];
static C5ScanEntry* scanCacheActive = scanCacheA;
static C5ScanEntry* scanCacheWork = scanCacheB;
static uint8_t      scanCountActive = 0;
static uint8_t      scanCountWork = 0;
static bool         scanWork5GHzXpAwarded = false;  // Once per scan batch
static uint32_t     lastScanCompleteMs = 0;

// Channel counts
static C5ChannelCounts channelCounts;
static bool            parsingChannelView = false;
static bool            parsing5GHz = false;

// Packet monitor
static uint32_t pktPerSecond = 0;
static uint8_t  pktMonChannel = 1;

// Target BSSID for sequenced operations
static uint8_t targetBssid[6];
static int8_t  targetC5Index = -1;     // 1-based C5 scan index

// Timing
static uint32_t lastPingMs = 0;
static uint32_t lastRxMs = 0;
static uint32_t stateEnteredMs = 0;
static uint32_t seqStepEnteredMs = 0;

// Capabilities probe (best-effort)
static C5Capabilities caps;
static bool           capsProbePending = false;
static bool           capsProbeSent = false;
static uint32_t       capsProbeSentMs = 0;

// Handshake import (C5 SD -> Porkchop SD) state
static bool     importPendingAuto = false;
static uint32_t lastHandshakeCapturedMs = 0;
static uint8_t  lastHandshakeBssid[6] = {0};
static char     lastHandshakeSsid[33] = {0};

static bool     xferActive = false;
static bool     xferAwaitingStart = false;
static uint32_t xferTotalBytes = 0;
static uint32_t xferWrittenBytes = 0;
static uint32_t xferCrc = 0xFFFFFFFFu;
static uint32_t xferLastProgressMs = 0;
static File     xferFile;
static char     xferRemotePath[160] = {0};
static char     xferLocalPath[96] = {0};
static uint32_t xferExpectedOffset = 0;
static bool     reconPausedForImport = false;

// Import listing: track newest handshake .pcap by timestamp suffix.
static bool     importListingActive = false;
static uint64_t importBestTimestamp = 0;
static char     importBestFilename[96] = {0};  // base filename only (no dir)

// Connection lifecycle
static uint8_t  pingAttempts = 0;
static uint8_t  errorRetries = 0;
static uint32_t errorBackoffMs = 5000;
static bool     c5XpAwarded = false;    // Once per session
static bool     gpsPaused = false;

// Auto-scan timer (uses C5Config::scanIntervalMs, 0 = disabled)
static uint32_t lastAutoScanMs = 0;
static bool     firstScanDone = false;

// GPS forwarding from C5 (start_gps_raw NMEA passthrough)
static TinyGPSPlus c5Gps;
static GPSData     c5GpsData = {0};
static bool        c5GpsForwarding = false;      // confirmed active (NMEA received)
static bool        c5GpsAwaitingStart = false;    // start_gps_raw sent, waiting for first NMEA
static bool        c5GpsPendingStart = false;     // queued, waiting for CONNECTED+idle
static bool        c5GpsConfigSent = false;       // gps_set sent, waiting before start_gps_raw
static uint32_t    c5GpsConfigSentMs = 0;         // when gps_set was sent
static uint32_t    c5GpsLastFixMs = 0;
static uint32_t    c5GpsAwaitingSentMs = 0;       // when start_gps_raw was sent (for timeout)
static GPSData     c5CoastSnapshot = {0};         // last known good C5 GPS position for coasting
static uint32_t    c5CoastStartMs = 0;
static bool        c5CoastActive = false;

static constexpr uint32_t GPS_AWAITING_TIMEOUT_MS = 10000;   // re-queue if no NMEA in 10s
static constexpr uint32_t GPS_CONFIG_SETTLE_MS    = 200;     // let gps_set reconfigure before start

// Protocol timing constants
static constexpr uint32_t PING_INTERVAL_MS      = 2000;
static constexpr uint32_t DETECT_TIMEOUT_MS     = 30000;
static constexpr uint32_t SCAN_TIMEOUT_MS       = 20000;
static constexpr uint32_t ATTACK_TIMEOUT_MS     = 60000;
static constexpr uint32_t STOP_QUIET_MS         = 500;
static constexpr uint32_t HEARTBEAT_STALE_MS    = 15000;
static constexpr uint32_t HEARTBEAT_PROBE_MS    = 3000;
static constexpr uint16_t MAX_READ_PER_UPDATE_IDLE = 128;
static constexpr uint16_t MAX_READ_PER_UPDATE_XFER = 768;
static constexpr uint32_t MAX_ERROR_BACKOFF_MS  = 30000;
static constexpr uint32_t CAPS_PROBE_TIMEOUT_MS = 1500;
static constexpr uint32_t XFER_STALL_TIMEOUT_MS = 10000;

// ============================================================================
// Forward Declarations
// ============================================================================

static void sendCommand(const char* cmd);
static void processLine(const char* line);
static void parseScanCSV(const char* line);
static void parseChannelViewLine(const char* line);
static void setState(C5State newState);
static void setSeqStep(SeqStep step);
static void advanceSequencer();
static void injectScanIntoRecon();
static wifi_auth_mode_t mapAuthString(const char* auth);
static int findBssidInCache(const uint8_t* bssid);
static bool sanitizeSsidLikeC5(const char* in, char* out, size_t outLen);
static bool deriveNamingFromC5Filename(const char* filename, uint8_t outBssid[6], char outSsid[33]);
static void abortTransfer(const char* reason);
static bool startTransferForRemotePath(const char* remotePath, const uint8_t bssidForName[6], const char* ssidForName);
static void sendFileGetForRemotePath(const char* remotePath);
static void enterDisconnected(const char* reason);
static void feedNmeaLine(const char* line);
static void updateC5GpsData();

// ============================================================================
// 5GHz Channel Mapping
// ============================================================================

static const uint8_t CH5_MAP[] = {
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128,
    132, 136, 140, 144, 149, 153, 157, 161, 165
};
static constexpr uint8_t CH5_COUNT = sizeof(CH5_MAP);

static int8_t ch5ToIndex(uint8_t channel) {
    for (uint8_t i = 0; i < CH5_COUNT; i++) {
        if (CH5_MAP[i] == channel) return i;
    }
    return -1;
}

// ============================================================================
// Lifecycle
// ============================================================================

void JanusHog::init() {
    state = C5State::OFF;
    currentOp = C5Op::NONE;
    seqStep = SeqStep::IDLE;
    hsResult = HandshakeResult::IDLE;
    linePos = 0;
    scanCountActive = 0;
    scanCountWork = 0;
    scanWork5GHzXpAwarded = false;
    pingAttempts = 0;
    errorRetries = 0;
    errorBackoffMs = 5000;
    c5XpAwarded = false;
    gpsPaused = false;
    c5GpsAwaitingStart = false;
    c5GpsAwaitingSentMs = 0;
    c5GpsConfigSent = false;
    c5GpsConfigSentMs = 0;
    pktPerSecond = 0;
    memset(scanCacheA, 0, sizeof(scanCacheA));
    memset(scanCacheB, 0, sizeof(scanCacheB));
    scanCacheActive = scanCacheA;
    scanCacheWork = scanCacheB;
    memset(&channelCounts, 0, sizeof(channelCounts));
    memset(lineBuf, 0, sizeof(lineBuf));
    memset(targetBssid, 0, sizeof(targetBssid));
    targetC5Index = -1;
    parsingChannelView = false;
    parsing5GHz = false;
    lastAutoScanMs = 0;
    firstScanDone = false;
    lineOverflow = false;
    lastScanCompleteMs = 0;

    memset(&caps, 0, sizeof(caps));
    caps.valid = false;
    capsProbePending = false;
    capsProbeSent = false;
    capsProbeSentMs = 0;

    importPendingAuto = false;
    lastHandshakeCapturedMs = 0;
    memset(lastHandshakeBssid, 0, sizeof(lastHandshakeBssid));
    memset(lastHandshakeSsid, 0, sizeof(lastHandshakeSsid));

    importListingActive = false;
    importBestTimestamp = 0;
    memset(importBestFilename, 0, sizeof(importBestFilename));

    xferActive = false;
    xferAwaitingStart = false;
    xferTotalBytes = 0;
    xferWrittenBytes = 0;
    xferCrc = 0xFFFFFFFFu;
    xferLastProgressMs = 0;
    if (xferFile) {
        xferFile.close();
    }
    xferRemotePath[0] = '\0';
    xferLocalPath[0] = '\0';
    xferExpectedOffset = 0;
    reconPausedForImport = false;

    if (!Config::c5().enabled) {
        return;
    }

    // Heap gate check
    auto gate = HeapGates::checkGate(20000, 15000);
    if (!HeapGates::canMeet(gate, nullptr, 0)) {
        C5_LOGF("Heap too low for C5 init");
        return;
    }

    // Validate UART pins before touching hardware — bad pins cause WDT hang
    uint8_t txPin = Config::c5().uartTxPin;
    uint8_t rxPin = Config::c5().uartRxPin;
    if (txPin > 48 || rxPin > 48 || txPin == rxPin) {
        C5_LOGF("Invalid UART pins TX=%d RX=%d, aborting", txPin, rxPin);
        return;
    }

    // Handle GPS pin conflict — check any overlap, not just Grove defaults
    GPSConfig gpsCfg = Config::gps();

    if (gpsCfg.enabled &&
        (txPin == gpsCfg.txPin || txPin == gpsCfg.rxPin ||
         rxPin == gpsCfg.txPin || rxPin == gpsCfg.rxPin)) {
        GPS::sleep();
        gpsPaused = true;
        C5_LOGF("GPS sleeping - C5 pins overlap GPS (TX%d/RX%d vs G%d/G%d)",
                   txPin, rxPin, gpsCfg.txPin, gpsCfg.rxPin);
    }

    Serial1.begin(Config::c5().baudRate, SERIAL_8N1, rxPin, txPin);

    setState(C5State::DISCONNECTED);
    lastPingMs = millis();
    sendCommand("ping");

    C5_LOGF("Init (RX=%d TX=%d baud=%lu)",
               rxPin, txPin, Config::c5().baudRate);
}

void JanusHog::update() {
    if (state == C5State::OFF) return;

    uint32_t now = millis();

    // --- UART RX: drain bytes into lineBuf, process complete lines ---
    uint16_t bytesRead = 0;
    uint16_t maxRead = (state == C5State::TRANSFERRING) ? MAX_READ_PER_UPDATE_XFER : MAX_READ_PER_UPDATE_IDLE;
    while (Serial1.available() && bytesRead < maxRead) {
        char c = (char)Serial1.read();
        bytesRead++;
        lastRxMs = now;

        if (c == '\n' || c == '\r') {
            if (linePos > 0 && !lineOverflow) {
                lineBuf[linePos] = '\0';
                processLine(lineBuf);
            }
            linePos = 0;
            lineOverflow = false;
        } else if (linePos < sizeof(lineBuf) - 1) {
            if (!lineOverflow) {
                lineBuf[linePos++] = c;
            }
        } else {
            // Oversize line: stop buffering until newline to avoid truncated parses.
            lineOverflow = true;
        }

        if (bytesRead % 32 == 0) yield();
    }

    // --- Connection state machine ---
    switch (state) {
        case C5State::DISCONNECTED:
            if (now - lastPingMs >= PING_INTERVAL_MS) {
                sendCommand("ping");
                lastPingMs = now;
                pingAttempts++;
            }
            if (now - stateEnteredMs >= DETECT_TIMEOUT_MS) {
                setState(C5State::ERROR);
                C5_LOGF("Detection timeout after %d pings", pingAttempts);
            }
            break;

        case C5State::CONNECTED:
            // Heartbeat: if no data for HEARTBEAT_STALE_MS, probe
            if (now - lastRxMs >= HEARTBEAT_STALE_MS) {
                // Throttle probes: without this, we'd spam "ping" every loop iteration.
                if (now - lastPingMs >= HEARTBEAT_PROBE_MS) {
                    sendCommand("ping");
                    lastPingMs = now;
                }
                if (now - lastRxMs >= HEARTBEAT_STALE_MS + HEARTBEAT_PROBE_MS) {
                    C5_LOGF("Heartbeat lost, reconnecting");
                    enterDisconnected("C5 HEARTBEAT LOST");
                }
            }
            if (state != C5State::CONNECTED) break;

            // Capabilities probe (best-effort, one-shot per connect)
            if (capsProbePending && !capsProbeSent && currentOp == C5Op::NONE) {
                sendCommand("pork_caps");
                capsProbeSent = true;
                capsProbeSentMs = now;
            }
            if (capsProbeSent && !caps.valid && (now - capsProbeSentMs) >= CAPS_PROBE_TIMEOUT_MS) {
                // No response (older firmware). Keep caps.valid=false and stop probing.
                capsProbePending = false;
            }

            // GPS forwarding phase 1: stop stale reader, configure module type
            if (c5GpsPendingStart && !c5GpsConfigSent && currentOp == C5Op::NONE) {
                sendCommand("stop");              // kill any pre-existing GPS reader (wrong baud)
                sendCommand("gps_set m5stack");  // configure m5stack GPS @ 115200
                c5GpsConfigSent = true;
                c5GpsConfigSentMs = now;
                c5GpsPendingStart = false;
                Display::notify(NoticeKind::STATUS, "C5 GPS: CFG M5STACK", 1500, NoticeChannel::TOP_BAR);
                C5_LOGF("GPS module set to m5stack");
            }

            // GPS forwarding phase 2: start raw NMEA after config settles
            if (c5GpsConfigSent && !c5GpsAwaitingStart &&
                (now - c5GpsConfigSentMs >= GPS_CONFIG_SETTLE_MS)) {
                sendCommand("start_gps_raw");
                c5GpsAwaitingStart = true;
                c5GpsAwaitingSentMs = now;
                c5GpsConfigSent = false;
                Display::notify(NoticeKind::STATUS, "C5 GPS: AWAITING NMEA", 2000, NoticeChannel::TOP_BAR);
                C5_LOGF("GPS forwarding requested (awaiting NMEA)");
            }

            // GPS awaiting timeout: no NMEA received after start_gps_raw → re-queue
            if (c5GpsAwaitingStart && !c5GpsForwarding &&
                (now - c5GpsAwaitingSentMs >= GPS_AWAITING_TIMEOUT_MS)) {
                c5GpsAwaitingStart = false;
                c5GpsPendingStart = true;  // retry
                Display::notify(NoticeKind::WARNING, "C5 GPS: NO NMEA, RETRY", 2000, NoticeChannel::TOP_BAR);
                C5_LOGF("GPS forwarding timeout (no NMEA), will retry");
            }

            // Auto-import: after a 5GHz handshake capture, pull the newest C5 handshake to local SD.
            if (importPendingAuto && currentOp == C5Op::NONE) {
                if (caps.valid) {
                    importPendingAuto = false;  // one-shot
                    if (caps.hasFileGet) {
                        (void)requestImportNewestHandshake();
                    } else {
                        // Don't spam on older firmware; user can still import manually after flashing.
                        C5_LOGF("Auto-import skipped (file_get unsupported)");
                    }
                } else {
                    // Caps probe still pending: wait rather than skipping.
                    if (!capsProbePending && capsProbeSent) {
                        importPendingAuto = false;
                        C5_LOGF("Auto-import skipped (caps unknown)");
                    }
                }
            }

            // Auto-scan timer: periodic 5GHz network discovery
            // Defer while GPS forwarding is establishing — C5 can't scan and
            // forward GPS simultaneously ("already running" conflict).
            {
                uint16_t interval = Config::c5().scanIntervalMs;
                if (interval > 0 && currentOp == C5Op::NONE && !c5GpsAwaitingStart && !c5GpsConfigSent) {
                    if (!firstScanDone || (now - lastAutoScanMs >= interval)) {
                        lastAutoScanMs = now;
                        firstScanDone = true;
                        requestScan();
                    }
                }
            }
            break;

        case C5State::TRANSFERRING:
            // Stall detection: if transfer/listing stops making progress, abort.
            if ((currentOp == C5Op::IMPORT_HANDSHAKES) &&
                (xferAwaitingStart || xferActive || importListingActive)) {
                uint32_t lastProgress = xferLastProgressMs ? xferLastProgressMs : stateEnteredMs;
                if (now - lastProgress >= XFER_STALL_TIMEOUT_MS) {
                    abortTransfer("XFER TIMEOUT");
                    break;
                }
            }
            advanceSequencer();
            break;

        case C5State::SCANNING: {
            // Handle scan timeout (but not after we already got the completion marker)
            bool scanInProgress = (seqStep == SeqStep::STOPPING ||
                                   seqStep == SeqStep::SCAN_REQUESTED ||
                                   seqStep == SeqStep::SCAN_SHOW_RESULTS);
            if (scanInProgress && (now - stateEnteredMs >= SCAN_TIMEOUT_MS)) {
                C5_LOGF("Scan timeout, %d networks parsed", scanCountWork);

                // If we have partial results, finalize them so UI/recon stays consistent.
                if (scanCountWork > 0) {
                    // Swap: make the (partial) work cache the active cache.
                    C5ScanEntry* oldActive = scanCacheActive;
                    scanCacheActive = scanCacheWork;
                    scanCacheWork = oldActive;
                    scanCountActive = scanCountWork;
                    scanCountWork = 0;
                    scanWork5GHzXpAwarded = false;
                    lastScanCompleteMs = now;
                    injectScanIntoRecon();

                    // If this scan was servicing a handshake, still attempt target selection.
                    if (currentOp == C5Op::HANDSHAKE || currentOp == C5Op::SAE_OVERFLOW) {
                        setSeqStep(SeqStep::TARGET_SELECTING);
                        break;
                    }
                }

                if (currentOp == C5Op::HANDSHAKE) {
                    // Scan timed out while trying to service a handshake request.
                    hsResult = HandshakeResult::FAILED;
                }
                setState(C5State::CONNECTED);
                currentOp = C5Op::NONE;
                setSeqStep(SeqStep::IDLE);
            }
            // Sequencer may need to advance
            advanceSequencer();
            break;
        }

        case C5State::ATTACKING:
            if (now - stateEnteredMs >= ATTACK_TIMEOUT_MS) {
                C5_LOGF("Attack timeout");
                hsResult = HandshakeResult::FAILED;
                sendCommand("stop");
                setState(C5State::CONNECTED);
                currentOp = C5Op::NONE;
                setSeqStep(SeqStep::IDLE);
            }
            advanceSequencer();
            break;

        case C5State::MONITORING:
            // channel_view/packet_monitor run until requestStop()
            // Heartbeat still applies
            if (now - lastRxMs >= HEARTBEAT_STALE_MS + HEARTBEAT_PROBE_MS) {
                C5_LOGF("Monitor heartbeat lost");
                enterDisconnected("C5 MONITOR LOST");
            }
            if (state != C5State::MONITORING) break;
            // Allow STOPPING -> next monitor transition even if C5 doesn't emit stop markers.
            advanceSequencer();
            break;

        case C5State::ERROR:
            if (now - stateEnteredMs >= errorBackoffMs) {
                errorRetries++;
                errorBackoffMs = errorBackoffMs * 2;
                if (errorBackoffMs > MAX_ERROR_BACKOFF_MS) {
                    errorBackoffMs = MAX_ERROR_BACKOFF_MS;
                }
                setState(C5State::DISCONNECTED);
                pingAttempts = 0;
            }
            break;

        default:
            break;
    }
}

void JanusHog::shutdown() {
    if (state == C5State::OFF) return;

    sendCommand("stop");
    delay(50);

    // If a file transfer is in progress, close handles and remove partial output.
    abortTransfer(nullptr);
    Serial1.end();

    // Stop GPS forwarding before waking local GPS
    c5GpsForwarding = false;
    c5GpsAwaitingStart = false;
    c5GpsPendingStart = false;
    c5GpsConfigSent = false;
    c5GpsConfigSentMs = 0;
    c5GpsAwaitingSentMs = 0;
    memset(&c5GpsData, 0, sizeof(c5GpsData));
    c5CoastActive = false;
    c5CoastSnapshot.valid = false;

    if (gpsPaused) {
        GPS::wake();
        gpsPaused = false;
    }

    state = C5State::OFF;
    currentOp = C5Op::NONE;
    seqStep = SeqStep::IDLE;
    scanCountActive = 0;
    scanCountWork = 0;
    scanWork5GHzXpAwarded = false;
    lastScanCompleteMs = 0;

    C5_LOGF("Shutdown");
}

void JanusHog::reinit() {
    shutdown();
    delay(50);
    init();
}

// ============================================================================
// State Queries
// ============================================================================

C5State JanusHog::getState() { return state; }
C5Op JanusHog::getCurrentOp() { return currentOp; }
bool JanusHog::isEnabled() { return Config::c5().enabled; }

bool JanusHog::isConnected() {
    return state == C5State::CONNECTED ||
           state == C5State::SCANNING ||
           state == C5State::ATTACKING ||
           state == C5State::MONITORING ||
           state == C5State::TRANSFERRING;
}

bool JanusHog::isBusy() {
    return (currentOp != C5Op::NONE) ||
           (state == C5State::SCANNING) ||
           (state == C5State::ATTACKING) ||
           (state == C5State::MONITORING) ||
           (state == C5State::TRANSFERRING);
}

bool JanusHog::isReady() {
    return (state == C5State::CONNECTED) && (currentOp == C5Op::NONE);
}

const C5Capabilities& JanusHog::getCapabilities() {
    return caps;
}

bool JanusHog::getTransferProgress(uint32_t* outBytes, uint32_t* outTotal) {
    if (!outBytes || !outTotal) return false;
    if (!xferActive && !xferAwaitingStart) return false;
    *outBytes = xferWrittenBytes;
    *outTotal = xferTotalBytes;
    return true;
}

// ============================================================================
// Active Commands
// ============================================================================

bool JanusHog::requestScan() {
    if (!isConnected()) return false;
    if (currentOp == C5Op::IMPORT_HANDSHAKES) return false;

    scanCountWork = 0;
    scanWork5GHzXpAwarded = false;
    lastAutoScanMs = millis();  // Reset auto-scan timer on manual scan too
    currentOp = C5Op::SCAN;

    if (state != C5State::CONNECTED) {
        // Something else is running — go through STOPPING sequence
        setState(C5State::SCANNING);
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
    } else {
        setState(C5State::SCANNING);
        setSeqStep(SeqStep::SCAN_REQUESTED);
        sendCommand("scan_networks");
    }
    C5_LOGF("Scan requested");
    return true;
}

bool JanusHog::requestHandshake(const uint8_t* bssid) {
    if (!isConnected()) return false;
    if (currentOp == C5Op::IMPORT_HANDSHAKES) return false;

    memcpy(targetBssid, bssid, 6);
    targetC5Index = -1;
    hsResult = HandshakeResult::IN_PROGRESS;
    currentOp = C5Op::HANDSHAKE;

    // Check if target is in current scan cache
    int idx = findBssidInCache(bssid);
    if (idx >= 0) {
        // Direct attack — skip rescan
        targetC5Index = scanCacheActive[idx].c5Index;
        setState(C5State::ATTACKING);
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
        C5_LOGF("Handshake request (cached idx=%d)", targetC5Index);
    } else {
        // Need to rescan to find BSSID
        scanCountWork = 0;
        scanWork5GHzXpAwarded = false;
        setState(C5State::SCANNING);
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
        C5_LOGF("Handshake request (need rescan)");
    }
    return true;
}

bool JanusHog::requestSaeOverflow(const uint8_t* bssid) {
    if (!isConnected()) return false;
    if (currentOp == C5Op::IMPORT_HANDSHAKES) return false;

    memcpy(targetBssid, bssid, 6);
    targetC5Index = -1;
    currentOp = C5Op::SAE_OVERFLOW;

    int idx = findBssidInCache(bssid);
    if (idx >= 0) {
        targetC5Index = scanCacheActive[idx].c5Index;
        setState(C5State::ATTACKING);
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
    } else {
        scanCountWork = 0;
        scanWork5GHzXpAwarded = false;
        setState(C5State::SCANNING);
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
    }
    return true;
}

bool JanusHog::requestChannelView() {
    if (!isConnected()) return false;
    if (currentOp == C5Op::IMPORT_HANDSHAKES) return false;

    currentOp = C5Op::CHANNEL_VIEW;
    memset(&channelCounts, 0, sizeof(channelCounts));
    parsingChannelView = false;
    parsing5GHz = false;

    if (state != C5State::CONNECTED) {
        // Something else is running — go through STOPPING sequence
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
    } else {
        setState(C5State::MONITORING);
        setSeqStep(SeqStep::CHANNEL_VIEW_ACTIVE);
        sendCommand("channel_view");
    }
    C5_LOGF("Channel view requested");
    return true;
}

bool JanusHog::requestPacketMonitor(uint8_t channel) {
    if (!isConnected()) return false;
    if (currentOp == C5Op::IMPORT_HANDSHAKES) return false;

    currentOp = C5Op::PACKET_MONITOR;
    pktMonChannel = channel;
    pktPerSecond = 0;

    if (state != C5State::CONNECTED) {
        // Something else is running — go through STOPPING sequence
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
    } else {
        setState(C5State::MONITORING);
        setSeqStep(SeqStep::PACKET_MONITOR_ACTIVE);
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "packet_monitor %d", (int)pktMonChannel);
        sendCommand(cmd);
    }
    C5_LOGF("Packet monitor ch%d requested", channel);
    return true;
}

bool JanusHog::requestImportNewestHandshake() {
    if (!isConnected()) return false;
    if (currentOp == C5Op::IMPORT_HANDSHAKES) return false;

    // We need a local SD card to store imports.
    if (!Config::isSDAvailable()) {
        Display::notify(NoticeKind::STATUS, "NO SD FOR IMPORT", 2000, NoticeChannel::TOP_BAR);
        return false;
    }

    // Avoid disrupting an active attack sequence.
    if (state == C5State::ATTACKING || currentOp == C5Op::HANDSHAKE || currentOp == C5Op::SAE_OVERFLOW) {
        Display::notify(NoticeKind::STATUS, "C5 BUSY (ATTACK)", 2000, NoticeChannel::TOP_BAR);
        return false;
    }

    // If caps are known and file_get is missing, fail fast with a clear hint.
    if (caps.valid && !caps.hasFileGet) {
        Display::notify(NoticeKind::STATUS, "C5 FW NO FILE_GET", 2500, NoticeChannel::TOP_BAR);
        return false;
    }

    // Reduce CPU pressure during transfer; resume once import completes/aborts.
    reconPausedForImport = false;
    if (NetworkRecon::isRunning() && !NetworkRecon::isPaused()) {
        NetworkRecon::pause();
        reconPausedForImport = true;
    }

    // Reset listing state
    importListingActive = false;
    importBestTimestamp = 0;
    importBestFilename[0] = '\0';

    currentOp = C5Op::IMPORT_HANDSHAKES;
    C5State prevState = state;
    setState(C5State::TRANSFERRING);

    if (prevState != C5State::CONNECTED) {
        setSeqStep(SeqStep::STOPPING);
        sendCommand("stop");
    } else {
        setSeqStep(SeqStep::IMPORT_LISTING);
        importListingActive = true;
        xferLastProgressMs = millis();
        sendCommand("list_dir lab/handshakes");
    }

    C5_LOGF("Import requested (newest handshake)");
    return true;
}

void JanusHog::requestStop() {
    if (state == C5State::OFF || state == C5State::DISCONNECTED) return;
    sendCommand("stop");
    if (currentOp == C5Op::IMPORT_HANDSHAKES) {
        abortTransfer("C5 STOP");
    }
    currentOp = C5Op::NONE;
    seqStep = SeqStep::IDLE;
    parsingChannelView = false;
    if (state != C5State::ERROR) {
        setState(C5State::CONNECTED);
    }
}

// ============================================================================
// Result Polling
// ============================================================================

HandshakeResult JanusHog::getHandshakeResult() { return hsResult; }
void JanusHog::clearHandshakeResult() { hsResult = HandshakeResult::IDLE; }
const C5ChannelCounts& JanusHog::getChannelCounts() { return channelCounts; }
uint32_t JanusHog::getPacketsPerSecond() { return pktPerSecond; }
uint8_t JanusHog::getScanCount() { return scanCountActive; }

const C5ScanEntry* JanusHog::getScanEntry(uint8_t index) {
    if (index >= scanCountActive) return nullptr;
    return &scanCacheActive[index];
}

// ============================================================================
// Status String
// ============================================================================

void JanusHog::getStatusString(char* buf, uint8_t len) {
    const char* stateStr = "OFF";
    switch (state) {
        case C5State::OFF:          stateStr = "OFF"; break;
        case C5State::DISCONNECTED: stateStr = "SEARCHING"; break;
        case C5State::CONNECTED:    stateStr = "READY"; break;
        case C5State::SCANNING:     stateStr = "SCANNING"; break;
        case C5State::ATTACKING:    stateStr = "ATTACKING"; break;
        case C5State::MONITORING:   stateStr = "MONITORING"; break;
        case C5State::TRANSFERRING: stateStr = "XFER"; break;
        case C5State::ERROR:        stateStr = "ERROR"; break;
    }
    snprintf(buf, len, "C5:%s", stateStr);
}

// ============================================================================
// Internal: UART TX
// ============================================================================

static void sendCommand(const char* cmd) {
    if (!cmd || cmd[0] == '\0') return;
    Serial1.print(cmd);
    Serial1.print('\n');
    Serial1.flush();

    // Avoid spamming Serial with periodic keep-alives.
    if (strcmp(cmd, "ping") != 0) {
        C5_LOGF("TX> %s", cmd);
    }
}

// ============================================================================
// Internal: State Helpers
// ============================================================================

static void setState(C5State newState) {
    state = newState;
    stateEnteredMs = millis();
    if (newState == C5State::DISCONNECTED) {
        pingAttempts = 0;
    }
    if (newState == C5State::CONNECTED) {
        errorRetries = 0;
        errorBackoffMs = 5000;
    }
}

static void setSeqStep(SeqStep step) {
    seqStep = step;
    seqStepEnteredMs = millis();
}

static void enterDisconnected(const char* reason) {
    if (state == C5State::OFF || state == C5State::DISCONNECTED) return;

    // Move to DISCONNECTED first so abortTransfer() won't bounce state back to CONNECTED.
    setState(C5State::DISCONNECTED);

    // If an import is in progress, close handles and remove partial output.
    if (currentOp == C5Op::IMPORT_HANDSHAKES) {
        abortTransfer((reason && reason[0]) ? reason : "C5 LINK LOST");
    }

    // Any in-flight op is invalid once the UART link drops.
    if (currentOp == C5Op::HANDSHAKE && hsResult == HandshakeResult::IN_PROGRESS) {
        hsResult = HandshakeResult::FAILED;
    }

    currentOp = C5Op::NONE;
    setSeqStep(SeqStep::IDLE);
    parsingChannelView = false;
    parsing5GHz = false;
    pktPerSecond = 0;

    // GPS forwarding lost — will re-start on next pong
    if (c5GpsForwarding || c5GpsAwaitingStart || c5GpsConfigSent) {
        c5GpsForwarding = false;
        c5GpsAwaitingStart = false;
        c5GpsConfigSent = false;
        c5GpsPendingStart = true;  // re-queue for reconnect
        // Clear cached fix data and reset parser so reconnect starts fresh.
        // Without this, a brief disconnect (<30s) could expose stale coordinates
        // for ~1-2s until new NMEA overwrites the TinyGPSPlus state.
        memset(&c5GpsData, 0, sizeof(c5GpsData));
        c5CoastActive = false;
        c5CoastSnapshot.valid = false;
        c5Gps = TinyGPSPlus();
    }
}

// ============================================================================
// Internal: Line Processing
// ============================================================================

static void processLine(const char* line) {
    if (!line || line[0] == '\0') return;

    // --- NMEA sentences from C5 GPS forwarding (start_gps_raw) ---
    // JanOS may prefix NMEA with tags or whitespace, so scan for '$G' anywhere.
    // TinyGPSPlus validates checksums, so feeding a false positive is safe.
    {
        const char* nmea = strstr(line, "$G");
        if (nmea && (strncmp(nmea + 2, "P", 1) == 0 ||
                     strncmp(nmea + 2, "N", 1) == 0 ||
                     strncmp(nmea + 2, "L", 1) == 0 ||
                     strncmp(nmea + 2, "A", 1) == 0)) {
            feedNmeaLine(nmea);
            return;
        }
        // BeiDou: $BD
        if (!nmea) {
            nmea = strstr(line, "$BD");
            if (nmea) {
                feedNmeaLine(nmea);
                return;
            }
        }
    }

    // --- Capabilities probe response ---
    // Expected (patched) format: pork_caps|proto=<n>|file_get=<0|1>|fw=<tag>
    if (strncmp(line, "pork_caps|", 10) == 0) {
        memset(&caps, 0, sizeof(caps));
        caps.valid = true;
        caps.hasPorkCaps = true;
        caps.proto = 0;
        caps.hasFileGet = false;
        caps.fw[0] = '\0';

        const char* p = line + 10;
        while (p && *p) {
            const char* next = strchr(p, '|');
            size_t len = next ? (size_t)(next - p) : strlen(p);
            if (len > 0) {
                if (len >= 6 && strncmp(p, "proto=", 6) == 0) {
                    caps.proto = (uint8_t)atoi(p + 6);
                } else if (len >= 9 && strncmp(p, "file_get=", 9) == 0) {
                    caps.hasFileGet = atoi(p + 9) != 0;
                } else if (len >= 3 && strncmp(p, "fw=", 3) == 0) {
                    size_t copyLen = len - 3;
                    if (copyLen >= sizeof(caps.fw)) copyLen = sizeof(caps.fw) - 1;
                    memcpy(caps.fw, p + 3, copyLen);
                    caps.fw[copyLen] = '\0';
                }
            }
            p = next ? (next + 1) : nullptr;
        }

        capsProbePending = false;
        C5_LOGF("Caps: proto=%u file_get=%s fw=%s",
                (unsigned)caps.proto, caps.hasFileGet ? "yes" : "no", caps.fw[0] ? caps.fw : "?");
        return;
    }

    // --- File transfer framing (import) ---
    if (currentOp == C5Op::IMPORT_HANDSHAKES) {
        if (seqStep == SeqStep::IMPORT_FILE_GET && xferAwaitingStart) {
            if (strstr(line, "not found") != nullptr ||
                strstr(line, "Unknown command") != nullptr ||
                strstr(line, "Unrecognized") != nullptr) {
                abortTransfer("NO FILE_GET");
                return;
            }
        }

        // file_get_start|<size>|<path>
        if (strncmp(line, "file_get_start|", 15) == 0) {
            const char* p = line + 15;
            char* endp = nullptr;
            uint32_t size = (uint32_t)strtoul(p, &endp, 10);
            if (!endp || *endp != '|') {
                abortTransfer("BAD START");
                return;
            }
            const char* remotePath = endp + 1;

            xferTotalBytes = size;
            xferWrittenBytes = 0;
            xferExpectedOffset = 0;
            xferCrc = 0xFFFFFFFFu;
            xferLastProgressMs = millis();
            xferAwaitingStart = false;
            xferActive = true;

            // Open local file (truncate by delete+create)
            if (xferLocalPath[0] == '\0') {
                // Should have been set when we requested import.
                abortTransfer("NO LOCAL PATH");
                return;
            }
            if (SD.exists(xferLocalPath)) {
                SD.remove(xferLocalPath);
            }
            xferFile = SD.open(xferLocalPath, FILE_WRITE);
            if (!xferFile) {
                abortTransfer("SD OPEN FAIL");
                return;
            }

            C5_LOGF("XFER start: %lu bytes from %s", (unsigned long)size, remotePath);
            return;
        }

        // file_get_chunk|<offset>|<base64>
        if (strncmp(line, "file_get_chunk|", 15) == 0) {
            if (!xferActive || !xferFile) {
                abortTransfer("CHUNK NOFILE");
                return;
            }
            const char* p = line + 15;
            char* endp = nullptr;
            uint32_t offset = (uint32_t)strtoul(p, &endp, 10);
            if (!endp || *endp != '|') {
                abortTransfer("BAD CHUNK");
                return;
            }
            const char* b64 = endp + 1;

            if (offset != xferExpectedOffset) {
                abortTransfer("BAD OFFSET");
                return;
            }

            // Decode base64 chunk into stack buffer (chunk size is bounded by firmware).
            static uint8_t decoded[512];
            size_t outLen = 0;
            int rc = mbedtls_base64_decode(decoded, sizeof(decoded), &outLen,
                                           (const unsigned char*)b64, strlen(b64));
            if (rc != 0 || outLen == 0) {
                abortTransfer("B64 FAIL");
                return;
            }

            size_t written = xferFile.write(decoded, outLen);
            if (written != outLen) {
                abortTransfer("SD WRITE FAIL");
                return;
            }

            // CRC32 update (same polynomial as PigSync helper).
            for (size_t i = 0; i < outLen; i++) {
                xferCrc ^= decoded[i];
                for (int j = 0; j < 8; j++) {
                    xferCrc = (xferCrc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(xferCrc & 1u));
                }
            }

            xferWrittenBytes += (uint32_t)outLen;
            xferExpectedOffset += (uint32_t)outLen;
            xferLastProgressMs = millis();
            return;
        }

        // file_get_end|<size>|<crc32hex>
        if (strncmp(line, "file_get_end|", 13) == 0) {
            uint32_t size = 0;
            uint32_t remoteCrc = 0;
            bool hasRemoteCrc = false;

            const char* p = line + 13;
            char* endp = nullptr;
            size = (uint32_t)strtoul(p, &endp, 10);
            if (endp && *endp == '|') {
                remoteCrc = (uint32_t)strtoul(endp + 1, nullptr, 16);
                hasRemoteCrc = true;
            }

            if (xferFile) {
                xferFile.flush();
                xferFile.close();
            }

            uint32_t localCrc = ~xferCrc;
            bool sizeOk = (xferWrittenBytes == xferTotalBytes) && (size == 0 || xferWrittenBytes == size);
            bool crcOk = (!hasRemoteCrc) || (remoteCrc == localCrc);

            if (sizeOk && crcOk) {
                Display::notify(NoticeKind::STATUS, "C5 CAPTURE IMPORTED", 2500, NoticeChannel::TOP_BAR);
                C5_LOGF("XFER complete: %lu bytes crc32=%08lX", (unsigned long)xferWrittenBytes, (unsigned long)localCrc);
            } else {
                C5_LOGF("XFER verify fail: wrote=%lu exp=%lu endSize=%lu crcLocal=%08lX crcRemote=%08lX",
                        (unsigned long)xferWrittenBytes, (unsigned long)xferTotalBytes, (unsigned long)size,
                        (unsigned long)localCrc, (unsigned long)remoteCrc);
                abortTransfer("VERIFY FAIL");
                return;
            }

            // Cleanup and return to idle
            xferActive = false;
            xferAwaitingStart = false;
            xferTotalBytes = 0;
            xferWrittenBytes = 0;
            xferRemotePath[0] = '\0';
            xferLocalPath[0] = '\0';
            importListingActive = false;
            importBestTimestamp = 0;
            importBestFilename[0] = '\0';

            currentOp = C5Op::NONE;
            setSeqStep(SeqStep::IDLE);
            setState(C5State::CONNECTED);
            if (reconPausedForImport) {
                NetworkRecon::resume();
                reconPausedForImport = false;
            }
            return;
        }

        // file_get_error|<reason>
        if (strncmp(line, "file_get_error|", 15) == 0) {
            abortTransfer("REMOTE ERROR");
            return;
        }

        // list_dir parsing while importing newest handshake
        if (seqStep == SeqStep::IMPORT_LISTING && importListingActive) {
            xferLastProgressMs = millis();
            if (strncmp(line, "No files found", 13) == 0) {
                abortTransfer("NO FILES");
                return;
            }
            if (strncmp(line, "Found ", 6) == 0) {
                // Listing complete; kick off transfer for the newest .pcap we saw.
                importListingActive = false;
                if (importBestFilename[0] == '\0') {
                    abortTransfer("NO PCAPS");
                    return;
                }

                // Build remote path: lab/handshakes/<filename>
                snprintf(xferRemotePath, sizeof(xferRemotePath), "lab/handshakes/%s", importBestFilename);

                // Prefer naming from the most recent captured handshake, otherwise derive from filename.
                uint8_t bssidForName[6] = {0};
                char ssidForName[33] = {0};
                bool haveRecentBssid = (lastHandshakeCapturedMs != 0) && ((millis() - lastHandshakeCapturedMs) <= 120000);
                if (haveRecentBssid) {
                    memcpy(bssidForName, lastHandshakeBssid, 6);
                    strncpy(ssidForName, lastHandshakeSsid, sizeof(ssidForName) - 1);
                    ssidForName[sizeof(ssidForName) - 1] = '\0';
                }

                if (!startTransferForRemotePath(xferRemotePath, haveRecentBssid ? bssidForName : nullptr,
                                                haveRecentBssid ? ssidForName : nullptr)) {
                    abortTransfer("XFER START FAIL");
                }
                return;
            }

            // Parse file line: "<n> <filename>"
            if (isdigit((unsigned char)line[0])) {
                const char* p = line;
                while (isdigit((unsigned char)*p)) p++;
                while (*p == ' ') p++;
                if (*p) {
                    const char* fname = p;
                    size_t flen = strlen(fname);
                    if (flen > 5 && strcmp(fname + flen - 5, ".pcap") == 0) {
                        // Extract timestamp from suffix: ..._<ts>.pcap
                        char base[96];
                        if (flen >= sizeof(base)) flen = sizeof(base) - 1;
                        memcpy(base, fname, flen);
                        base[flen] = '\0';
                        char* dot = strrchr(base, '.');
                        if (dot) *dot = '\0';
                        char* lastUnd = strrchr(base, '_');
                        if (lastUnd && isdigit((unsigned char)lastUnd[1])) {
                            uint64_t ts = strtoull(lastUnd + 1, nullptr, 10);
                            if (ts >= importBestTimestamp) {
                                importBestTimestamp = ts;
                                strncpy(importBestFilename, fname, sizeof(importBestFilename) - 1);
                                importBestFilename[sizeof(importBestFilename) - 1] = '\0';
                            }
                        } else if (importBestFilename[0] == '\0') {
                            // Fallback: if filenames don't match the expected pattern, still grab the first .pcap.
                            importBestTimestamp = 0;
                            strncpy(importBestFilename, fname, sizeof(importBestFilename) - 1);
                            importBestFilename[sizeof(importBestFilename) - 1] = '\0';
                        }
                    }
                }
            }
        }
    }

    // --- Pong (connection handshake) ---
    if (strcmp(line, "pong") == 0) {
        if (state == C5State::DISCONNECTED || state == C5State::ERROR) {
            setState(C5State::CONNECTED);
            currentOp = C5Op::NONE;
            setSeqStep(SeqStep::IDLE);
            parsingChannelView = false;
            parsing5GHz = false;
            pktPerSecond = 0;
            Display::notify(NoticeKind::STATUS, "C5 BOARD LINKED", 2000, NoticeChannel::TOP_BAR);
            Mood::setStatusMessage("5ghz linked up");
            Avatar::cuteJump();

            if (!c5XpAwarded) {
                XP::addXP(XPEvent::C5_CONNECTED);
                c5XpAwarded = true;
            }
            C5_LOGF("Board connected");

            // Auto-start GPS forwarding from C5.
            // The GPS module may be on the C5's Grove port (not Porkchop's),
            // so request regardless of local GPS config/pin state.
            if (!c5GpsForwarding) {
                c5GpsPendingStart = true;
            }

            // Reset and probe caps on each (re)connect.
            memset(&caps, 0, sizeof(caps));
            caps.valid = false;
            capsProbePending = true;
            capsProbeSent = false;
            capsProbeSentMs = 0;
        }
        return;
    }

    // --- Scan completion marker ---
    if (strncmp(line, "Scan results printed", 20) == 0) {
        if (state == C5State::SCANNING) {
            C5_LOGF("Scan complete: %d networks", scanCountWork);

            // Finalize scan results: swap work cache into active so UI can keep showing data while next scan runs.
            C5ScanEntry* oldActive = scanCacheActive;
            scanCacheActive = scanCacheWork;
            scanCacheWork = oldActive;
            scanCountActive = scanCountWork;
            scanCountWork = 0;
            scanWork5GHzXpAwarded = false;
            lastScanCompleteMs = millis();

            injectScanIntoRecon();

            // If we were scanning as part of a handshake sequence, advance
            if (currentOp == C5Op::HANDSHAKE || currentOp == C5Op::SAE_OVERFLOW) {
                setSeqStep(SeqStep::TARGET_SELECTING);
            } else {
                currentOp = C5Op::NONE;
                setSeqStep(SeqStep::IDLE);
                setState(C5State::CONNECTED);

                char buf[32];
                snprintf(buf, sizeof(buf), "C5: %d NETS FOUND", scanCountActive);
                Display::notify(NoticeKind::STATUS, buf, 2000, NoticeChannel::TOP_BAR);
            }
        }
        return;
    }

    // --- Scan async status ---
    if (strncmp(line, "WiFi scan completed", 19) == 0) {
        if (seqStep == SeqStep::SCAN_REQUESTED) {
            // JanOS scan done, now request results
            setSeqStep(SeqStep::SCAN_SHOW_RESULTS);
            sendCommand("show_scan_results");
        }
        return;
    }

    if (strncmp(line, "No networks found", 17) == 0 ||
        strncmp(line, "No scan has been", 16) == 0) {
        if (state == C5State::SCANNING) {
            C5_LOGF("Scan: %s", line);
            scanCountWork = 0;
            scanWork5GHzXpAwarded = false;
            scanCountActive = 0;
            lastScanCompleteMs = millis();
            if (currentOp == C5Op::HANDSHAKE) {
                hsResult = HandshakeResult::FAILED;
            }
            setState(C5State::CONNECTED);
            currentOp = C5Op::NONE;
            setSeqStep(SeqStep::IDLE);
        }
        return;
    }

    // --- Handshake capture markers ---
    if (C5UartMarkers::isHandshakeCapturedLine(line)) {
        hsResult = HandshakeResult::CAPTURED;
        C5_LOGF("Handshake captured: %s", line);

        // Capture metadata for auto-import naming.
        memcpy(lastHandshakeBssid, targetBssid, 6);
        lastHandshakeCapturedMs = millis();
        lastHandshakeSsid[0] = '\0';
        const char* q1 = strchr(line, '\'');
        const char* q2 = q1 ? strchr(q1 + 1, '\'') : nullptr;
        if (q1 && q2 && q2 > q1 + 1) {
            size_t n = (size_t)(q2 - (q1 + 1));
            if (n >= sizeof(lastHandshakeSsid)) n = sizeof(lastHandshakeSsid) - 1;
            memcpy(lastHandshakeSsid, q1 + 1, n);
            lastHandshakeSsid[n] = '\0';
        }

        // Schedule import once C5 returns to idle.
        importPendingAuto = true;
        return;
    }

    // projectZero JanOS and older variants use different completion strings.
    // Accept any of them, but only while we are actually running a handshake op.
    if (currentOp == C5Op::HANDSHAKE && C5UartMarkers::isHandshakeCompleteLine(line)) {
        // If we never saw a capture marker, count as failure.
        if (hsResult == HandshakeResult::IN_PROGRESS) {
            hsResult = HandshakeResult::FAILED;
        }
        // Do not send an extra "stop" here: the C5 has already finished its handshake task.
        setState(C5State::CONNECTED);
        currentOp = C5Op::NONE;
        setSeqStep(SeqStep::IDLE);
        return;
    }

    // --- Channel view parsing ---
    if (strcmp(line, "channel_view_start") == 0) {
        parsingChannelView = true;
        parsing5GHz = false;
        memset(&channelCounts, 0, sizeof(channelCounts));
        return;
    }
    if (strcmp(line, "channel_view_end") == 0) {
        parsingChannelView = false;
        channelCounts.valid = true;
        return;
    }
    if (parsingChannelView) {
        parseChannelViewLine(line);
        return;
    }

    // --- Packet monitor ---
    {
        const char* pktSuffix = strstr(line, "pkts");
        if (pktSuffix != nullptr && pktSuffix != line) {
            // Parse "<N>pkts"
            pktPerSecond = (uint32_t)atoi(line);
            return;
        }
    }

    // --- Stop confirmation ---
    if (strstr(line, "Stop command received") != nullptr ||
        strstr(line, "Channel view monitor stopped") != nullptr ||
        strstr(line, "Stopping packet monitor") != nullptr) {
        if (seqStep == SeqStep::STOPPING) {
            // Advance sequencer past stop phase
            advanceSequencer();
        }
        return;
    }

    // --- CSV scan lines (start with '"') ---
    if (line[0] == '"' && (state == C5State::SCANNING) &&
        (seqStep == SeqStep::SCAN_SHOW_RESULTS || seqStep == SeqStep::SCAN_REQUESTED)) {
        parseScanCSV(line);
        return;
    }

    // --- "Selected network N:" confirmation (for SAE overflow sequence) ---
    if (strncmp(line, "Selected network", 16) == 0) {
        if (seqStep == SeqStep::SAE_SELECT_SENT) {
            sendCommand("sae_overflow");
            setSeqStep(SeqStep::ATTACK_MONITORING);
            C5_LOGF("SAE overflow started after selection");
        }
        return;
    }

    // --- "Scan still in progress..." retry ---
    if (strncmp(line, "Scan still in progress", 22) == 0) {
        // JanOS hasn't finished scanning yet. Retry show_scan_results after 500ms.
        // Reset the seqStep timer so advanceSequencer doesn't time out prematurely.
        if (seqStep == SeqStep::SCAN_SHOW_RESULTS) {
            setSeqStep(SeqStep::SCAN_REQUESTED); // Will re-trigger show_scan_results on "WiFi scan completed"
        }
        return;
    }

    // --- Error messages ---
    if (strstr(line, "already active") != nullptr ||
        strstr(line, "already running") != nullptr) {
        C5_LOGF("Conflict: %s", line);
        return;
    }
}

// ============================================================================
// Internal: JanOS CSV Parser
// ============================================================================
// Format: "id","ssid","vendor","bssid","channel","security","rssi","band"
// All fields double-quoted. Parse by walking quotes — no strdup, no strtok.

static bool extractQuotedField(const char*& p, char* out, uint8_t maxLen) {
    if (*p != '"') return false;
    p++; // skip opening quote

    uint8_t i = 0;
    while (*p) {
        // CSV escape: "" inside a quoted field means a literal '"'
        if (*p == '"' && *(p + 1) == '"') {
            if (i < maxLen - 1) out[i++] = '"';
            p += 2;
            continue;
        }
        // Closing quote: '"' followed by comma or EOL
        if (*p == '"' && (*(p + 1) == ',' || *(p + 1) == '\0' || *(p + 1) == '\n')) {
            break;
        }
        if (i < maxLen - 1) out[i++] = *p;
        p++;
    }
    out[i] = '\0';

    if (*p == '"') p++; // skip closing quote
    if (*p == ',') p++; // skip comma

    return true;
}

static void parseScanCSV(const char* line) {
    if (scanCountWork >= SCAN_CACHE_MAX) return;

    char field[65]; // reusable field buffer
    const char* p = line;

    C5ScanEntry& entry = scanCacheWork[scanCountWork];
    memset(&entry, 0, sizeof(C5ScanEntry));

    // Field 0: index (1-based)
    if (!extractQuotedField(p, field, sizeof(field))) return;
    entry.c5Index = (uint8_t)atoi(field);

    // Field 1: SSID
    if (!extractQuotedField(p, entry.ssid, sizeof(entry.ssid))) return;

    // Field 2: vendor (ignore)
    if (!extractQuotedField(p, field, sizeof(field))) return;

    // Field 3: BSSID ("AA:BB:CC:DD:EE:FF")
    if (!extractQuotedField(p, field, sizeof(field))) return;
    if (strlen(field) >= 17) {
        for (int i = 0; i < 6; i++) {
            char hex[3] = { field[i * 3], field[i * 3 + 1], '\0' };
            entry.bssid[i] = (uint8_t)strtol(hex, nullptr, 16);
        }
    }

    // Field 4: channel
    if (!extractQuotedField(p, field, sizeof(field))) return;
    entry.channel = (uint8_t)atoi(field);

    // Field 5: security
    if (!extractQuotedField(p, field, sizeof(field))) return;
    entry.authmode = (uint8_t)mapAuthString(field);

    // Field 6: RSSI
    if (!extractQuotedField(p, field, sizeof(field))) return;
    entry.rssi = (int8_t)atoi(field);

    // Field 7: band (ignore — derived from channel)

    scanCountWork++;

    // Award XP once per scan batch for discovering any 5GHz networks.
    if (entry.channel > 14 && !scanWork5GHzXpAwarded) {
        scanWork5GHzXpAwarded = true;
        XP::addXP(XPEvent::C5_5GHZ_FOUND);
    }
}

static wifi_auth_mode_t mapAuthString(const char* auth) {
    if (strcmp(auth, "OPEN") == 0) return WIFI_AUTH_OPEN;
    if (strcmp(auth, "WEP") == 0) return WIFI_AUTH_WEP;
    if (strcmp(auth, "WPA_PSK") == 0) return WIFI_AUTH_WPA_PSK;
    if (strcmp(auth, "WPA2_PSK") == 0) return WIFI_AUTH_WPA2_PSK;
    if (strcmp(auth, "WPA_WPA2_PSK") == 0) return WIFI_AUTH_WPA_WPA2_PSK;
    if (strcmp(auth, "WPA2_ENTERPRISE") == 0) return WIFI_AUTH_WPA2_ENTERPRISE;
    if (strcmp(auth, "WPA3_PSK") == 0) return WIFI_AUTH_WPA3_PSK;
    if (strcmp(auth, "WPA2_WPA3_PSK") == 0) return WIFI_AUTH_WPA2_WPA3_PSK;
    if (strcmp(auth, "WAPI_PSK") == 0) return WIFI_AUTH_WAPI_PSK;
    return WIFI_AUTH_WPA2_PSK; // default
}

// ============================================================================
// Internal: Channel View Parser
// ============================================================================

static void parseChannelViewLine(const char* line) {
    if (strcmp(line, "band:24") == 0) {
        parsing5GHz = false;
        return;
    }
    if (strcmp(line, "band:5") == 0) {
        parsing5GHz = true;
        return;
    }

    // Parse "ch<N>:<count>"
    if (line[0] != 'c' || line[1] != 'h') return;

    const char* colonPos = strchr(line + 2, ':');
    if (!colonPos) return;

    int chNum = atoi(line + 2);
    uint16_t count = (uint16_t)atoi(colonPos + 1);

    if (!parsing5GHz) {
        if (chNum >= 1 && chNum <= 14) {
            channelCounts.ch24[chNum - 1] = count;
        }
    } else {
        int8_t idx = ch5ToIndex((uint8_t)chNum);
        if (idx >= 0) {
            channelCounts.ch5[idx] = count;
        }
    }
}

// ============================================================================
// Internal: Command Sequencer
// ============================================================================

static void advanceSequencer() {
    uint32_t elapsed = millis() - seqStepEnteredMs;

    switch (seqStep) {
        case SeqStep::STOPPING:
            if (elapsed >= STOP_QUIET_MS) {
                if (currentOp == C5Op::SCAN) {
                    setSeqStep(SeqStep::SCAN_REQUESTED);
                    sendCommand("scan_networks");
                } else if (currentOp == C5Op::HANDSHAKE || currentOp == C5Op::SAE_OVERFLOW) {
                    if (targetC5Index > 0) {
                        // Already have index, go straight to attack
                        char cmd[48];
                        if (currentOp == C5Op::HANDSHAKE) {
                            snprintf(cmd, sizeof(cmd), "start_handshake %d", targetC5Index);
                            sendCommand(cmd);
                            setSeqStep(SeqStep::ATTACK_SENT);
                        } else {
                            // SAE: select first, wait for confirm, then overflow
                            snprintf(cmd, sizeof(cmd), "select_networks %d", targetC5Index);
                            sendCommand(cmd);
                            setSeqStep(SeqStep::SAE_SELECT_SENT);
                        }
                        setState(C5State::ATTACKING);
                    } else {
                        // Need to scan first
                        scanCountWork = 0;
                        scanWork5GHzXpAwarded = false;
                        setSeqStep(SeqStep::SCAN_REQUESTED);
                        sendCommand("scan_networks");
                        setState(C5State::SCANNING);
                    }
                 } else if (currentOp == C5Op::CHANNEL_VIEW) {
                     setSeqStep(SeqStep::CHANNEL_VIEW_ACTIVE);
                     sendCommand("channel_view");
                     setState(C5State::MONITORING);
                 } else if (currentOp == C5Op::PACKET_MONITOR) {
                     char cmd[32];
                     snprintf(cmd, sizeof(cmd), "packet_monitor %d", (int)pktMonChannel);
                     setSeqStep(SeqStep::PACKET_MONITOR_ACTIVE);
                     sendCommand(cmd);
                     setState(C5State::MONITORING);
                 } else if (currentOp == C5Op::IMPORT_HANDSHAKES) {
                     // List handshakes on the C5 SD, then pull the newest .pcap.
                     importListingActive = true;
                     importBestTimestamp = 0;
                     importBestFilename[0] = '\0';
                     xferLastProgressMs = millis();
                     setSeqStep(SeqStep::IMPORT_LISTING);
                     sendCommand("list_dir lab/handshakes");
                     setState(C5State::TRANSFERRING);
                 }
             }
             break;

        case SeqStep::SCAN_REQUESTED:
            // Fallback: some JanOS builds don't print (or vary) the "WiFi scan completed" marker.
            // After a short delay, proactively request results; JanOS will reply with either CSV or
            // "Scan still in progress...".
            if (elapsed >= 8000) {
                setSeqStep(SeqStep::SCAN_SHOW_RESULTS);
                sendCommand("show_scan_results");
            }
            break;

        case SeqStep::TARGET_SELECTING: {
            // Find our target BSSID in the fresh scan cache
            int idx = findBssidInCache(targetBssid);
            if (idx >= 0) {
                targetC5Index = scanCacheActive[idx].c5Index;
                char cmd[48];
                if (currentOp == C5Op::HANDSHAKE) {
                    snprintf(cmd, sizeof(cmd), "start_handshake %d", targetC5Index);
                    sendCommand(cmd);
                    setSeqStep(SeqStep::ATTACK_SENT);
                } else {
                    // SAE: select first, wait for confirm, then overflow
                    snprintf(cmd, sizeof(cmd), "select_networks %d", targetC5Index);
                    sendCommand(cmd);
                    setSeqStep(SeqStep::SAE_SELECT_SENT);
                }
                setState(C5State::ATTACKING);
                C5_LOGF("Target found at C5 index %d", targetC5Index);
            } else {
                // Target not found in C5 scan
                C5_LOGF("Target BSSID not found in C5 scan");
                hsResult = HandshakeResult::FAILED;
                currentOp = C5Op::NONE;
                setSeqStep(SeqStep::IDLE);
                setState(C5State::CONNECTED);
            }
            break;
        }

        case SeqStep::SAE_SELECT_SENT:
            // Wait for "Selected network N:" confirmation (or timeout after 2s)
            if (elapsed >= 2000) {
                // Timeout — send sae_overflow anyway
                sendCommand("sae_overflow");
                setSeqStep(SeqStep::ATTACK_MONITORING);
            }
            break;

        case SeqStep::ATTACK_SENT:
            // Transition to monitoring
            setSeqStep(SeqStep::ATTACK_MONITORING);
            break;

        case SeqStep::IMPORT_LISTING:
            // list_dir should be quick; if we see nothing for too long, abort.
            if (elapsed >= 10000) {
                abortTransfer("LIST TIMEOUT");
            }
            break;

        case SeqStep::IMPORT_FILE_GET:
            // Waiting for file_get_start/chunks/end. Stall detection is handled in update().
            break;

        default:
            break;
    }
}

// ============================================================================
// Internal: Inject Scan Results Into NetworkRecon
// ============================================================================

static void injectScanIntoRecon() {
    uint8_t injected = 0;
    for (uint8_t i = 0; i < scanCountActive; i++) {
        const C5ScanEntry& entry = scanCacheActive[i];
        if (entry.channel <= 14) continue;  // Porkchop already owns 2.4GHz
        NetworkRecon::injectExternal(
            entry.bssid, entry.ssid, entry.rssi,
            entry.channel, (wifi_auth_mode_t)entry.authmode,
            NET_SOURCE_C5
        );
        injected++;
        if (injected % 8 == 0) yield();
    }
    C5_LOGF("Injected %d 5GHz networks into recon", injected);
}

// ============================================================================
// Internal: BSSID Lookup in Scan Cache
// ============================================================================

static int findBssidInCache(const uint8_t* bssid) {
    for (uint8_t i = 0; i < scanCountActive; i++) {
        if (memcmp(scanCacheActive[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Internal: Handshake Import Helpers
// ============================================================================

static bool sanitizeSsidLikeC5(const char* in, char* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!in || in[0] == '\0') return false;

    size_t j = 0;
    for (size_t i = 0; i < 32 && in[i] && j < outLen - 1; i++) {
        char c = in[i];
        bool ok = (isalnum((unsigned char)c) != 0) || (c == '-') || (c == '_') || (c == '.') || (c == ' ');
        if (!ok) c = '_';
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[j++] = c;
    }
    // Trim trailing spaces/underscores for stable comparisons
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '_')) {
        j--;
    }
    out[j] = '\0';
    return j > 0;
}

static bool isAllHexN(const char* s, size_t n) {
    if (!s) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'F') ||
                   (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

static uint8_t hexByte(const char* p) {
    auto nib = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
        if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
        return 0;
    };
    return (uint8_t)((nib(p[0]) << 4) | nib(p[1]));
}

static bool deriveNamingFromC5Filename(const char* filename, uint8_t outBssid[6], char outSsid[33]) {
    if (outBssid) memset(outBssid, 0, 6);
    if (outSsid) outSsid[0] = '\0';
    if (!filename || filename[0] == '\0' || !outBssid || !outSsid) return false;

    // Strip directories
    const char* base = filename;
    const char* slash = strrchr(filename, '/');
    if (slash) base = slash + 1;
    const char* bslash = strrchr(base, '\\');
    if (bslash) base = bslash + 1;

    size_t blen = strlen(base);
    if (blen < 10) return false;

    char tmp[96];
    if (blen >= sizeof(tmp)) blen = sizeof(tmp) - 1;
    memcpy(tmp, base, blen);
    tmp[blen] = '\0';

    // Remove extension
    char* dot = strrchr(tmp, '.');
    if (!dot) return false;
    *dot = '\0';

    // Parse ..._<mac6>_<ts>
    char* u3 = strrchr(tmp, '_');
    if (!u3 || !u3[1] || !isdigit((unsigned char)u3[1])) return false;
    *u3 = '\0';

    char* u2 = strrchr(tmp, '_');
    if (!u2 || strlen(u2 + 1) != 6 || !isAllHexN(u2 + 1, 6)) return false;

    uint8_t suffix[3] = { hexByte(u2 + 1), hexByte(u2 + 3), hexByte(u2 + 5) };
    *u2 = '\0';

    // SSID portion (already C5-sanitized)
    strncpy(outSsid, tmp[0] ? tmp : "HIDDEN", 32);
    outSsid[32] = '\0';

    char ssidSafeWant[33] = {0};
    sanitizeSsidLikeC5(outSsid, ssidSafeWant, sizeof(ssidSafeWant));

    // Find best match from last scan cache by MAC suffix (+ optional SSID match).
    int bestIdx = -1;
    int bestScore = 0;
    int8_t bestRssi = -127;
    for (uint8_t i = 0; i < scanCountActive; i++) {
        const C5ScanEntry& e = scanCacheActive[i];
        if (e.bssid[3] != suffix[0] || e.bssid[4] != suffix[1] || e.bssid[5] != suffix[2]) {
            continue;
        }

        int score = 1;
        char ssidSafeHave[33] = {0};
        if (sanitizeSsidLikeC5(e.ssid, ssidSafeHave, sizeof(ssidSafeHave)) &&
            ssidSafeWant[0] && strcmp(ssidSafeHave, ssidSafeWant) == 0) {
            score = 2;
        }

        if (score > bestScore || (score == bestScore && e.rssi > bestRssi)) {
            bestScore = score;
            bestIdx = (int)i;
            bestRssi = e.rssi;
        }
    }

    if (bestIdx >= 0) {
        const C5ScanEntry& e = scanCacheActive[bestIdx];
        memcpy(outBssid, e.bssid, 6);
        strncpy(outSsid, e.ssid[0] ? e.ssid : outSsid, 32);
        outSsid[32] = '\0';
        return true;
    }

    // Could not derive full BSSID.
    return false;
}

static void abortTransfer(const char* reason) {
    if (xferFile) {
        xferFile.close();
    }
    // Remove partial file to avoid confusing the captures browser.
    if (xferLocalPath[0] && SD.exists(xferLocalPath)) {
        SD.remove(xferLocalPath);
    }

    xferActive = false;
    xferAwaitingStart = false;
    xferTotalBytes = 0;
    xferWrittenBytes = 0;
    xferRemotePath[0] = '\0';
    xferLocalPath[0] = '\0';
    xferExpectedOffset = 0;
    importListingActive = false;
    importBestTimestamp = 0;
    importBestFilename[0] = '\0';

    if (currentOp == C5Op::IMPORT_HANDSHAKES) {
        currentOp = C5Op::NONE;
        setSeqStep(SeqStep::IDLE);
        if (state != C5State::OFF && state != C5State::DISCONNECTED && state != C5State::ERROR) {
            setState(C5State::CONNECTED);
        }
    }

    if (reconPausedForImport) {
        NetworkRecon::resume();
        reconPausedForImport = false;
    }

    if (reason && reason[0]) {
        Display::notify(NoticeKind::STATUS, reason, 2000, NoticeChannel::TOP_BAR);
        C5_LOGF("Import aborted: %s", reason);
    }
}

static void sendFileGetForRemotePath(const char* remotePath) {
    if (!remotePath || remotePath[0] == '\0') return;

    char cmd[220];
    // Quote the path: C5 filenames may include spaces.
    snprintf(cmd, sizeof(cmd), "file_get \"%s\"", remotePath);
    sendCommand(cmd);
}

static bool startTransferForRemotePath(const char* remotePath, const uint8_t bssidForName[6], const char* ssidForName) {
    if (!remotePath || remotePath[0] == '\0') return false;

    // Reset any prior transfer state (idempotent).
    if (xferFile) {
        xferFile.close();
    }
    xferActive = false;
    xferAwaitingStart = true;
    xferTotalBytes = 0;
    xferWrittenBytes = 0;
    xferCrc = 0xFFFFFFFFu;
    xferExpectedOffset = 0;
    xferLastProgressMs = millis();

    strncpy(xferRemotePath, remotePath, sizeof(xferRemotePath) - 1);
    xferRemotePath[sizeof(xferRemotePath) - 1] = '\0';

    const char* dir = SDLayout::handshakesDir();
    if (!SD.exists(dir)) {
        if (!SD.mkdir(dir)) {
            Display::notify(NoticeKind::STATUS, "IMPORT: MKDIR FAIL", 2500, NoticeChannel::TOP_BAR);
            return false;
        }
    }

    uint8_t bssid[6] = {0};
    char ssid[33] = {0};
    bool haveName = false;

    if (bssidForName != nullptr) {
        bool nonZero = false;
        for (int i = 0; i < 6; i++) nonZero |= (bssidForName[i] != 0);
        if (nonZero) {
            memcpy(bssid, bssidForName, 6);
            if (ssidForName && ssidForName[0]) {
                strncpy(ssid, ssidForName, 32);
                ssid[32] = '\0';
            }
            haveName = true;
        }
    }

    if (!haveName) {
        // Derive from the C5 filename format if possible.
        haveName = deriveNamingFromC5Filename(remotePath, bssid, ssid);
    }

    if (haveName) {
        SDLayout::buildCaptureFilename(xferLocalPath, sizeof(xferLocalPath),
                                       dir, ssid, bssid, ".pcap");
    } else {
        // Fallback: preserve remote base filename (captures menu will still show it, but without BSSID parsing).
        const char* base = remotePath;
        const char* slash = strrchr(remotePath, '/');
        if (slash) base = slash + 1;
        size_t blen = strlen(base);
        if (blen > 70) blen = 70;
        char localBase[80];
        memcpy(localBase, base, blen);
        localBase[blen] = '\0';
        snprintf(xferLocalPath, sizeof(xferLocalPath), "%s/%s", dir, localBase);
    }

    setSeqStep(SeqStep::IMPORT_FILE_GET);
    setState(C5State::TRANSFERRING);

    Display::notify(NoticeKind::STATUS, "IMPORTING FROM C5...", 1500, NoticeChannel::TOP_BAR);
    sendFileGetForRemotePath(remotePath);
    return true;
}

// ============================================================================
// C5 GPS Forwarding (start_gps_raw NMEA passthrough)
// ============================================================================

static void feedNmeaLine(const char* line) {
    // Confirm GPS forwarding on first NMEA sentence received
    if (c5GpsAwaitingStart && !c5GpsForwarding) {
        c5GpsForwarding = true;
        c5GpsAwaitingStart = false;
        Display::notify(NoticeKind::STATUS, "C5 GPS: NMEA FLOWING", 2000, NoticeChannel::TOP_BAR);
        C5_LOGF("GPS forwarding confirmed (NMEA received)");
    }

    // Feed each character of the NMEA sentence to TinyGPSPlus, including
    // a trailing newline to mark sentence end.
    for (const char* p = line; *p; p++) {
        c5Gps.encode(*p);
    }
    c5Gps.encode('\n');

    updateC5GpsData();
}

static void updateC5GpsData() {
    bool prevFix = c5GpsData.fix;
    bool valid = c5Gps.location.isValid();
    uint32_t age = c5Gps.location.age();
    uint8_t satellites = c5Gps.satellites.value();
    uint16_t hdop = c5Gps.hdop.value();

    // Quality-gated fix (same logic as local GPS)
    bool rawFix = valid && (age < GPSQuality::MAX_AGE_MS);
    bool qualityOk = GPSQuality::isFixAcceptable(satellites, hdop);
    bool fix = rawFix && qualityOk;

    double latitude  = c5Gps.location.lat();
    double longitude = c5Gps.location.lng();
    double altitude  = c5Gps.altitude.meters();
    float  speed     = c5Gps.speed.kmph();
    float  course    = c5Gps.course.deg();
    uint32_t date    = c5Gps.date.isValid() ? c5Gps.date.value() : 0;
    uint32_t time    = c5Gps.time.isValid() ? c5Gps.time.value() : 0;

    // Coasting logic
    bool coasting = false;
    uint32_t now = millis();

    if (fix) {
        if (GPSQuality::isCoastWorthy(satellites, hdop)) {
            c5CoastSnapshot.latitude  = latitude;
            c5CoastSnapshot.longitude = longitude;
            c5CoastSnapshot.altitude  = altitude;
            c5CoastSnapshot.speed     = speed;
            c5CoastSnapshot.course    = course;
            c5CoastSnapshot.satellites = satellites;
            c5CoastSnapshot.hdop      = hdop;
            c5CoastSnapshot.valid     = true;
            c5CoastSnapshot.fix       = true;
            c5CoastSnapshot.coasting  = false;
            c5CoastStartMs = now;
        }
        c5CoastActive = false;
    } else if (c5CoastSnapshot.valid && (now - c5CoastStartMs) < GPSQuality::COAST_MS) {
        if (!c5CoastActive) c5CoastActive = true;
        coasting = true;
        fix = true;
        latitude  = c5CoastSnapshot.latitude;
        longitude = c5CoastSnapshot.longitude;
        altitude  = c5CoastSnapshot.altitude;
        speed     = c5CoastSnapshot.speed;
        course    = c5CoastSnapshot.course;
    } else {
        if (c5CoastActive) {
            c5CoastActive = false;
            c5CoastSnapshot.valid = false;
        }
    }

    c5GpsData.latitude   = latitude;
    c5GpsData.longitude  = longitude;
    c5GpsData.altitude   = altitude;
    c5GpsData.speed      = speed;
    c5GpsData.course     = course;
    c5GpsData.satellites = satellites;
    c5GpsData.hdop       = hdop;
    c5GpsData.date       = date;
    c5GpsData.time       = time;
    c5GpsData.age        = age;
    c5GpsData.valid      = valid;
    c5GpsData.fix        = fix;
    c5GpsData.coasting   = coasting;

    if (fix && !prevFix) {
        c5GpsLastFixMs = now;
        GPS::maybeSyncSystemTime(c5GpsData.date, c5GpsData.time);
        char buf[40];
        snprintf(buf, sizeof(buf), "C5 GPS FIX %dSAT", c5GpsData.satellites);
        Display::notify(NoticeKind::STATUS, buf, 2500, NoticeChannel::TOP_BAR);
        C5_LOGF("GPS fix acquired: %.6f,%.6f sats=%d",
                 c5GpsData.latitude, c5GpsData.longitude, c5GpsData.satellites);
    } else if (fix) {
        c5GpsLastFixMs = now;
    } else if (prevFix) {
        Display::notify(NoticeKind::WARNING, "C5 GPS FIX LOST", 2000, NoticeChannel::TOP_BAR);
        C5_LOGF("GPS fix lost (valid=%d age=%lu)", valid, (unsigned long)age);
    }
}

bool JanusHog::hasC5GPSFix() {
    return c5GpsForwarding && c5GpsData.fix &&
           (millis() - c5GpsLastFixMs < 10000);
}

GPSData JanusHog::getC5GPSData() {
    return c5GpsData;
}

bool JanusHog::isGPSForwarding() {
    return c5GpsForwarding || c5GpsAwaitingStart;
}
