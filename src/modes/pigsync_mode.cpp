/**
 * PigSync ESP-NOW Client Implementation (Porkchop/POPS side)
 * 
 * SON OF A PIG - Reliable sync with Sirloin devices
 */

#include "pigsync_mode.h"
#include "pigsync_protocol.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <SD.h>
#include <M5Cardputer.h>
#include <sys/time.h>  // Phase 3: settimeofday for RTC sync
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/wifi_utils.h"
#include "../core/heap_gates.h"
#include "../core/heap_policy.h"
#include "../core/network_recon.h"
#include "../piglet/mood.h"
#include "../ui/display.h"
#include "../modes/warhog.h"
#include "../modes/oink.h"
#include "../web/wpasec.h"
#include "../web/wigle.h"


#ifndef PIGSYNC_LOG_ENABLED
#define PIGSYNC_LOG_ENABLED 0
#endif

#if PIGSYNC_LOG_ENABLED
#define PIGSYNC_LOGF(...) Serial.printf(__VA_ARGS__)
#define PIGSYNC_LOGLN(msg) Serial.println(msg)
#define PIGSYNC_LOG(msg) Serial.print(msg)
#else
#define PIGSYNC_LOGF(...) do {} while (0)
#define PIGSYNC_LOGLN(...) do {} while (0)
#define PIGSYNC_LOG(msg) do {} while (0)
#endif

// ==[ STATIC MEMBER DEFINITIONS ]==
bool PigSyncMode::running = false;
bool PigSyncMode::initialized = false;
uint8_t PigSyncMode::selectedIndex = 0;

std::vector<SirloinDevice> PigSyncMode::devices;
uint8_t PigSyncMode::connectedMac[6] = {0};
bool PigSyncMode::connected = false;

uint16_t PigSyncMode::remotePMKIDCount = 0;
uint16_t PigSyncMode::remoteHSCount = 0;
uint16_t PigSyncMode::totalSynced = 0;
uint16_t PigSyncMode::syncedPMKIDs = 0;
uint16_t PigSyncMode::syncedHandshakes = 0;

PigSyncMode::State PigSyncMode::state = PigSyncMode::State::IDLE;

uint8_t PigSyncMode::currentType = 0;
uint16_t PigSyncMode::currentIndex = 0;
uint16_t PigSyncMode::totalChunks = 0;
uint16_t PigSyncMode::receivedChunks = 0;

SyncProgress PigSyncMode::progress = {0};
uint8_t PigSyncMode::rxBuffer[2048] = {0};
uint16_t PigSyncMode::rxBufferLen = 0;
char PigSyncMode::lastError[64] = {0};

uint8_t PigSyncMode::dialogueId = 0;
uint8_t PigSyncMode::dialoguePhase = 0;
uint32_t PigSyncMode::callStartTime = 0;
uint32_t PigSyncMode::phraseStartTime = 0;
char PigSyncMode::papaGoodbyeSelected[64] = {0};

uint32_t PigSyncMode::lastDiscoveryTime = 0;
uint32_t PigSyncMode::discoveryStartTime = 0;
bool PigSyncMode::scanning = false;
uint32_t PigSyncMode::connectStartTime = 0;
uint32_t PigSyncMode::lastHelloTime = 0;
uint8_t PigSyncMode::helloRetryCount = 0;
uint32_t PigSyncMode::readyStartTime = 0;
uint8_t PigSyncMode::channelRetryCount = 0;
uint32_t PigSyncMode::syncCompleteTime = 0;

uint16_t PigSyncMode::sessionId = 0;
uint8_t PigSyncMode::remoteMood = 128;
uint8_t PigSyncMode::lastBountyMatches = 0;
uint8_t PigSyncMode::dataChannel = PIGSYNC_DISCOVERY_CHANNEL;

// Reliability tracking
static PigSyncReliability reliability;

PigSyncMode::CaptureCallback PigSyncMode::onCaptureCb = nullptr;
PigSyncMode::SyncCompleteCallback PigSyncMode::onSyncCompleteCb = nullptr;

// Peer info for connected Sirloin
static esp_now_peer_info_t sirloinPeer;

// ==[ CONTROL TX RELIABILITY ]==
static const size_t CONTROL_TX_MAX = 160;
static const uint8_t CONTROL_QUEUE_MAX = 3;
struct ControlTxState {
    bool waiting;
    uint8_t type;
    uint8_t seq;
    uint32_t lastSend;
    uint8_t retries;
    size_t len;
    uint8_t mac[6];
    uint8_t buf[CONTROL_TX_MAX];
};

static ControlTxState controlTx = {};
static ControlTxState controlQueue[CONTROL_QUEUE_MAX] = {};
static uint8_t controlQueueHead = 0;
static uint8_t controlQueueTail = 0;
static uint8_t controlQueueCount = 0;
static bool pendingStartSync = false;
static bool pendingNextCapture = false;

static bool isControlCommand(uint8_t type) {
    switch (type) {
        case CMD_HELLO:
        case CMD_READY:
        case CMD_GET_COUNT:
        case CMD_MARK_SYNCED:
        case CMD_PURGE:
        case CMD_BOUNTIES:
        case CMD_TIME_SYNC:
            return true;
        default:
            return false;
    }
}

static bool isControlResponse(uint8_t type) {
    switch (type) {
        case RSP_RING:
        case RSP_HELLO:
        case RSP_READY:
        case RSP_COUNT:
        case RSP_OK:
        case RSP_ERROR:
        case RSP_PURGED:
        case RSP_BOUNTIES_ACK:
        case RSP_TIME_SYNC:
        case RSP_DISCONNECT:
            return true;
        default:
            return false;
    }
}

static bool isSessionBoundResponse(uint8_t type) {
    switch (type) {
        case RSP_READY:
        case RSP_OK:
        case RSP_ERROR:
        case RSP_DISCONNECT:
        case RSP_COUNT:
        case RSP_CHUNK:
        case RSP_COMPLETE:
        case RSP_PURGED:
        case RSP_BOUNTIES_ACK:
        case RSP_TIME_SYNC:
            return true;
        default:
            return false;
    }
}

static void enqueueControl(const uint8_t* mac, const uint8_t* buf, size_t len, uint8_t type, uint8_t seq) {
    if (controlQueueCount >= CONTROL_QUEUE_MAX) {
        return;
    }
    ControlTxState& slot = controlQueue[controlQueueTail];
    memcpy(slot.buf, buf, len);
    slot.len = len;
    slot.type = type;
    slot.seq = seq;
    memcpy(slot.mac, mac, sizeof(slot.mac));
    slot.retries = 0;
    slot.waiting = true;
    slot.lastSend = 0;
    controlQueueTail = (controlQueueTail + 1) % CONTROL_QUEUE_MAX;
    controlQueueCount++;
}

static bool dequeueControl(ControlTxState& out) {
    if (controlQueueCount == 0) {
        return false;
    }
    out = controlQueue[controlQueueHead];
    controlQueueHead = (controlQueueHead + 1) % CONTROL_QUEUE_MAX;
    controlQueueCount--;
    return true;
}

static void resetControlQueue() {
    controlQueueHead = 0;
    controlQueueTail = 0;
    controlQueueCount = 0;
}

static void sendControlPacket(const uint8_t* mac, const uint8_t* buf, size_t len, uint8_t type, uint8_t seq) {
    if (len > CONTROL_TX_MAX) {
        return;
    }
    if (!controlTx.waiting) {
        memcpy(controlTx.buf, buf, len);
        controlTx.len = len;
        controlTx.type = type;
        controlTx.seq = seq;
        memcpy(controlTx.mac, mac, sizeof(controlTx.mac));
        controlTx.retries = 0;
        controlTx.waiting = true;
        controlTx.lastSend = millis();
        esp_now_send(controlTx.mac, controlTx.buf, controlTx.len);
        return;
    }
    enqueueControl(mac, buf, len, type, seq);
}

static void trySendQueuedControl() {
    if (controlTx.waiting) {
        return;
    }
    ControlTxState next = {};
    if (!dequeueControl(next)) {
        return;
    }
    controlTx = next;
    controlTx.lastSend = millis();
    esp_now_send(controlTx.mac, controlTx.buf, controlTx.len);
}

static void clearControlTx() {
    controlTx.waiting = false;
    controlTx.len = 0;
    controlTx.type = 0;
    controlTx.seq = 0;
    controlTx.retries = 0;
    controlTx.lastSend = 0;
    trySendQueuedControl();
}

static bool parseSirloinPMKID(const uint8_t* data, uint16_t len, CapturedPMKID& out) {
    if (!data || len < 65) {
        return false;
    }

    memset(&out, 0, sizeof(out));
    memcpy(out.bssid, data, 6);
    memcpy(out.station, data + 6, 6);

    uint8_t ssidLen = data[12];
    if (ssidLen > 32) ssidLen = 32;
    memcpy(out.ssid, data + 13, ssidLen);
    out.ssid[ssidLen] = '\0';

    memcpy(out.pmkid, data + 45, 16);
    out.timestamp = millis();
    out.saved = false;
    out.saveAttempts = 0;
    return true;
}

static bool parseSirloinHandshake(const uint8_t* data, uint16_t len, CapturedHandshake& out) {
    if (!data || len < 48) {
        return false;
    }

    memset(&out, 0, sizeof(out));
    out.beaconData = nullptr;
    out.beaconLen = 0;

    memcpy(out.bssid, data, 6);
    memcpy(out.station, data + 6, 6);

    uint8_t ssidLen = data[12];
    if (ssidLen > 32) ssidLen = 32;
    memcpy(out.ssid, data + 13, ssidLen);
    out.ssid[ssidLen] = '\0';

    size_t offset = 45;  // bssid(6) + station(6) + ssid_len(1) + ssid(32)
    if (offset + 3 > len) {
        return false;
    }

    // Skip serialized mask (we recompute from parsed frames).
    offset += 1;

    uint16_t beaconLen = data[offset] | (data[offset + 1] << 8);
    offset += 2;

    if (beaconLen > 0) {
        if (beaconLen > 512 || offset + beaconLen > len) {
            return false;
        }
        out.beaconData = (uint8_t*)malloc(beaconLen);
        if (!out.beaconData) {
            return false;
        }
        memcpy(out.beaconData, data + offset, beaconLen);
        out.beaconLen = beaconLen;
        offset += beaconLen;
    }

    out.capturedMask = 0;
    while (offset < len) {
        if (offset + 2 > len) break;
        uint16_t frameLen = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        if (offset + frameLen > len) break;
        const uint8_t* frameData = data + offset;
        offset += frameLen;

        if (offset + 2 > len) break;
        uint16_t fullLen = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        if (offset + fullLen + 6 > len) break;  // msg+rss+ts
        const uint8_t* fullFrame = data + offset;
        offset += fullLen;

        uint8_t msgNum = data[offset++];
        int8_t rssi = (int8_t)data[offset++];
        uint32_t ts = data[offset] |
                      (data[offset + 1] << 8) |
                      (data[offset + 2] << 16) |
                      (data[offset + 3] << 24);
        offset += 4;

        if (msgNum < 1 || msgNum > 4) {
            continue;
        }

        EAPOLFrame& frame = out.frames[msgNum - 1];
        uint16_t copyLen = frameLen;
        if (copyLen > sizeof(frame.data)) copyLen = sizeof(frame.data);
        memcpy(frame.data, frameData, copyLen);
        frame.len = copyLen;

        uint16_t fullCopyLen = fullLen;
        if (fullCopyLen > sizeof(frame.fullFrame)) fullCopyLen = sizeof(frame.fullFrame);
        if (fullCopyLen > 0) {
            memcpy(frame.fullFrame, fullFrame, fullCopyLen);
        }
        frame.fullFrameLen = fullCopyLen;
        frame.messageNum = msgNum;
        frame.rssi = rssi;
        frame.timestamp = (ts < 1000000000) ? ts : millis();

        out.capturedMask |= (1 << (msgNum - 1));
    }

    out.firstSeen = millis();
    out.lastSeen = out.firstSeen;
    out.saved = false;
    out.saveAttempts = 0;

    if (out.capturedMask == 0) {
        if (out.beaconData) {
            free(out.beaconData);
            out.beaconData = nullptr;
        }
        out.beaconLen = 0;
        return false;
    }

    return true;
}

static void removeIfExists(const char* path) {
    if (path && SD.exists(path)) {
        SD.remove(path);
    }
}

static uint8_t getControlMaxRetries(uint8_t type) {
    if (type == CMD_HELLO) {
        uint16_t retries = PIGSYNC_HELLO_TIMEOUT / PIGSYNC_ACK_TIMEOUT;
        if (retries < 1) retries = 1;
        if (retries > 255) retries = 255;
        return (uint8_t)retries;
    }
    if (type == CMD_READY) {
        uint16_t retries = PIGSYNC_READY_TIMEOUT / PIGSYNC_ACK_TIMEOUT;
        if (retries < 1) retries = 1;
        if (retries > 255) retries = 255;
        return (uint8_t)retries;
    }
    return PIGSYNC_MAX_RETRIES;
}

static void handleControlAck(const PigSyncHeader* hdr) {
    if (!controlTx.waiting) return;
    if (hdr->ack == controlTx.seq) {
        clearControlTx();
    }
}

static volatile bool pendingControlAck = false;
static volatile uint8_t pendingControlAckSeq = 0;

// Upgrade peer to encrypted after RSP_HELLO received
static void upgradePeerEncryption(const uint8_t* mac, uint8_t channel) {
    if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0) return;
    
    // Remove and re-add with encryption on specified channel
    esp_now_del_peer(mac);
    
    memset(&sirloinPeer, 0, sizeof(sirloinPeer));
    memcpy(sirloinPeer.peer_addr, mac, 6);
    sirloinPeer.channel = channel;
    sirloinPeer.encrypt = true;
    memcpy(sirloinPeer.lmk, PIGSYNC_LMK, 16);
    
    esp_now_add_peer(&sirloinPeer);
    PIGSYNC_LOGF("[PIGSYNC-CLI-PEER] Upgraded to encrypted on ch%d\n", channel);
}

// Pending data from callbacks (processed in update())
static volatile bool pendingRingReceived = false;
static volatile uint32_t pendingRingAt = 0;
static volatile bool pendingHelloReceived = false;
static volatile bool pendingHelloClearControl = false;
static volatile uint16_t pendingPMKIDCount = 0;
static volatile uint16_t pendingHSCount = 0;
static volatile uint8_t pendingDialogueId = 0;
static volatile uint8_t pendingMood = 128;
static volatile uint16_t pendingSessionId = 0;  // 16-bit session
static volatile uint8_t pendingDataChannel = PIGSYNC_DISCOVERY_CHANNEL;  // Data channel from RSP_HELLO

static volatile bool pendingReadyReceived = false;  // RSP_READY from Sirloin
static volatile bool pendingReadyClearControl = false;

static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// Name reveal notification for UI
static volatile bool pendingNameReveal = false;
static char pendingNameRevealName[16] = {0};

static volatile bool pendingChunkReceived = false;
static const uint8_t PENDING_CHUNK_QUEUE_SIZE = 8;
struct PendingChunkSlot {
    bool used;
    uint16_t seq;
    uint16_t total;
    uint16_t len;
    uint8_t data[256];
};
static PendingChunkSlot pendingChunkQueue[PENDING_CHUNK_QUEUE_SIZE] = {};
static uint8_t pendingChunkCount = 0;
static void clearPendingChunkQueue() {
    taskENTER_CRITICAL(&pendingMux);
    for (uint8_t i = 0; i < PENDING_CHUNK_QUEUE_SIZE; i++) {
        pendingChunkQueue[i].used = false;
    }
    pendingChunkCount = 0;
    pendingChunkReceived = false;
    taskEXIT_CRITICAL(&pendingMux);
}

static volatile bool pendingCompleteReceived = false;
static volatile uint16_t pendingTotalBytes = 0;
static volatile uint32_t pendingCRC = 0;

static volatile bool pendingPurgedReceived = false;
static volatile uint16_t pendingPurgedCount = 0;
static volatile uint8_t pendingBountyMatches = 0;

static volatile bool pendingErrorReceived = false;
static volatile uint8_t pendingErrorCode = 0;

// Forward declarations for pending device updates
static volatile bool pendingBeaconReceived = false;
static uint8_t pendingBeaconMac[6] = {0};
static int8_t pendingBeaconRSSI = 0;
static uint16_t pendingBeaconPending = 0;
static uint8_t pendingBeaconFlags = 0;

// Phase 3: Grunt beacon data
static volatile bool pendingGruntReceived = false;
static uint8_t pendingGruntMac[6] = {0};
static uint8_t pendingGruntFlags = 0;
static uint8_t pendingGruntCaptureCount = 0;
static uint8_t pendingGruntBattery = 0;
static uint8_t pendingGruntStorage = 0;
static uint32_t pendingGruntUnixTime = 0;
static uint16_t pendingGruntUptime = 0;
static char pendingGruntName[5] = {0};

// Phase 3: Time sync response
static volatile bool pendingTimeSyncReceived = false;
static uint8_t pendingTimeSyncValid = 0;
static uint32_t pendingTimeSyncUnix = 0;
static uint32_t pendingTimeSyncRtt = 0;  // Round-trip time in ms

// Session timeout - detect if Sirloin stops responding
static const uint32_t SESSION_TIMEOUT = 60000;  // 60 seconds (increased from 10 seconds)
static volatile uint32_t lastPacketTime = 0;
static volatile bool pendingDisconnectReceived = false;  // RSP_DISCONNECT from Sirloin

// Control response duplicate tracking
static uint8_t lastControlRspSeq = 0;
static uint8_t lastControlRspType = 0;
static uint16_t lastControlRspSession = 0;
static bool lastControlRspValid = false;

// ==[ ESP-NOW CALLBACKS ]==

// ESP32-S3 uses older ESP-NOW callback signature (no esp_now_recv_info_t)
void pigSyncOnRecv(const uint8_t* mac, const uint8_t* data, int len) {
    PIGSYNC_LOGF("[PIGSYNC-CLI-RX] len=%d from %02X:%02X:%02X:%02X:%02X:%02X\n", 
                  len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // ==[ PHASE 3: BEACON_GRUNT - Layer 0 connectionless beacon ]==
    // Check for grunt first (different format, no PigSyncHeader)
    if (len >= (int)sizeof(BeaconGrunt) && data[0] == PIGSYNC_MAGIC && data[2] == BEACON_GRUNT) {
        const BeaconGrunt* grunt = (const BeaconGrunt*)data;
        
        taskENTER_CRITICAL(&pendingMux);
        memcpy(pendingGruntMac, grunt->sirloinMac, 6);
        pendingGruntFlags = grunt->flags;
        pendingGruntCaptureCount = grunt->captureCount;
        pendingGruntBattery = grunt->batteryPercent;
        pendingGruntStorage = grunt->storagePercent;
        pendingGruntUnixTime = grunt->unixTime;
        pendingGruntUptime = grunt->uptimeMin;
        memcpy(pendingGruntName, grunt->name, 4);
        pendingGruntReceived = true;
        taskEXIT_CRITICAL(&pendingMux);
        PIGSYNC_LOGF("[PIGSYNC-CLI-RX] BEACON_GRUNT from %s batt=%d%% captures=%d\n", 
                      grunt->name, grunt->batteryPercent, grunt->captureCount);
        return;
    }
    
    if (len < (int)sizeof(PigSyncHeader)) return;
    if (!isValidPacket(data, len)) return;
    
    const PigSyncHeader* hdr = (const PigSyncHeader*)data;
    PIGSYNC_LOGF("[PIGSYNC-CLI-RX] type=0x%02X\n", hdr->type);

    if (isSessionBoundResponse(hdr->type)) {
        uint16_t expectedSession = PigSyncMode::getSessionId();
        if (expectedSession == 0) {
            taskENTER_CRITICAL(&pendingMux);
            expectedSession = pendingSessionId;
            taskEXIT_CRITICAL(&pendingMux);
        }
        if (expectedSession == 0 || hdr->sessionId != expectedSession) {
            PIGSYNC_LOGF("[PIGSYNC-CLI-RX] Ignoring session mismatch type=0x%02X session=0x%04X expected=0x%04X\n",
                          hdr->type, hdr->sessionId, expectedSession);
            return;
        }
    }

    if (hdr->sessionId != 0) {
        if (hdr->seq == reliability.lastRxSeq || isSeqNewer(hdr->seq, reliability.lastRxSeq)) {
            taskENTER_CRITICAL(&pendingMux);
            reliability.lastRxSeq = hdr->seq;
            taskEXIT_CRITICAL(&pendingMux);
        }
    }

    if (isControlResponse(hdr->type)) {
        if (controlTx.waiting && hdr->ack == controlTx.seq) {
            taskENTER_CRITICAL(&pendingMux);
            pendingControlAck = true;
            pendingControlAckSeq = hdr->ack;
            taskEXIT_CRITICAL(&pendingMux);
        }
        if (lastControlRspValid &&
            hdr->seq == lastControlRspSeq &&
            hdr->type == lastControlRspType &&
            hdr->sessionId == lastControlRspSession) {
            // Duplicate control response, ignore processing
            goto update_last_packet_time;
        }
        lastControlRspSeq = hdr->seq;
        lastControlRspType = hdr->type;
        lastControlRspSession = hdr->sessionId;
        lastControlRspValid = true;
    }
    
    switch (hdr->type) {
        case RSP_BEACON: {
            PIGSYNC_LOGLN("[PIGSYNC-CLI-RX] RSP_BEACON");
            PIGSYNC_LOGF("[PIGSYNC-CLI-RX] sizeof(RspBeacon)=%d len=%d\n", (int)sizeof(RspBeacon), len);
            if (len < (int)sizeof(RspBeacon)) {
                PIGSYNC_LOGLN("[PIGSYNC-CLI-ERR] RSP_BEACON too short");
                break;
            }
            const RspBeacon* rsp = (const RspBeacon*)data;
            
            // Store in pending buffer - will be processed in update()
            // NOTE: Use callback MAC, not rsp->son_mac! WiFi.macAddress() differs from ESP-NOW sender MAC
            taskENTER_CRITICAL(&pendingMux);
            memcpy(pendingBeaconMac, mac, 6);  // Use actual ESP-NOW sender MAC
            pendingBeaconRSSI = rsp->rssi;
            pendingBeaconPending = rsp->pending;
            pendingBeaconFlags = rsp->flags;
            pendingBeaconReceived = true;
            taskEXIT_CRITICAL(&pendingMux);
            PIGSYNC_LOGLN("[PIGSYNC-CLI-RX] pendingBeaconReceived=true");
            break;
        }
        
        case RSP_HELLO: {
            if (len < (int)sizeof(RspHello)) break;
            const RspHello* rsp = (const RspHello*)data;

            if (controlTx.waiting && controlTx.type == CMD_HELLO) {
                taskENTER_CRITICAL(&pendingMux);
                pendingHelloClearControl = true;
                taskEXIT_CRITICAL(&pendingMux);
            }
            
            taskENTER_CRITICAL(&pendingMux);
            pendingPMKIDCount = rsp->pmkid_count;
            pendingHSCount = rsp->hs_count;
            pendingDialogueId = rsp->dialogue_id % DIALOGUE_TRACK_COUNT;
            pendingMood = rsp->mood;
            pendingSessionId = rsp->hdr.sessionId;  // Session in header now
            pendingDataChannel = rsp->data_channel; // Channel for data transfer
            pendingHelloReceived = true;
            taskEXIT_CRITICAL(&pendingMux);
            PIGSYNC_LOGF("[PIGSYNC-CLI-RX] RSP_HELLO sessionId=0x%04X dataChannel=%d\n", rsp->hdr.sessionId, rsp->data_channel);
            break;
        }

        case RSP_RING: {
            if (controlTx.waiting && controlTx.type == CMD_HELLO) {
                taskENTER_CRITICAL(&pendingMux);
                pendingHelloClearControl = true;
                taskEXIT_CRITICAL(&pendingMux);
            }
            taskENTER_CRITICAL(&pendingMux);
            pendingRingReceived = true;
            pendingRingAt = millis();
            taskEXIT_CRITICAL(&pendingMux);
            PIGSYNC_LOGLN("[PIGSYNC-CLI-RX] RSP_RING");
            break;
        }
        
        case RSP_READY: {
            if (len < (int)sizeof(RspReady)) break;
            const RspReady* rsp = (const RspReady*)data;

            if (controlTx.waiting && controlTx.type == CMD_READY) {
                taskENTER_CRITICAL(&pendingMux);
                pendingReadyClearControl = true;
                taskEXIT_CRITICAL(&pendingMux);
            }
            
            taskENTER_CRITICAL(&pendingMux);
            pendingPMKIDCount = rsp->pmkid_count;
            pendingHSCount = rsp->hs_count;
            pendingReadyReceived = true;
            taskEXIT_CRITICAL(&pendingMux);
            PIGSYNC_LOGF("[PIGSYNC-CLI-RX] RSP_READY sessionId=0x%04X\n", rsp->hdr.sessionId);
            break;
        }
        
        case RSP_CHUNK: {
            if (len < (int)sizeof(RspChunk)) break;
            const RspChunk* rsp = (const RspChunk*)data;
            
            uint16_t dataLen = len - sizeof(RspChunk);
            if (dataLen > 256) dataLen = 256;

            taskENTER_CRITICAL(&pendingMux);
            int slot = -1;
            for (uint8_t i = 0; i < PENDING_CHUNK_QUEUE_SIZE; i++) {
                if (pendingChunkQueue[i].used && pendingChunkQueue[i].seq == rsp->chunk_seq) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                for (uint8_t i = 0; i < PENDING_CHUNK_QUEUE_SIZE; i++) {
                    if (!pendingChunkQueue[i].used) {
                        slot = i;
                        break;
                    }
                }
            }
            if (slot >= 0) {
                PendingChunkSlot& entry = pendingChunkQueue[slot];
                if (!entry.used) {
                    pendingChunkCount++;
                }
                entry.used = true;
                entry.seq = rsp->chunk_seq;
                entry.total = rsp->chunk_total;
                entry.len = dataLen;
                memcpy(entry.data, data + sizeof(RspChunk), dataLen);
                pendingChunkReceived = true;
            }
            taskEXIT_CRITICAL(&pendingMux);
            break;
        }
        
        case RSP_COMPLETE: {
            if (len < (int)sizeof(RspComplete)) break;
            const RspComplete* rsp = (const RspComplete*)data;
            
            taskENTER_CRITICAL(&pendingMux);
            pendingTotalBytes = rsp->total_bytes;
            pendingCRC = rsp->crc32;
            pendingCompleteReceived = true;
            taskEXIT_CRITICAL(&pendingMux);
            break;
        }
        
        case RSP_PURGED: {
            if (len < (int)sizeof(RspPurged)) break;
            const RspPurged* rsp = (const RspPurged*)data;
            
            taskENTER_CRITICAL(&pendingMux);
            pendingPurgedCount = rsp->purged_count;
            pendingBountyMatches = rsp->bounty_matches;
            pendingPurgedReceived = true;
            taskEXIT_CRITICAL(&pendingMux);
            break;
        }
        
        case RSP_ERROR: {
            if (len < (int)sizeof(RspError)) break;
            const RspError* rsp = (const RspError*)data;
            
            taskENTER_CRITICAL(&pendingMux);
            pendingErrorCode = rsp->error_code;
            pendingErrorReceived = true;
            taskEXIT_CRITICAL(&pendingMux);
            break;
        }
        
        case RSP_TIME_SYNC: {
            // Phase 3: Sirloin sends us its RTC time
            if (len < (int)sizeof(RspTimeSync)) break;
            const RspTimeSync* rsp = (const RspTimeSync*)data;
            
            taskENTER_CRITICAL(&pendingMux);
            pendingTimeSyncReceived = true;
            pendingTimeSyncValid = rsp->rtcValid;
            pendingTimeSyncUnix = rsp->sirloinUnixTime;
            pendingTimeSyncRtt = millis() - rsp->echoedMillis;  // RTT in ms
            taskEXIT_CRITICAL(&pendingMux);
            PIGSYNC_LOGF("[PIGSYNC-CLI-RX] RSP_TIME_SYNC rtcValid=%d unix=%lu rtt=%lums\n", 
                          rsp->rtcValid, rsp->sirloinUnixTime, pendingTimeSyncRtt);
            break;
        }
        
        case RSP_DISCONNECT: {
            // Sirloin is ending the call
            taskENTER_CRITICAL(&pendingMux);
            pendingDisconnectReceived = true;
            taskEXIT_CRITICAL(&pendingMux);
            PIGSYNC_LOGLN("[PIGSYNC-CLI-RX] RSP_DISCONNECT - Sirloin ended call");
            break;
        }
        
        case RSP_OK:
        case RSP_BOUNTIES_ACK:
            // Acknowledged
            break;
    }
    
    // Update last packet time for session timeout (any valid packet resets)
update_last_packet_time:
    taskENTER_CRITICAL(&pendingMux);
    lastPacketTime = millis();
    taskEXIT_CRITICAL(&pendingMux);
}

void pigSyncOnSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        PIGSYNC_LOGF("[PIGSYNC-CLI-ERR] Send failed (mac=%02X:%02X:%02X:%02X:%02X:%02X)\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

// ==[ LIFECYCLE ]==

void PigSyncMode::init() {
    if (initialized) return;

    // WiFi must be in STA mode for ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Set discovery channel (channel 1)
    esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
    dataChannel = PIGSYNC_DISCOVERY_CHANNEL;

    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        PIGSYNC_LOGLN("[PIGSYNC-CLI-ERR] ESP-NOW init failed");
        return;
    }

    // Set PMK for encrypted communications
    if (esp_now_set_pmk(PIGSYNC_PMK) != ESP_OK) {
        PIGSYNC_LOGLN("[PIGSYNC-CLI-ERR] Failed to set PMK");
    }

    // Register callbacks
    esp_now_register_recv_cb(pigSyncOnRecv);
    esp_now_register_send_cb(pigSyncOnSent);

    initialized = true;
    PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] INIT");
}

// Handle keyboard input for device selection and interaction
void PigSyncMode::handleKeyboardInput() {
    M5Cardputer.update();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState state = M5Cardputer.Keyboard.keysState();

        // Handle quit/back key
        if (state.del == '`' || state.del == 27) { // ` or ESC
            PIGSYNC_LOGLN("[PIGSYNC-CLI] User quit to menu");
            stop();
            return;
        }

        // Handle device selection and connection
        if (PigSyncMode::state == PigSyncMode::State::IDLE) {
            if (!devices.empty()) {
                // Navigation keys
                if (state.del == ';') { // Previous/Up
                    if (selectedIndex > 0) {
                        selectedIndex--;
                        PIGSYNC_LOGF("[PIGSYNC-CLI] Selected device %d\n", selectedIndex);
                    }
                } else if (state.del == '.') { // Next/Down
                    if (selectedIndex < devices.size() - 1) {
                        selectedIndex++;
                        PIGSYNC_LOGF("[PIGSYNC-CLI] Selected device %d\n", selectedIndex);
                    }
                } else if (state.del == '\n' || state.del == '\r') { // Enter
                    // Connect to selected device
                    PIGSYNC_LOGF("[PIGSYNC-CLI] Connecting to device %d\n", selectedIndex);
                    connectTo(selectedIndex);
                }
            } else if (!PigSyncMode::isScanning()) {
                // No devices and not scanning - Enter starts scanning
                if (state.del == '\n' || state.del == '\r') {
                    PIGSYNC_LOGLN("[PIGSYNC-CLI] Starting device scan");
                    startDiscovery();
                }
            }
        }
    }
}

// Ensure ESP-NOW is ready (other modes may have deinitialized it)
bool PigSyncMode::ensureEspNowReady() {
    if (!initialized) {
        PIGSYNC_LOGLN("[PIGSYNC-CLI] ESP-NOW not initialized, reinitializing...");
        init();
        return initialized;
    }

    // Check if ESP-NOW is still working by trying a simple operation
    // If WiFi mode changed, ESP-NOW might be deinitialized
    wifi_mode_t currentMode;
    esp_wifi_get_mode(&currentMode);
    if (currentMode != WIFI_MODE_STA) {
        PIGSYNC_LOGLN("[PIGSYNC-CLI] WiFi mode changed, reinitializing ESP-NOW...");

        // Clean up and reinitialize
        esp_now_deinit();
        initialized = false;
        init();
        return initialized;
    }

    return true;
}

void PigSyncMode::start() {
    if (running) return;
    
    // Pause NetworkRecon - promiscuous mode conflicts with ESP-NOW
    NetworkRecon::pause();
    
    // Soft WiFi reset — keep driver alive to avoid RX buffer realloc failures
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);
    delay(100);
    
    init();
    
    // Clear state
    devices.clear();
    devices.reserve(10);
    state = State::IDLE;
    connected = false;
    memset(connectedMac, 0, 6);
    remotePMKIDCount = 0;
    remoteHSCount = 0;
    totalSynced = 0;
    syncedPMKIDs = 0;
    syncedHandshakes = 0;
    rxBufferLen = 0;
    lastError[0] = 0;
    lastHelloTime = 0;
    helloRetryCount = 0;
    dialoguePhase = 0;
    callStartTime = 0;
    phraseStartTime = 0;
    syncCompleteTime = 0;
    papaGoodbyeSelected[0] = 0;
    controlTx = {};
    resetControlQueue();
    pendingStartSync = false;
    pendingNextCapture = false;
    lastControlRspValid = false;
    
    // Clear pending flags
    pendingRingReceived = false;
    pendingHelloReceived = false;
    clearPendingChunkQueue();
    pendingCompleteReceived = false;
    pendingPurgedReceived = false;
    
    running = true;
    startDiscovery();
    
    PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] START");
}

void PigSyncMode::stop() {
    if (!running) return;

    running = false;

    disconnect();
    stopDiscovery();

    // Deinit ESP-NOW to free resources
    if (initialized) {
        esp_now_deinit();
        initialized = false;
        PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] DEINIT");
    }

    // Reclaim device list memory
    devices.clear();
    devices.shrink_to_fit();

    // Resume NetworkRecon (restores promiscuous mode)
    NetworkRecon::resume();

    PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] STOP");
}

void PigSyncMode::update() {
    static uint32_t lastDebugUpdate = 0;
    if (millis() - lastDebugUpdate > 1000) {
        lastDebugUpdate = millis();
        PIGSYNC_LOGF("[PIGSYNC-CLI-STATE] update running=%d state=%d scanning=%d pendingBeacon=%d\n",
                      running, (int)state, scanning, pendingBeaconReceived);
    }

    if (!running) return;

    // Handle keyboard input for device selection and interaction
    handleKeyboardInput();

    // Ensure ESP-NOW is still ready (other modes may interfere)
    if (!ensureEspNowReady()) {
        PIGSYNC_LOGLN("[PIGSYNC-CLI-ERR] ESP-NOW not ready, skipping update");
        return;
    }
    
    uint32_t now = millis();
    
    // ==[ CONNECTION TIMEOUT ]==
    if ((state == State::CONNECTING || state == State::RINGING) && connectStartTime > 0) {
        if (now - connectStartTime > PIGSYNC_HELLO_TIMEOUT) {
            snprintf(lastError, sizeof(lastError), "Connection timeout");
            disconnect();
            state = State::ERROR;
            return;  // Stop processing after error
        }
    }

    // ==[ CONTROL RETRY ]==
    if (controlTx.waiting && controlTx.lastSend > 0) {
        if (now - controlTx.lastSend > PIGSYNC_ACK_TIMEOUT) {
            controlTx.retries++;
            uint8_t maxRetries = getControlMaxRetries(controlTx.type);
            if (controlTx.retries >= maxRetries) {
                PIGSYNC_LOGF("[PIGSYNC-CLI-ERR] Control timeout type=0x%02X\n", controlTx.type);
                if (controlTx.type == CMD_HELLO || controlTx.type == CMD_READY) {
                    snprintf(lastError, sizeof(lastError), "Handshake timeout");
                    disconnect();
                    state = State::ERROR;
                    return;
                }
                clearControlTx();
            } else {
                PIGSYNC_LOGF("[PIGSYNC-CLI] Control retry type=0x%02X (%d/%d)\n",
                    controlTx.type, controlTx.retries, maxRetries);
                controlTx.lastSend = now;
                esp_now_send(controlTx.mac, controlTx.buf, controlTx.len);
            }
        }
    }

    // ==[ PROCESS PENDING CONTROL ACK ]==
    if (pendingControlAck) {
        taskENTER_CRITICAL(&pendingMux);
        uint8_t ackSeq = pendingControlAckSeq;
        pendingControlAck = false;
        taskEXIT_CRITICAL(&pendingMux);
        if (controlTx.waiting) {
            PigSyncHeader hdr = {};
            hdr.ack = ackSeq;
            handleControlAck(&hdr);
        }
    }
    
    // ==[ CHANNEL SWITCH TIMEOUT ]==
    // Per spec: 500ms timeout, fallback to channel 1, max 3 retries
    if (state == State::CONNECTED_WAITING_READY && readyStartTime > 0) {
        if (now - readyStartTime > PIGSYNC_READY_TIMEOUT) {
            channelRetryCount++;
            PIGSYNC_LOGF("[PIGSYNC-CLI-ERR] RSP_READY timeout (retry %d/3)\n", channelRetryCount);
            
            if (channelRetryCount >= 3) {
                // Max retries reached - abort
                snprintf(lastError, sizeof(lastError), "Channel switch failed");
                disconnect();
                state = State::ERROR;
                return;  // Don't continue processing after error
            } else {
                // Fallback to discovery channel and retry CMD_HELLO
                PIGSYNC_LOGF("[PIGSYNC-CLI] Channel switch timeout, falling back to discovery channel (retry %d/3)\n", channelRetryCount);

                // Disconnect cleanly first
                if (connected) {
                    sendCommand(CMD_DISCONNECT);
                    delay(10);
                    esp_now_del_peer(connectedMac);
                    connected = false;
                }

                // Switch back to discovery channel
                esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
                delay(PIGSYNC_CHANNEL_SWITCH_MS);
                dataChannel = PIGSYNC_DISCOVERY_CHANNEL;

                // Re-add peer on discovery channel (unencrypted for new handshake)
                esp_now_peer_info_t peerInfo = {};
                memcpy(peerInfo.peer_addr, connectedMac, 6);
                peerInfo.channel = PIGSYNC_DISCOVERY_CHANNEL;
                peerInfo.encrypt = false;
                esp_err_t addResult = esp_now_add_peer(&peerInfo);
                PIGSYNC_LOGF("[PIGSYNC-CLI] Peer re-added on discovery channel, result=%d\n", addResult);

                // Reset state for retry
                connected = true;
                state = State::CONNECTING;
                connectStartTime = now;  // Reset connection timeout for retry
                lastHelloTime = 0;
                helloRetryCount = 0;
                PIGSYNC_LOGF("[PIGSYNC-CLI] Retrying CMD_HELLO to %02X:%02X:%02X:%02X:%02X:%02X\n",
                    connectedMac[0], connectedMac[1], connectedMac[2],
                    connectedMac[3], connectedMac[4], connectedMac[5]);
                sendHello();
            }
        }
    }
    
    // ==[ CHUNK/TRANSFER TIMEOUT ]==
    if ((state == State::WAITING_CHUNKS || state == State::SYNCING) && progress.inProgress) {
        if (now - progress.startTime > PIGSYNC_TRANSFER_TIMEOUT) {
            PIGSYNC_LOGLN("[PIGSYNC-CLI-ERR] Transfer timeout");
            snprintf(lastError, sizeof(lastError), "Transfer timeout");
            progress.inProgress = false;
            // Stay connected, allow retry
            state = State::CONNECTED;
        }
    }
    
    // ==[ PROCESS PENDING ERROR ]==
    if (pendingErrorReceived) {
        taskENTER_CRITICAL(&pendingMux);
        uint8_t errCode = pendingErrorCode;
        pendingErrorReceived = false;
        taskEXIT_CRITICAL(&pendingMux);
        
        const char* errMsg = "Unknown error";
        switch (errCode) {
            case PIGSYNC_ERR_INVALID_CMD: errMsg = "Invalid command"; break;
            case PIGSYNC_ERR_INVALID_INDEX: errMsg = "Invalid index"; break;
            case PIGSYNC_ERR_BUSY: errMsg = "Son is busy"; break;
            case PIGSYNC_ERR_NO_CAPTURES: errMsg = "No captures"; break;
            case PIGSYNC_ERR_TIMEOUT: errMsg = "Son timeout"; break;
            case PIGSYNC_ERR_CRC_FAIL: errMsg = "CRC failed"; break;
            case PIGSYNC_ERR_NOT_READY: errMsg = "Son not ready"; break;
            case PIGSYNC_ERR_SERIALIZE_FAIL: errMsg = "Serialize failed"; break;
            case PIGSYNC_ERR_BUFFER_OVERFLOW: errMsg = "Buffer overflow"; break;
        }
        snprintf(lastError, sizeof(lastError), "%s", errMsg);
        PIGSYNC_LOGF("[PIGSYNC-CLI-ERR] From SON: %s\n", errMsg);
        
        // If we were syncing, abort current transfer
        if (state == State::WAITING_CHUNKS || state == State::SYNCING) {
            progress.inProgress = false;
            state = State::CONNECTED;
        }
    }
    
    // ==[ PROCESS PENDING DISCONNECT FROM SIRLOIN ]==
    if (pendingDisconnectReceived) {
        taskENTER_CRITICAL(&pendingMux);
        pendingDisconnectReceived = false;
        taskEXIT_CRITICAL(&pendingMux);
        
        PIGSYNC_LOGLN("[PIGSYNC-CLI] Sirloin ended call gracefully");
        // Don't send CMD_DISCONNECT back - just clean up locally
        if (connected) {
            esp_now_del_peer(connectedMac);
            connected = false;
        }
        memset(connectedMac, 0, 6);

        // Fallback to discovery channel
        if (dataChannel != PIGSYNC_DISCOVERY_CHANNEL) {
            esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
            dataChannel = PIGSYNC_DISCOVERY_CHANNEL;
            PIGSYNC_LOGLN("[PIGSYNC-CLI] Fell back to discovery channel after RSP_DISCONNECT");
        }

        controlTx = {};
        resetControlQueue();
        pendingStartSync = false;
        pendingNextCapture = false;
        lastControlRspValid = false;
        state = State::IDLE;
    }
    
    // ==[ SESSION TIMEOUT - Sirloin stopped responding ]==
    if (connected && lastPacketTime > 0) {
        if (now - lastPacketTime > SESSION_TIMEOUT) {
            PIGSYNC_LOGLN("[PIGSYNC-CLI-ERR] Session timeout - Sirloin unresponsive");
            snprintf(lastError, sizeof(lastError), "Connection lost");
            // Clean disconnect without notifying (Sirloin already gone)
            esp_now_del_peer(connectedMac);
            connected = false;
            memset(connectedMac, 0, 6);

            // Fallback to discovery channel
            if (dataChannel != PIGSYNC_DISCOVERY_CHANNEL) {
                esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
                dataChannel = PIGSYNC_DISCOVERY_CHANNEL;
                PIGSYNC_LOGLN("[PIGSYNC-CLI] Fell back to discovery channel after session timeout");
            }

            state = State::IDLE;
            lastPacketTime = 0;
        }
    }
    
    // ==[ DISCOVERY ]==
    if (scanning && state == State::SCANNING) {
        if (now - lastDiscoveryTime >= PIGSYNC_DISCOVERY_INTERVAL) {
            lastDiscoveryTime = now;
            sendDiscover();
        }
    }

    // Prune stale devices even when not scanning (handles MAC changes after mode switch)
    if (!devices.empty()) {
        devices.erase(
            std::remove_if(devices.begin(), devices.end(),
                [now](const SirloinDevice& d) { return now - d.lastSeen > 5000; }),
            devices.end()
        );
        if (selectedIndex >= devices.size() && !devices.empty()) {
            selectedIndex = devices.size() - 1;
        }
    }
    
    // ==[ PROCESS PENDING BEACON ]==
    if (pendingBeaconReceived) {
        taskENTER_CRITICAL(&pendingMux);
        uint8_t beaconMac[6];
        memcpy(beaconMac, pendingBeaconMac, 6);
        int8_t beaconRSSI = pendingBeaconRSSI;
        uint16_t beaconPending = pendingBeaconPending;
        uint8_t beaconFlags = pendingBeaconFlags;
        pendingBeaconReceived = false;
        taskEXIT_CRITICAL(&pendingMux);
        
        PIGSYNC_LOGF("[PIGSYNC-CLI-RX] Beacon MAC=%02X:%02X:%02X:%02X:%02X:%02X pending=%d\n",
                      beaconMac[0], beaconMac[1], beaconMac[2], beaconMac[3], beaconMac[4], beaconMac[5], beaconPending);
        
        // Check if already in device list
        bool found = false;
        for (auto& dev : devices) {
            if (memcmp(dev.mac, beaconMac, 6) == 0) {
                dev.rssi = beaconRSSI;
                dev.pendingCaptures = beaconPending;
                dev.flags = beaconFlags;
                dev.lastSeen = now;
                found = true;
                PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] Updated device");
                break;
            }
        }
        
        if (!found && devices.size() < 10) {
            SirloinDevice dev;
            memcpy(dev.mac, beaconMac, 6);
            dev.rssi = beaconRSSI;
            dev.pendingCaptures = beaconPending;
            dev.flags = beaconFlags;
            dev.lastSeen = now;
            dev.syncing = false;
            dev.hasGruntInfo = false;
            snprintf(dev.name, sizeof(dev.name), "SIRLOIN");
            devices.push_back(dev);
            PIGSYNC_LOGF("[PIGSYNC-CLI-STATE] Added device total=%d\n", devices.size());
        }
    }
    
    // ==[ PROCESS PENDING GRUNT (Phase 3) ]==
    if (pendingGruntReceived) {
        taskENTER_CRITICAL(&pendingMux);
        uint8_t gruntMac[6];
        memcpy(gruntMac, pendingGruntMac, 6);
        uint8_t gruntFlags = pendingGruntFlags;
        uint8_t gruntCaptures = pendingGruntCaptureCount;
        uint8_t gruntBattery = pendingGruntBattery;
        uint8_t gruntStorage = pendingGruntStorage;
        uint32_t gruntTime = pendingGruntUnixTime;
        uint16_t gruntUptime = pendingGruntUptime;
        char gruntName[5];
        memcpy(gruntName, pendingGruntName, 4);
        gruntName[4] = '\0';
        pendingGruntReceived = false;
        taskEXIT_CRITICAL(&pendingMux);
        
        PIGSYNC_LOGF("[PIGSYNC-CLI] Grunt from %s batt=%d%% caps=%d\n", gruntName, gruntBattery, gruntCaptures);
        
        // Update or add device - try MAC first, then name fallback for MAC randomization
        bool found = false;
        for (auto& dev : devices) {
            char priorName[16];
            strncpy(priorName, dev.name, sizeof(priorName) - 1);
            priorName[sizeof(priorName) - 1] = '\0';
            // Primary: MAC address match
            if (memcmp(dev.mac, gruntMac, 6) == 0) {
                dev.pendingCaptures = gruntCaptures;
                dev.flags = gruntFlags & BEACON_FLAG_ALERT_MASK;
                dev.lastSeen = now;
                dev.batteryPercent = gruntBattery;
                dev.storagePercent = gruntStorage;
                dev.moodTier = (gruntFlags >> BEACON_FLAG_MOOD_SHIFT) & 0x07;
                dev.rtcTime = gruntTime;
                dev.uptimeMin = gruntUptime;
                dev.hasGruntInfo = true;
                if (gruntName[0]) {
                    snprintf(dev.name, sizeof(dev.name), "%s", gruntName);
                    if (strncmp(priorName, dev.name, sizeof(dev.name)) != 0 &&
                        strncmp(dev.name, "SIRLOIN", sizeof(dev.name)) != 0) {
                        taskENTER_CRITICAL(&pendingMux);
                        pendingNameReveal = true;
                        strncpy(pendingNameRevealName, dev.name, sizeof(pendingNameRevealName) - 1);
                        pendingNameRevealName[sizeof(pendingNameRevealName) - 1] = '\0';
                        taskEXIT_CRITICAL(&pendingMux);
                    }
                }
                found = true;
                PIGSYNC_LOGF("[PIGSYNC-CLI-STATE] Updated device %s by MAC\n", gruntName);
                break;
            }
            // Fallback: Name match (handles MAC randomization during mode changes)
            else if (dev.hasGruntInfo && gruntName[0] &&
                     strncmp(dev.name, gruntName, 4) == 0) {
                // Update MAC address (device randomized it) and all other info
                memcpy(dev.mac, gruntMac, 6);
                dev.pendingCaptures = gruntCaptures;
                dev.flags = gruntFlags & BEACON_FLAG_ALERT_MASK;
                dev.lastSeen = now;
                dev.batteryPercent = gruntBattery;
                dev.storagePercent = gruntStorage;
                dev.moodTier = (gruntFlags >> BEACON_FLAG_MOOD_SHIFT) & 0x07;
                dev.rtcTime = gruntTime;
                dev.uptimeMin = gruntUptime;
                dev.hasGruntInfo = true;
                if (gruntName[0] &&
                    strncmp(priorName, dev.name, sizeof(dev.name)) != 0 &&
                    strncmp(dev.name, "SIRLOIN", sizeof(dev.name)) != 0) {
                    taskENTER_CRITICAL(&pendingMux);
                    pendingNameReveal = true;
                    strncpy(pendingNameRevealName, dev.name, sizeof(pendingNameRevealName) - 1);
                    pendingNameRevealName[sizeof(pendingNameRevealName) - 1] = '\0';
                    taskEXIT_CRITICAL(&pendingMux);
                }
                found = true;
                PIGSYNC_LOGF("[PIGSYNC-CLI-STATE] Updated device %s by name (MAC changed)\n", gruntName);
                break;
            }
        }
        
        if (!found && devices.size() < 10) {
            SirloinDevice dev = {};
            memcpy(dev.mac, gruntMac, 6);
            dev.pendingCaptures = gruntCaptures;
            dev.flags = gruntFlags & BEACON_FLAG_ALERT_MASK;
            dev.lastSeen = now;
            dev.syncing = false;
            dev.batteryPercent = gruntBattery;
            dev.storagePercent = gruntStorage;
            dev.moodTier = (gruntFlags >> BEACON_FLAG_MOOD_SHIFT) & 0x07;
            dev.rtcTime = gruntTime;
            dev.uptimeMin = gruntUptime;
            dev.hasGruntInfo = true;
            if (gruntName[0]) {
                snprintf(dev.name, sizeof(dev.name), "%s", gruntName);
                if (strncmp(dev.name, "SIRLOIN", sizeof(dev.name)) != 0) {
                    taskENTER_CRITICAL(&pendingMux);
                    pendingNameReveal = true;
                    strncpy(pendingNameRevealName, dev.name, sizeof(pendingNameRevealName) - 1);
                    pendingNameRevealName[sizeof(pendingNameRevealName) - 1] = '\0';
                    taskEXIT_CRITICAL(&pendingMux);
                }
            } else {
                snprintf(dev.name, sizeof(dev.name), "SIRLOIN");
            }
            devices.push_back(dev);
            PIGSYNC_LOGF("[PIGSYNC-CLI-STATE] Added device from grunt total=%d\n", devices.size());
        }
    }
    
    // ==[ PROCESS PENDING TIME SYNC (Phase 3) ]==
    if (pendingTimeSyncReceived) {
        taskENTER_CRITICAL(&pendingMux);
        uint8_t rtcValid = pendingTimeSyncValid;
        uint32_t unixTime = pendingTimeSyncUnix;
        uint32_t rtt = pendingTimeSyncRtt;
        pendingTimeSyncReceived = false;
        taskEXIT_CRITICAL(&pendingMux);
        
        if (rtcValid && unixTime > 0) {
            // Adjust for half of RTT (one-way latency)
            uint32_t adjustedTime = unixTime + (rtt / 2000);  // RTT in ms -> sec/2
            
            // Set Porkchop's system time from Sirloin's RTC
            struct timeval tv;
            tv.tv_sec = adjustedTime;
            tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            
            PIGSYNC_LOGF("[PIGSYNC-CLI] Time synced from Sirloin: %lu (RTT=%lums)\n", adjustedTime, rtt);
        } else {
            PIGSYNC_LOGLN("[PIGSYNC-CLI] Sirloin RTC not valid, skipping time sync");
        }
    }

    // ==[ PROCESS PENDING RING ]==
    if (pendingRingReceived) {
        uint32_t ringAt = 0;
        taskENTER_CRITICAL(&pendingMux);
        pendingRingReceived = false;
        ringAt = pendingRingAt;
        pendingRingAt = 0;
        taskEXIT_CRITICAL(&pendingMux);

        if (state == State::CONNECTING || state == State::RINGING) {
            state = State::RINGING;
            PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] RINGING (awaiting accept)");
            connectStartTime = ringAt ? ringAt : now;  // Extend HELLO timeout while rings continue
        }
    }
    
    // ==[ PROCESS PENDING HELLO ]==
    if (pendingHelloReceived) {
        taskENTER_CRITICAL(&pendingMux);
        remotePMKIDCount = pendingPMKIDCount;
        remoteHSCount = pendingHSCount;
        dialogueId = pendingDialogueId;
        remoteMood = pendingMood;
        sessionId = pendingSessionId;
        dataChannel = pendingDataChannel;
        bool clearControl = pendingHelloClearControl;
        pendingHelloClearControl = false;
        pendingHelloReceived = false;
        taskEXIT_CRITICAL(&pendingMux);

        if (clearControl) {
            clearControlTx();
        }

        PIGSYNC_LOGF("[PIGSYNC-CLI] Processing RSP_HELLO: PMKIDs=%d HS=%d mood=%d sessionId=0x%04X dataChannel=%d\n",
            remotePMKIDCount, remoteHSCount, remoteMood, sessionId, dataChannel);
        
        PIGSYNC_LOGF("[PIGSYNC-CLI] RSP_HELLO received, sessionId=0x%04X, switching to data channel %d\n", sessionId, dataChannel);

        // Brief delay to allow server to switch channels first
        delay(50);

        // Remove peer from current channel before switching
        esp_now_del_peer(connectedMac);
        PIGSYNC_LOGF("[PIGSYNC-CLI] Removed peer from discovery channel\n");

        // Switch to data channel
        esp_wifi_set_channel(dataChannel, WIFI_SECOND_CHAN_NONE);
        delay(PIGSYNC_CHANNEL_SWITCH_MS);
        PIGSYNC_LOGF("[PIGSYNC-CLI] Switched to data channel %d\n", dataChannel);
        
        // Re-add peer on new channel WITH ENCRYPTION (Sirloin expects encrypted CMD_READY)
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, connectedMac, 6);
        peerInfo.channel = dataChannel;
        peerInfo.encrypt = true;
        memcpy(peerInfo.lmk, PIGSYNC_LMK, 16);
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            PIGSYNC_LOGLN("[PIGSYNC-CLI] Failed to add peer on data channel");
            // Fallback to discovery channel on peer add failure
            if (dataChannel != PIGSYNC_DISCOVERY_CHANNEL) {
                esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
                dataChannel = PIGSYNC_DISCOVERY_CHANNEL;
                PIGSYNC_LOGLN("[PIGSYNC-CLI] Fell back to discovery channel after peer add failure");
            }
            state = State::ERROR;
            return;
        }
        
        // Transition to waiting for RSP_READY
        state = State::CONNECTED_WAITING_READY;
        readyStartTime = now;  // Track timeout for RSP_READY
        
        // Send CMD_READY to confirm we switched
        sendReady();
    }
    
    // ==[ PROCESS PENDING READY ]==
    if (pendingReadyReceived && state == State::CONNECTED_WAITING_READY) {
        taskENTER_CRITICAL(&pendingMux);
        bool clearControl = pendingReadyClearControl;
        pendingReadyClearControl = false;
        pendingReadyReceived = false;
        taskEXIT_CRITICAL(&pendingMux);

        if (clearControl) {
            clearControlTx();
        }
        
        uint32_t handshakeTime = now - readyStartTime;
        PIGSYNC_LOGF("[PIGSYNC-CLI] RSP_READY received, channel handshake complete (%lums)\n", handshakeTime);
        
        // NOTE: Peer already encrypted from channel switch (no need for upgradePeerEncryption here)
        
        state = State::CONNECTED;
        dialoguePhase = 0;
        phraseStartTime = now;
        callStartTime = now;

        // Dialogue now handled by terminal system
        
        PIGSYNC_LOGF("[PIGSYNC-CLI-STATE] CONNECTED PMKIDs=%d HS=%d\n", remotePMKIDCount, remoteHSCount);
        
        // Send bounties if we have any
        sendBounties();
        
        // Phase 3: Request time sync from Sirloin (it has persistent RTC)
        sendTimeSync();
        
        // Start sync if there's data
        if (remotePMKIDCount > 0 || remoteHSCount > 0) {
            pendingStartSync = true;
        } else {
            // No data - go to goodbye
            dialoguePhase = 2;
            phraseStartTime = now;
            
            // Select Papa's goodbye (0 captures = tier 0)
            strncpy(papaGoodbyeSelected, selectPapaGoodbye(0), sizeof(papaGoodbyeSelected) - 1);
            sendPurge();
        }
    }
    
    // ==[ PROCESS PENDING CHUNK ]==
    if (pendingChunkReceived) {
        PendingChunkSlot localQueue[PENDING_CHUNK_QUEUE_SIZE];
        uint8_t localCount = 0;

        taskENTER_CRITICAL(&pendingMux);
        for (uint8_t i = 0; i < PENDING_CHUNK_QUEUE_SIZE; i++) {
            if (pendingChunkQueue[i].used) {
                localQueue[localCount++] = pendingChunkQueue[i];
                pendingChunkQueue[i].used = false;
            }
        }
        pendingChunkCount = 0;
        pendingChunkReceived = false;
        taskEXIT_CRITICAL(&pendingMux);

        for (uint8_t i = 0; i < localCount; i++) {
            uint16_t seq = localQueue[i].seq;
            uint16_t total = localQueue[i].total;
            uint16_t chunkLen = localQueue[i].len;

            totalChunks = total;
            
            // Validate sequence - only accept expected seq or retransmissions
            // Expected: receivedChunks (next chunk) or receivedChunks-1 (retransmit of last)
            bool validSeq = (seq == receivedChunks) || 
                            (receivedChunks > 0 && seq == receivedChunks - 1);
            
            if (validSeq) {
                // Calculate offset
                uint16_t offset = seq * PIGSYNC_MAX_PAYLOAD;
                if (offset + chunkLen <= RX_BUFFER_SIZE) {
                    memcpy(rxBuffer + offset, localQueue[i].data, chunkLen);
                    
                    // Only advance if this is new data (not a retransmit)
                    if (seq == receivedChunks) {
                        if (offset + chunkLen > rxBufferLen) {
                            rxBufferLen = offset + chunkLen;
                        }
                        receivedChunks++;
                    }
                    
                    progress.currentChunk = receivedChunks;
                    progress.totalChunks = totalChunks;
                    progress.bytesReceived = rxBufferLen;
                    
                    // Send ACK (always ACK to stop retransmits)
                    sendAckChunk(seq);
                }
            } else {
                PIGSYNC_LOGF("[PIGSYNC-CLI-ERR] Out-of-order chunk got=%d expected=%d\n", seq, receivedChunks);
                // Don't ACK - sender will retry correct sequence
            }
        }
    }
    
    // ==[ PROCESS PENDING COMPLETE ]==
    if (pendingCompleteReceived) {
        taskENTER_CRITICAL(&pendingMux);
        uint16_t totalBytes = pendingTotalBytes;
        uint32_t crc = pendingCRC;
        pendingCompleteReceived = false;
        taskEXIT_CRITICAL(&pendingMux);

        if (totalBytes > RX_BUFFER_SIZE) {
            snprintf(lastError, sizeof(lastError), "Buffer overflow");
            progress.inProgress = false;
            rxBufferLen = 0;
            receivedChunks = 0;
            disconnect();
            state = State::ERROR;
            return;
        }
        
        // Verify CRC
        uint32_t calcCRC = calculateCRC32(rxBuffer, rxBufferLen);
        
        if (calcCRC == crc) {
            // Save capture
            bool success = false;
            if (currentType == CAPTURE_TYPE_PMKID) {
                success = savePMKID(rxBuffer, rxBufferLen);
                if (success) syncedPMKIDs++;
            } else {
                success = saveHandshake(rxBuffer, rxBufferLen);
                if (success) syncedHandshakes++;
            }
            
            if (success) {
                totalSynced++;
                sendMarkSynced(currentType, currentIndex);
                
                // Invoke capture callback
                if (onCaptureCb) {
                    onCaptureCb(currentType, rxBuffer, rxBufferLen);
                }
            }
            
            // Request next
            currentIndex++;
            rxBufferLen = 0;
            receivedChunks = 0;
            progress.inProgress = false;
            pendingNextCapture = true;
        } else {
            snprintf(lastError, sizeof(lastError), "CRC mismatch");
            // Retry same capture
            rxBufferLen = 0;
            receivedChunks = 0;
            sendStartSync(currentType, currentIndex);
        }
    }
    
    // ==[ PROCESS PENDING PURGED ]==
    if (pendingPurgedReceived) {
        taskENTER_CRITICAL(&pendingMux);
        uint16_t purged = pendingPurgedCount;
        uint8_t bountyMatches = pendingBountyMatches;
        pendingPurgedReceived = false;
        taskEXIT_CRITICAL(&pendingMux);
        
        lastBountyMatches = bountyMatches;

        // Dialogue now handled by terminal system
        
        dialoguePhase = 2;  // Goodbye phase
        phraseStartTime = now;
        state = State::SYNC_COMPLETE;
        syncCompleteTime = now;  // Track when sync completed
        
        if (onSyncCompleteCb) {
            onSyncCompleteCb(syncedPMKIDs, syncedHandshakes);
        }
        
        // Log sync completion
        SDLog::log("SON-OF-PIG", "Sync complete: %d PMKIDs, %d HS, %d bounties",
                   syncedPMKIDs, syncedHandshakes, bountyMatches);
    }

    // ==[ GOODBYE PHASE COMPLETE ]==
    // Let terminal timing control dialogue flow - phases advance naturally
    
    // ==[ CLEANUP AFTER SYNC COMPLETE ]==
    // Wait for goodbye toast to expire, then disconnect and free resources
    if (state == State::SYNC_COMPLETE && syncCompleteTime > 0) {
        if (now - syncCompleteTime > PIGSYNC_TOAST_DURATION + 500) {  // +500ms grace period
            PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] SYNC_COMPLETE disconnecting");
            disconnect();
            syncCompleteTime = 0;  // Prevent repeated disconnect
        }
    }
    
    // Toast system removed - dialogue now in terminal

    // ==[ DEFERRED CONTROL ACTIONS ]==
    if (pendingNextCapture && !controlTx.waiting && controlQueueCount == 0) {
        pendingNextCapture = false;
        requestNextCapture();
    }
    if (pendingStartSync && state == State::CONNECTED && !controlTx.waiting && controlQueueCount == 0) {
        pendingStartSync = false;
        startSync();
    }
}

bool PigSyncMode::consumeNameReveal(char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) {
        return false;
    }
    bool hasName = false;
    taskENTER_CRITICAL(&pendingMux);
    if (pendingNameReveal) {
        pendingNameReveal = false;
        strncpy(buffer, pendingNameRevealName, bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
        hasName = true;
    }
    taskEXIT_CRITICAL(&pendingMux);
    return hasName;
}

// ==[ DISCOVERY ]==

void PigSyncMode::startDiscovery() {
    devices.clear();
    scanning = true;
    state = State::SCANNING;
    lastDiscoveryTime = 0;
    discoveryStartTime = millis();
    
    // Send first discovery immediately
    sendDiscover();
}

void PigSyncMode::stopDiscovery() {
    scanning = false;
    if (state == State::SCANNING) {
        state = State::IDLE;
    }
}

bool PigSyncMode::isScanning() {
    return scanning;
}

bool PigSyncMode::hasValidDevices() {
    // Remove stale devices
    uint32_t now = millis();
    devices.erase(
        std::remove_if(devices.begin(), devices.end(),
            [now](const SirloinDevice& d) { return now - d.lastSeen > 5000; }),
        devices.end()
    );
    return !devices.empty();
}

// ==[ CONNECTION ]==

bool PigSyncMode::connectTo(uint8_t deviceIndex) {
    if (deviceIndex >= devices.size()) return false;
    
    // Guard against duplicate connect calls - set state atomically
    if (state == State::CONNECTING || state == State::RINGING || isConnected()) {
        PIGSYNC_LOGLN("[PIGSYNC-CLI-STATE] connectTo ignored - already connecting/connected");
        return false;
    }
    
    // Set state IMMEDIATELY to prevent race conditions
    state = State::CONNECTING;
    pendingRingReceived = false;
    
    stopDiscovery();

    SirloinDevice& dev = devices[deviceIndex];
    memcpy(connectedMac, dev.mac, 6);

    // Ensure we're on discovery channel before HELLO
    esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
    dataChannel = PIGSYNC_DISCOVERY_CHANNEL;
    
    // Add as peer (unencrypted for initial handshake on discovery channel)
    memset(&sirloinPeer, 0, sizeof(sirloinPeer));
    memcpy(sirloinPeer.peer_addr, dev.mac, 6);
    sirloinPeer.channel = PIGSYNC_DISCOVERY_CHANNEL;
    sirloinPeer.encrypt = false;  // No encryption for CMD_HELLO - Sirloin doesn't have our LMK yet
    
    esp_now_del_peer(dev.mac);  // Remove if exists
    esp_err_t addErr = esp_now_add_peer(&sirloinPeer);
    if (addErr != ESP_OK) {
        PIGSYNC_LOGF("[PIGSYNC-CLI-ERR] Failed to add peer err=%d\n", addErr);
        snprintf(lastError, sizeof(lastError), "Failed to add peer");
        state = State::IDLE;
        return false;
    }
    
    connected = true;
    lastPacketTime = millis();  // Initialize session timeout
    // state already set to CONNECTING at top of function
    connectStartTime = millis();  // Start connection timeout
    lastHelloTime = 0;
    helloRetryCount = 0;
    syncCompleteTime = 0;  // Reset sync complete tracking
    channelRetryCount = 0;  // Reset channel switch retry counter
    dev.syncing = true;
    selectedIndex = deviceIndex;
    
    // Small delay to let peer setup stabilize
    delay(10);

    // Send HELLO
    sendHello();
    
    return true;
}

void PigSyncMode::disconnect() {
    if (connected) {
        // Notify Sirloin before disconnecting
        sendCommand(CMD_DISCONNECT);
        delay(10);  // Brief delay to let packet send
        esp_now_del_peer(connectedMac);
        connected = false;
    }
    memset(connectedMac, 0, 6);

    // Always fallback to discovery channel after disconnection
    if (dataChannel != PIGSYNC_DISCOVERY_CHANNEL) {
        esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
        dataChannel = PIGSYNC_DISCOVERY_CHANNEL;
        PIGSYNC_LOGLN("[PIGSYNC-CLI] Fell back to discovery channel after disconnect");
    }

    callStartTime = 0;
    connectStartTime = 0;
    phraseStartTime = 0;
    dialoguePhase = 0;
    syncCompleteTime = 0;
    controlTx = {};
    resetControlQueue();
    clearPendingChunkQueue();
    pendingRingReceived = false;
    pendingStartSync = false;
    pendingNextCapture = false;
    lastControlRspValid = false;
    state = State::IDLE;
    lastHelloTime = 0;
    helloRetryCount = 0;
}

bool PigSyncMode::isConnected() {
    return connected && (state == State::CONNECTED || state == State::SYNCING || 
                         state == State::WAITING_CHUNKS || state == State::SYNC_COMPLETE);
}

bool PigSyncMode::isConnecting() {
    return state == State::CONNECTING || state == State::RINGING;
}

const SirloinDevice* PigSyncMode::getConnectedDevice() {
    if (!connected || selectedIndex >= devices.size()) return nullptr;
    return &devices[selectedIndex];
}

// ==[ UI HELPERS ]==

void PigSyncMode::getDeviceDisplayName(uint8_t index, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;

    const SirloinDevice* device = getDevice(index);
    if (!device) {
        snprintf(buffer, bufferSize, "No device");
        return;
    }

    // Format: "SIRLOIN [Name] RSSI: -XXdBm Caps: XXX"
    char name[16];
    if (device->hasGruntInfo && device->name[0]) {
        snprintf(name, sizeof(name), "%s", device->name);
    } else {
        snprintf(name, sizeof(name), "SIRLOIN");
    }

    char flags[32] = "";
    if (device->flags & FLAG_HUNTING) strcat(flags, "HUNT ");
    if (device->flags & FLAG_BUFFER_FULL) strcat(flags, "FULL ");
    if (device->flags & FLAG_CALL_ACTIVE) strcat(flags, "BUSY ");

    if (flags[0]) {
        // Remove trailing space
        flags[strlen(flags)-1] = '\0';
        snprintf(buffer, bufferSize, "%s RSSI:%ddBm Caps:%d [%s]",
                name, device->rssi, device->pendingCaptures, flags);
    } else {
        snprintf(buffer, bufferSize, "%s RSSI:%ddBm Caps:%d",
                name, device->rssi, device->pendingCaptures);
    }
}

void PigSyncMode::getStatusMessage(char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;

    if (!running) {
        snprintf(buffer, bufferSize, "PIGSYNC OFFLINE");
        return;
    }

    switch (state) {
        case State::IDLE:
            if (isScanning()) {
                if (devices.empty()) {
                    snprintf(buffer, bufferSize, "SCANNING... No Sirloin found");
                } else {
                    snprintf(buffer, bufferSize, "SCANNING... %d device(s) found", devices.size());
                }
            } else {
                snprintf(buffer, bufferSize, "READY - Press C to connect");
            }
            break;

        case State::CONNECTING:
            snprintf(buffer, bufferSize, "CONNECTING...");
            break;

        case State::RINGING:
            snprintf(buffer, bufferSize, "RINGING... Waiting for Sirloin");
            break;

        case State::CONNECTED_WAITING_READY:
            snprintf(buffer, bufferSize, "HANDSHAKE...");
            break;

        case State::CONNECTED:
            if (remotePMKIDCount > 0 || remoteHSCount > 0) {
                snprintf(buffer, bufferSize, "CONNECTED - Auto-sync starting...");
            } else {
                snprintf(buffer, bufferSize, "CONNECTED - No data to sync");
            }
            break;

        case State::SYNCING:
        case State::WAITING_CHUNKS:
            if (progress.inProgress) {
                uint8_t percent = PigSyncMode::getSyncProgress();
                snprintf(buffer, bufferSize, "SYNCING... %d%%", percent);
            } else {
                snprintf(buffer, bufferSize, "SYNCING...");
            }
            break;

        case State::SYNC_COMPLETE:
            snprintf(buffer, bufferSize, "SYNC COMPLETE!");
            break;

        case State::ERROR:
            if (lastError[0]) {
                snprintf(buffer, bufferSize, "ERROR: %s", lastError);
            } else {
                snprintf(buffer, bufferSize, "CONNECTION ERROR");
            }
            break;
    }
}

// ==[ SYNC OPERATIONS ]==

bool PigSyncMode::startSync() {
    if (!connected) return false;
    if (remotePMKIDCount == 0 && remoteHSCount == 0) return false;
    
    // Free caches and suspend sprites for maximum heap before data transfer
    WPASec::freeCacheMemory();
    WiGLE::freeUploadedListMemory();
    delay(200);
    yield();
    
    // Guard: ensure enough contiguous heap for reliable transfer
    HeapGates::GateStatus gate = HeapGates::checkGate(0, HeapPolicy::kPigSyncMinContig);
    if (!HeapGates::canMeet(gate, lastError, sizeof(lastError))) {
        return false;
    }
    
    state = State::SYNCING;
    dialoguePhase = 1;  // Syncing phase
    currentIndex = 0;
    totalSynced = 0;
    syncedPMKIDs = 0;
    syncedHandshakes = 0;
    rxBufferLen = 0;
    receivedChunks = 0;  // Reset chunk counter
    totalChunks = 0;
    
    progress.inProgress = true;
    progress.startTime = millis();
    progress.bytesReceived = 0;
    progress.currentChunk = 0;
    progress.totalChunks = 0;
    
    // Start with PMKIDs first
    if (remotePMKIDCount > 0) {
        currentType = CAPTURE_TYPE_PMKID;
        sendStartSync(CAPTURE_TYPE_PMKID, 0);
    } else {
        currentType = CAPTURE_TYPE_HANDSHAKE;
        sendStartSync(CAPTURE_TYPE_HANDSHAKE, 0);
    }
    
    return true;
}

void PigSyncMode::abortSync() {
    if (!connected) return;
    
    PigSyncHeader pkt;
    pkt.magic = PIGSYNC_MAGIC;
    pkt.version = PIGSYNC_VERSION;
    pkt.type = CMD_ABORT;
    pkt.flags = 0;
    
    esp_now_send(connectedMac, (uint8_t*)&pkt, sizeof(pkt));
    
    state = State::CONNECTED;
    progress.inProgress = false;
}

bool PigSyncMode::isSyncing() {
    return state == State::SYNCING || state == State::WAITING_CHUNKS;
}

bool PigSyncMode::isSyncComplete() {
    return state == State::SYNC_COMPLETE;
}

uint8_t PigSyncMode::getSyncProgress() {
    if (progress.totalChunks == 0) return 0;
    return (progress.currentChunk * 100) / progress.totalChunks;
}

// ==[ DIALOGUE ]==

uint32_t PigSyncMode::getCallDuration() {
    if (callStartTime == 0) return 0;
    return millis() - callStartTime;
}

uint8_t PigSyncMode::getDialoguePhase() {
    return dialoguePhase;
}

const char* PigSyncMode::getPapaHelloPhrase() {
    return PAPA_HELLO[dialogueId % DIALOGUE_TRACK_COUNT];
}

const char* PigSyncMode::getPapaGoodbyePhrase() {
    if (papaGoodbyeSelected[0]) return papaGoodbyeSelected;
    return selectPapaGoodbye(totalSynced);
}

const char* PigSyncMode::getSonHelloPhrase() {
    return SON_HELLO[dialogueId % DIALOGUE_TRACK_COUNT];
}

const char* PigSyncMode::getSonGoodbyePhrase() {
    return SON_GOODBYE[dialogueId % DIALOGUE_TRACK_COUNT];
}

// ==[ PROTOCOL HELPERS ]==

void PigSyncMode::sendCommand(uint8_t type) {
    // Generic command sender for simple commands (header only)
    PigSyncHeader pkt;
    initHeader(&pkt, type, reliability.nextSeq(), reliability.lastRxSeq, sessionId);
    esp_now_send(connectedMac, (uint8_t*)&pkt, sizeof(pkt));
}

void PigSyncMode::sendDiscover() {
    // Broadcast discovery - need to add broadcast peer temporarily
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    esp_now_peer_info_t broadcastPeer;
    memset(&broadcastPeer, 0, sizeof(broadcastPeer));
    memcpy(broadcastPeer.peer_addr, broadcast, 6);
    broadcastPeer.channel = PIGSYNC_DISCOVERY_CHANNEL;
    broadcastPeer.encrypt = false;  // Discovery is unencrypted
    
    esp_now_del_peer(broadcast);
    esp_err_t addErr = esp_now_add_peer(&broadcastPeer);

    CmdDiscover pkt;
    initHeader(&pkt.hdr, CMD_DISCOVER, 0, 0, 0);  // sessionId=0 for discovery
    WiFi.macAddress(pkt.pops_mac);

    esp_err_t sendErr = esp_now_send(broadcast, (uint8_t*)&pkt, sizeof(pkt));
    PIGSYNC_LOGF("[PIGSYNC-CLI-TX] CMD_DISCOVER add=%d send=%d\n", addErr, sendErr);

    // Small delay before removing peer
    delay(5);
    esp_now_del_peer(broadcast);
}

void PigSyncMode::sendHello() {
    PIGSYNC_LOGF("[PIGSYNC-CLI-TX] CMD_HELLO to %02X:%02X:%02X:%02X:%02X:%02X\n",
        connectedMac[0], connectedMac[1], connectedMac[2],
        connectedMac[3], connectedMac[4], connectedMac[5]);
    
    // Reset reliability for new session
    reliability.reset();
    lastHelloTime = millis();
    
    CmdHello pkt;
    uint8_t seq = reliability.nextSeq();
    initHeader(&pkt.hdr, CMD_HELLO, seq, 0, 0);  // sessionId=0, Sirloin assigns
    
    sendControlPacket(connectedMac, (uint8_t*)&pkt, sizeof(pkt), CMD_HELLO, seq);
    esp_err_t addCheck = esp_now_is_peer_exist(connectedMac) ? ESP_OK : ESP_FAIL;
    PIGSYNC_LOGF("[PIGSYNC-CLI-TX] CMD_HELLO peer=%d\n", addCheck);
}

void PigSyncMode::sendReady() {
    PIGSYNC_LOGF("[PIGSYNC-CLI-TX] CMD_READY sessionId=%04X channel=%d\n", sessionId, dataChannel);
    
    CmdReady pkt;
    uint8_t seq = reliability.nextSeq();
    initHeader(&pkt.hdr, CMD_READY, seq, reliability.lastRxSeq, sessionId);
    
    sendControlPacket(connectedMac, (uint8_t*)&pkt, sizeof(pkt), CMD_READY, seq);
}

void PigSyncMode::sendStartSync(uint8_t captureType, uint16_t index) {
    CmdStartSync pkt;
    uint8_t seq = reliability.nextSeq();
    initHeader(&pkt.hdr, CMD_START_SYNC, seq, reliability.lastRxSeq, sessionId);
    pkt.capture_type = captureType;
    pkt.reserved = 0;
    pkt.index = index;

    state = State::WAITING_CHUNKS;
    progress.captureType = captureType;
    progress.captureIndex = index;
    progress.currentChunk = 0;
    progress.inProgress = true;
    progress.startTime = millis();

    sendControlPacket(connectedMac, (uint8_t*)&pkt, sizeof(pkt), CMD_START_SYNC, seq);
}

void PigSyncMode::sendAckChunk(uint16_t seq) {
    CmdAckChunk pkt;
    initHeader(&pkt.hdr, CMD_ACK_CHUNK, reliability.nextSeq(), reliability.lastRxSeq, sessionId);
    pkt.chunk_seq = seq;   // Renamed field
    pkt.reserved = 0;
    
    esp_now_send(connectedMac, (uint8_t*)&pkt, sizeof(pkt));
}

void PigSyncMode::sendMarkSynced(uint8_t captureType, uint16_t index) {
    CmdMarkSynced pkt;
    uint8_t seq = reliability.nextSeq();
    initHeader(&pkt.hdr, CMD_MARK_SYNCED, seq, reliability.lastRxSeq, sessionId);
    pkt.capture_type = captureType;
    pkt.reserved = 0;
    pkt.index = index;
    
    sendControlPacket(connectedMac, (uint8_t*)&pkt, sizeof(pkt), CMD_MARK_SYNCED, seq);
}

void PigSyncMode::sendTimeSync() {
    // Phase 3: Request RTC time from Sirloin
    CmdTimeSync pkt;
    uint8_t seq = reliability.nextSeq();
    initHeader(&pkt.hdr, CMD_TIME_SYNC, seq, reliability.lastRxSeq, sessionId);
    pkt.porkchopMillis = millis();  // For RTT calculation
    
    sendControlPacket(connectedMac, (uint8_t*)&pkt, sizeof(pkt), CMD_TIME_SYNC, seq);
    PIGSYNC_LOGF("[PIGSYNC-CLI-TX] CMD_TIME_SYNC millis=%lu\n", pkt.porkchopMillis);
}

void PigSyncMode::sendPurge() {
    uint8_t buf[128];
    CmdPurge* pkt = (CmdPurge*)buf;
    
    uint8_t seq = reliability.nextSeq();
    initHeader(&pkt->hdr, CMD_PURGE, seq, reliability.lastRxSeq, sessionId);
    
    // Include Papa's goodbye message
    const char* goodbye = papaGoodbyeSelected[0] ? papaGoodbyeSelected : selectPapaGoodbye(totalSynced);
    size_t goodbyeLen = strlen(goodbye);
    if (goodbyeLen > 60) goodbyeLen = 60;
    
    pkt->papa_goodbye_len = goodbyeLen;
    memcpy(buf + sizeof(CmdPurge), goodbye, goodbyeLen);
    
    sendControlPacket(connectedMac, buf, sizeof(CmdPurge) + goodbyeLen, CMD_PURGE, seq);
}

void PigSyncMode::sendBounties() {
    uint8_t bountyBuf[PIGSYNC_MAX_BOUNTIES * 6] = {0};
    uint8_t bountyCount = 0;
    WarhogMode::buildBountyList(bountyBuf, &bountyCount);

    uint8_t buf[sizeof(CmdBounties) + PIGSYNC_MAX_BOUNTIES * 6] = {0};
    CmdBounties* pkt = (CmdBounties*)buf;
    uint8_t seq = reliability.nextSeq();
    initHeader(&pkt->hdr, CMD_BOUNTIES, seq, reliability.lastRxSeq, sessionId);
    pkt->count = bountyCount;
    pkt->reserved = 0;

    size_t payloadLen = sizeof(CmdBounties);
    if (bountyCount > 0) {
        size_t bountyLen = (size_t)bountyCount * 6;
        memcpy(buf + sizeof(CmdBounties), bountyBuf, bountyLen);
        payloadLen += bountyLen;
    }

    sendControlPacket(connectedMac, buf, payloadLen, CMD_BOUNTIES, seq);
}

void PigSyncMode::requestNextCapture() {
    if (controlTx.waiting || controlQueueCount > 0) {
        pendingNextCapture = true;
        return;
    }
    // Check if we need to move to handshakes
    if (currentType == CAPTURE_TYPE_PMKID) {
        if (currentIndex >= remotePMKIDCount) {
            // Done with PMKIDs, move to handshakes
            currentType = CAPTURE_TYPE_HANDSHAKE;
            currentIndex = 0;
        }
    }
    
    // Check if we need to move to handshakes or are done
    if (currentType == CAPTURE_TYPE_HANDSHAKE) {
        if (currentIndex >= remoteHSCount) {
            // All done!
            dialoguePhase = 2;  // Goodbye phase
            phraseStartTime = millis();
            
            // Select Papa's goodbye based on total synced
            strncpy(papaGoodbyeSelected, selectPapaGoodbye(totalSynced), sizeof(papaGoodbyeSelected) - 1);
            
            sendPurge();
            return;
        }
    }
    
    // Request next
    sendStartSync(currentType, currentIndex);
}

// ==[ SAVING ]==

bool PigSyncMode::savePMKID(const uint8_t* data, uint16_t len) {
    if (!Config::isSDAvailable()) return false;

    CapturedPMKID pmkid = {};
    if (!parseSirloinPMKID(data, len, pmkid)) {
        return false;
    }

    const char* handshakesDir = SDLayout::handshakesDir();
    if (!SD.exists(handshakesDir)) {
        SD.mkdir(handshakesDir);
    }

    char filename[64];
    SDLayout::buildCaptureFilename(filename, sizeof(filename),
                                   handshakesDir, pmkid.ssid, pmkid.bssid, ".22000");
    removeIfExists(filename);

    bool ok = OinkMode::savePMKID22000(pmkid, filename);
    return ok;
}

bool PigSyncMode::saveHandshake(const uint8_t* data, uint16_t len) {
    if (!Config::isSDAvailable()) return false;

    static CapturedHandshake hs;
    if (hs.beaconData) {
        free(hs.beaconData);
        hs.beaconData = nullptr;
    }
    memset(&hs, 0, sizeof(hs));

    if (!parseSirloinHandshake(data, len, hs)) {
        return false;
    }

    const char* handshakesDir = SDLayout::handshakesDir();
    if (!SD.exists(handshakesDir)) {
        SD.mkdir(handshakesDir);
    }

    char filenamePcap[64];
    SDLayout::buildCaptureFilename(filenamePcap, sizeof(filenamePcap),
                                   handshakesDir, hs.ssid, hs.bssid, ".pcap");
    removeIfExists(filenamePcap);

    char filename22000[64];
    SDLayout::buildCaptureFilename(filename22000, sizeof(filename22000),
                                   handshakesDir, hs.ssid, hs.bssid, "_hs.22000");
    removeIfExists(filename22000);

    bool pcapOk = OinkMode::saveHandshakePCAP(hs, filenamePcap);
    bool hs22kOk = OinkMode::saveHandshake22000(hs, filename22000);

    if (hs.beaconData) {
        free(hs.beaconData);
        hs.beaconData = nullptr;
    }

    return (pcapOk || hs22kOk);
}
