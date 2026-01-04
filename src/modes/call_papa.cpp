/**
 * SON OF A PIG - BLE Sync Client Implementation
 * 
 * Porkchop (Papa) receives captures from Sirloin devices over BLE.
 * Acts as BLE central/client, connects to Sirloin peripherals.
 * 
 * READY TO PCAP YOUR PHONE. LOL.
 */

#include "call_papa.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFi.h>
#include <atomic>
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../piglet/mood.h"
#include "../ui/display.h"

// Must match Sirloin's UUIDs exactly
#define SERVICE_UUID        "50524b43-4841-5033-4c49-4e4b53594e4b"  // "PRKCHAP3LINKSYNK"
#define CTRL_CHAR_UUID      "50524b43-0001-4841-5033-4c494e4b5359"
#define DATA_CHAR_UUID      "50524b43-0002-4841-5033-4c494e4b5359"
#define STATUS_CHAR_UUID    "50524b43-0003-4841-5033-4c494e4b5359"

// Commands (Porkchop -> Sirloin)
#define CMD_HELLO           0x01
#define CMD_GET_COUNT       0x02
#define CMD_START_SYNC      0x03
#define CMD_ACK_CHUNK       0x04
#define CMD_ABORT           0x05
#define CMD_MARK_SYNCED     0x06
#define CMD_PURGE_SYNCED    0x07

// Responses (Sirloin -> Porkchop)
#define RSP_HELLO           0x81
#define RSP_COUNT           0x82
#define RSP_SYNC_START      0x83
#define RSP_OK              0x84
#define RSP_ERROR           0x85
#define RSP_ABORTED         0x86
#define RSP_PURGED          0x87

// Chunk size (must match Sirloin)
#define CHUNK_SIZE          17

// Protocol magic bytes (must match Sirloin)
#define STATUS_MAGIC_P      0x50  // 'P'
#define STATUS_MAGIC_C      0x43  // 'C'

// Static member initialization
bool CallPapaMode::running = false;
bool CallPapaMode::bleInitialized = false;
uint8_t CallPapaMode::selectedIndex = 0;

std::vector<SirloinDevice> CallPapaMode::devices;
NimBLEClient* CallPapaMode::pClient = nullptr;
NimBLERemoteCharacteristic* CallPapaMode::pCtrlChar = nullptr;
NimBLERemoteCharacteristic* CallPapaMode::pDataChar = nullptr;
NimBLERemoteCharacteristic* CallPapaMode::pStatusChar = nullptr;
NimBLEAdvertisedDevice* CallPapaMode::targetDevice = nullptr;

uint16_t CallPapaMode::remotePMKIDCount = 0;
uint16_t CallPapaMode::remoteHSCount = 0;
uint16_t CallPapaMode::totalSynced = 0;
uint16_t CallPapaMode::syncedPMKIDs = 0;
uint16_t CallPapaMode::syncedHandshakes = 0;

bool CallPapaMode::readyFlagReceived = false;
uint16_t CallPapaMode::remotePendingCount = 0;
uint32_t CallPapaMode::connectionStartTime = 0;
uint32_t CallPapaMode::lastScanTime = 0;
uint32_t CallPapaMode::lastTimeoutTime = 0;
NimBLEAddress CallPapaMode::lastTimeoutDevice;  // Default constructor

CallPapaMode::State CallPapaMode::state = CallPapaMode::State::IDLE;

uint16_t CallPapaMode::currentType = 0;
uint16_t CallPapaMode::currentIndex = 0;
uint16_t CallPapaMode::totalChunks = 0;
uint16_t CallPapaMode::receivedChunks = 0;

SyncProgress CallPapaMode::progress = {0};
uint8_t CallPapaMode::rxBuffer[2048];
uint16_t CallPapaMode::rxBufferLen = 0;
char CallPapaMode::lastError[64] = "";

CallPapaMode::CaptureCallback CallPapaMode::onCaptureCb = nullptr;
CallPapaMode::SyncCompleteCallback CallPapaMode::onSyncCompleteCb = nullptr;

// Dialogue system - toxic dad/son relationship
static uint8_t currentDialogueId = 0;

// Dialogue state machine (runs in main loop, not callback)
enum class DialogueState {
    IDLE,
    HELLO_PAPA,      // Showing Papa's hello line
    HELLO_SON,       // Showing Son's response
    HELLO_LOOT,      // Showing loot count
    SYNC_RUNNING,    // Sync in progress
    GOODBYE_PAPA,    // Showing Papa's goodbye
    GOODBYE_SON,     // Showing Son's goodbye
    DONE
};
static std::atomic<DialogueState> dialogueState{DialogueState::IDLE};
static std::atomic<uint32_t> dialogueTimer{0};
static const uint32_t DIALOGUE_DELAY_MS = 2500;

// Toast state for Son's dialogue (non-blocking)
static String toastMessage = "";
static uint32_t toastStartTime = 0;
static bool toastActive = false;
static const uint32_t TOAST_DURATION_MS = 2500;

// Mutex for race condition protection between BLE callbacks and main loop
static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// Pending event flags - set in BLE callbacks, processed in main loop
// Protected by pendingMux for atomic read-modify-write
static volatile bool pendingHelloReceived = false;
static volatile uint16_t pendingPMKIDCount = 0;
static volatile uint16_t pendingHSCount = 0;
static volatile uint8_t pendingDialogueId = 0;

// Porkchop responses to RSP_HELLO (Papa's voice - Maximum Dysfunction)
static const char* PAPA_HELLO_RESPONSES[] = {
    "ABOUT TIME YOU SHOWED UP",
    "WHERES MY PMKID MONEY",
    "NOT SKID LOOT I HOPE"
};

// Porkchop responses on sync complete (Papa's voice - Inheritance jokes)
static const char* PAPA_COMPLETE_RESPONSES[] = {
    "MAYBE YOU AINT WORTHLESS",
    "ADDED TO INHERITANCE.TXT",
    "DISCONNECT BEFORE I REGRET IT"
};

// Sirloin responses to HELLO (Son's voice - shown as Toast on Porkchop)
static const char* SON_HELLO_RESPONSES[] = {
    "PAPA ITS YOUR FAVORITE MISTAKE",
    "SURPRISE IM NOT IN JAIL",
    "DONT HANG UP ON ME"
};

// Sirloin responses on sync complete (Son's voice - shown as Toast on Porkchop)
static const char* SON_COMPLETE_RESPONSES[] = {
    "SAME BLE TIME NEXT YEAR",
    "BYE OLD MAN",
    "/DEV/NULL YOUR CALLS"
};

// Papa's ROAST responses for 0 captures (Maximum Dysfunction)
static const char* PAPA_ROAST_RESPONSES[] = {
    "ZERO PMKIDS? NOT MY SON",
    "FAMILY TRADITION OF FAILURE",
    "SHOULD HAVE COMPILED YOU OUT"
};

// ============================================================================
// CRC32 VERIFICATION
// ============================================================================

uint32_t CallPapaMode::calculateCRC32(const uint8_t* data, uint16_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

// ============================================================================
// NOTIFICATION CALLBACKS
// ============================================================================

// Called when Control characteristic notifies (responses from Sirloin)
void ctrlNotifyCallback(NimBLERemoteCharacteristic* pChar,
                                uint8_t* pData, size_t length, bool isNotify) {
    Serial.printf("[DEBUG-CB] >>> ctrlNotifyCallback ENTRY (length=%d, isNotify=%d)\n", length, isNotify);
    if (length < 1) return;
    if (!CallPapaMode::isRunning()) return;  // Guard: don't process if stopped
    
    uint8_t rsp = pData[0];
    
    switch (rsp) {
        case RSP_HELLO:
            if (length >= 6) {
                // Parse RSP_HELLO: [rsp][version][pmkid_lo][pmkid_hi][hs_lo][hs_hi][flags][dialogue_id]
                uint8_t version = (length >= 2) ? pData[1] : 0;
                
                // Store in pending variables - will be processed in main loop
                // Protected by mutex for safe cross-context access
                taskENTER_CRITICAL(&pendingMux);
                pendingPMKIDCount = pData[2] | (pData[3] << 8);
                pendingHSCount = pData[4] | (pData[5] << 8);
                
                // Get dialogue_id from extended RSP_HELLO (byte 7) or random fallback
                if (length >= 8) {
                    pendingDialogueId = pData[7] % 3;  // Sirloin picked it
                } else {
                    pendingDialogueId = random(0, 3);  // Fallback: we pick
                }
                pendingHelloReceived = true;
                taskEXIT_CRITICAL(&pendingMux);
                
                // Protocol version check (spec expects 0x01)
                if (version != 0x01) {
                    Serial.printf("[SON-OF-PIG] WARNING: Protocol version mismatch! Expected 0x01, got 0x%02X\n", version);
                }
                
                Serial.printf("[SON-OF-PIG] HELLO: version=0x%02X, %d PMKIDs, %d Handshakes, dialogue=%d\n", 
                              version, pendingPMKIDCount, pendingHSCount, pendingDialogueId);
            }
            break;
            
        case RSP_COUNT:
            if (length >= 5) {
                CallPapaMode::remotePMKIDCount = pData[1] | (pData[2] << 8);
                CallPapaMode::remoteHSCount = pData[3] | (pData[4] << 8);
                Serial.printf("[SON-OF-PIG] COUNT: %d PMKIDs, %d Handshakes\n",
                              CallPapaMode::remotePMKIDCount, CallPapaMode::remoteHSCount);
            }
            break;
            
        case RSP_SYNC_START:
            if (length >= 5) {
                CallPapaMode::totalChunks = pData[1] | (pData[2] << 8) | (pData[3] << 16) | (pData[4] << 24);
                CallPapaMode::receivedChunks = 0;
                CallPapaMode::rxBufferLen = 0;
                CallPapaMode::progress.totalChunks = CallPapaMode::totalChunks;
                CallPapaMode::progress.currentChunk = 0;
                CallPapaMode::progress.inProgress = true;
                CallPapaMode::state = CallPapaMode::State::WAITING_CHUNKS;
                Serial.printf("[SON-OF-PIG] SYNC_START: %d chunks expected\n", CallPapaMode::totalChunks);
            }
            break;
            
        case RSP_OK:
            Serial.println("[SON-OF-PIG] OK");
            break;
            
        case RSP_ERROR:
            if (length >= 2) {
                snprintf(CallPapaMode::lastError, sizeof(CallPapaMode::lastError), 
                         "Error code: 0x%02X", pData[1]);
                Serial.printf("[SON-OF-PIG] ERROR: %s\n", CallPapaMode::lastError);
            }
            break;
            
        case RSP_ABORTED:
            Serial.println("[SON-OF-PIG] Transfer aborted");
            CallPapaMode::state = CallPapaMode::State::CONNECTED;
            CallPapaMode::progress.inProgress = false;
            break;
            
        case RSP_PURGED:
            if (length >= 2) {
                Serial.printf("[SON-OF-PIG] Purged %d captures\n", pData[1]);
            }
            // Let update() dialogue state machine handle transitions - don't manipulate state in callback
            break;
    }
}

// Called when Data characteristic notifies (capture chunks from Sirloin)
void dataNotifyCallback(NimBLERemoteCharacteristic* pChar,
                                uint8_t* pData, size_t length, bool isNotify) {
    Serial.printf("[DEBUG-CB] >>> dataNotifyCallback ENTRY (length=%d, isNotify=%d)\n", length, isNotify);
    if (length < 2) return;
    if (!CallPapaMode::isRunning()) return;  // Guard: don't process if stopped
    
    uint16_t seq = pData[0] | (pData[1] << 8);
    
    // Check for end marker (0xFFFF)
    if (seq == 0xFFFF && length >= 6) {
        // End of transfer - verify CRC
        uint32_t receivedCRC = pData[2] | (pData[3] << 8) | (pData[4] << 16) | (pData[5] << 24);
        uint32_t calculatedCRC = CallPapaMode::calculateCRC32(CallPapaMode::rxBuffer, CallPapaMode::rxBufferLen);
        
        if (receivedCRC == calculatedCRC) {
            Serial.printf("[SON-OF-PIG] Transfer complete! CRC OK, %d bytes\n", CallPapaMode::rxBufferLen);
            
            // Save capture based on type
            bool success = false;
            if (CallPapaMode::currentType == 0x01) {
                success = CallPapaMode::savePMKID(CallPapaMode::rxBuffer, CallPapaMode::rxBufferLen);
                if (success) CallPapaMode::syncedPMKIDs++;
            } else {
                success = CallPapaMode::saveHandshake(CallPapaMode::rxBuffer, CallPapaMode::rxBufferLen);
                if (success) CallPapaMode::syncedHandshakes++;
            }
            
            if (success) {
                CallPapaMode::totalSynced++;
                // Mood celebration removed - happens after dialogue completes
                // Silent transfer prevents dialogue message corruption
            }
            
            // Mark synced on Sirloin
            Serial.printf("[SON-OF-PIG] ===== SENDING CMD_MARK_SYNCED: type=0x%02X index=%d totalSynced=%d =====\n",
                          CallPapaMode::currentType, CallPapaMode::currentIndex, CallPapaMode::totalSynced);
            CallPapaMode::sendCommand(CMD_MARK_SYNCED, CallPapaMode::currentType, CallPapaMode::currentIndex);
            
            // Request next capture
            CallPapaMode::currentIndex++;
            CallPapaMode::progress.inProgress = false;
            CallPapaMode::requestNextCapture();
        } else {
            Serial.printf("[SON-OF-PIG] CRC MISMATCH! Got 0x%08X, expected 0x%08X\n",
                          receivedCRC, calculatedCRC);
            snprintf(CallPapaMode::lastError, sizeof(CallPapaMode::lastError), "CRC mismatch");
            // Retry same capture
            CallPapaMode::rxBufferLen = 0;
            CallPapaMode::receivedChunks = 0;
            CallPapaMode::sendCommand(CMD_START_SYNC, CallPapaMode::currentType, CallPapaMode::currentIndex);
        }
        return;
    }
    
    // Regular data chunk
    uint8_t dataLen = length - 2;
    uint16_t offset = seq * CHUNK_SIZE;
    
    if (offset + dataLen <= sizeof(CallPapaMode::rxBuffer)) {
        memcpy(CallPapaMode::rxBuffer + offset, pData + 2, dataLen);
        if (offset + dataLen > CallPapaMode::rxBufferLen) {
            CallPapaMode::rxBufferLen = offset + dataLen;
        }
        CallPapaMode::receivedChunks++;
        CallPapaMode::progress.currentChunk = CallPapaMode::receivedChunks;
        CallPapaMode::progress.bytesReceived = CallPapaMode::rxBufferLen;
        
        // Send ACK
        uint8_t ack[3] = {CMD_ACK_CHUNK, (uint8_t)(seq & 0xFF), (uint8_t)(seq >> 8)};
        if (CallPapaMode::pCtrlChar) {
            CallPapaMode::pCtrlChar->writeValue(ack, 3, false);
        }
        
        Serial.printf("[SON-OF-PIG] Chunk %d/%d received\n", seq + 1, CallPapaMode::totalChunks);
    }
}

// Called when Status characteristic notifies (state updates from Sirloin)
void statusNotifyCallback(NimBLERemoteCharacteristic* pChar,
                          uint8_t* pData, size_t length, bool isNotify) {
    // CRITICAL: Minimal processing only - avoid Serial.printf() which blocks for ~5ms each
    if (!CallPapaMode::isRunning()) return;  // Guard: don't process if stopped
    
    // Status structure: [0x50][0x43][pending_lo][pending_hi][flags][0x00]
    if (length < 6) return;
    
    // Verify protocol magic (fast check, no printf)
    if (pData[0] != STATUS_MAGIC_P || pData[1] != STATUS_MAGIC_C) return;
    
    // Parse status fields
    uint16_t pending = pData[2] | (pData[3] << 8);
    uint8_t flags = pData[4];
    
    bool ready = (flags & 0x04) != 0;     // READY flag
    bool syncing = (flags & 0x01) != 0;   // SYNCING flag
    bool bufferFull = (flags & 0x02) != 0; // BUFFER_FULL flag
    
    CallPapaMode::remotePendingCount = pending;
    
    // Update UI with buffer full warning (only when changed to reduce overhead)
    static bool lastBufferFull = false;
    if (bufferFull && !lastBufferFull) {
        // Only show warning when dialogue NOT active (prevents interrupting Papa/Son conversation)
        if (dialogueState == DialogueState::IDLE || dialogueState == DialogueState::DONE) {
            Mood::setStatusMessage("SIRLOIN BUFFER FULL!");
        }
        lastBufferFull = true;
    } else if (!bufferFull && lastBufferFull) {
        lastBufferFull = false;
    }
    
    // CRITICAL: Wait for READY flag before sending CMD_HELLO
    if (ready && !CallPapaMode::readyFlagReceived) {
        CallPapaMode::readyFlagReceived = true;
        
        // Update state and send HELLO
        CallPapaMode::state = CallPapaMode::State::CONNECTED;
        Mood::setStatusMessage("CALL ACCEPTED!");
        
        // Send CMD_HELLO now that we're ready (no delay in callback!)
        CallPapaMode::sendCommand(CMD_HELLO);
    }
}

// ============================================================================
// SCAN CALLBACK
// ============================================================================

class ScanCallbacks : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice* device) override {
        if (!device) {
            Serial.println("[SON-OF-PIG] onResult: null device!");
            return;
        }
        if (!CallPapaMode::isRunning()) {
            Serial.println("[SON-OF-PIG] onResult: not running, ignoring");
            return;  // Guard: don't process if stopped
        }
        
        // Debug: log every device found
        if (device->haveName()) {
            Serial.printf("[SON-OF-PIG] Device found: %s (RSSI: %d)\n", 
                         device->getName().c_str(), device->getRSSI());
        } else {
            Serial.printf("[SON-OF-PIG] Device found: %s (no name, RSSI: %d)\n", 
                         device->getAddress().toString().c_str(), device->getRSSI());
        }
        
        // Look for SIRLOIN by name or service UUID
        bool isSirloin = false;
        std::string deviceName = "";
        
        // Check by name first
        if (device->haveName()) {
            std::string name = device->getName();
            deviceName = name;
            if (name.find("SIRLOIN") != std::string::npos) {
                isSirloin = true;
                Serial.printf("[SON-OF-PIG] Found SIRLOIN by name: %s\n", 
                              device->getAddress().toString().c_str());
            }
        }
        
        // Check by service UUID if not found by name
        if (!isSirloin && device->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
            isSirloin = true;
            if (deviceName.empty()) {
                deviceName = "SIRLOIN";  // Default name if no name advertised
            }
            Serial.printf("[SON-OF-PIG] Found SIRLOIN by service UUID: %s\n", 
                          device->getAddress().toString().c_str());
        }
        
        if (isSirloin) {
                
                // Check if already in list
                NimBLEAddress addr = device->getAddress();
                auto& devList = CallPapaMode::getDevicesMutable();
                for (auto& d : devList) {
                    if (d.address == addr) {
                        // Update existing
                        d.rssi = device->getRSSI();
                        d.lastSeen = millis();
                        
                        // Parse manufacturer data for pending count
                        if (device->haveManufacturerData()) {
                            std::string mfgData = device->getManufacturerData();
                            if (mfgData.length() >= 5 && 
                                (uint8_t)mfgData[0] == 0x50 && 
                                (uint8_t)mfgData[1] == 0x43) {
                                d.pendingCaptures = (uint8_t)mfgData[2] | ((uint8_t)mfgData[3] << 8);
                                d.flags = (uint8_t)mfgData[4];
                            }
                        }
                        return;
                    }
                }
                
                // Add new device
                SirloinDevice newDevice;
                newDevice.address = addr;
                newDevice.rssi = device->getRSSI();
                newDevice.lastSeen = millis();
                newDevice.pendingCaptures = 0;
                newDevice.flags = 0;
                newDevice.syncing = false;
                
                // Copy name
                strncpy(newDevice.name, deviceName.c_str(), sizeof(newDevice.name) - 1);
                newDevice.name[sizeof(newDevice.name) - 1] = '\0';
                
                // Parse manufacturer data
                if (device->haveManufacturerData()) {
                    std::string mfgData = device->getManufacturerData();
                    if (mfgData.length() >= 5 && 
                        (uint8_t)mfgData[0] == 0x50 && 
                        (uint8_t)mfgData[1] == 0x43) {
                        newDevice.pendingCaptures = (uint8_t)mfgData[2] | ((uint8_t)mfgData[3] << 8);
                        newDevice.flags = (uint8_t)mfgData[4];
                    }
                }
                
                devList.push_back(newDevice);
                Serial.printf("[SON-OF-PIG] Added Sirloin: %s (%d dBm, %d captures)\n",
                              addr.toString().c_str(), newDevice.rssi, newDevice.pendingCaptures);
            }
    }
    
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        if (!CallPapaMode::isRunning()) return;
        // With continuous scanning (duration=0), this should rarely/never be called
        // Only happens if scan is explicitly stopped or errors out
        Serial.printf("[SON-OF-PIG] Scan ended unexpectedly (reason: %d), found %d Sirloin devices\n", 
                      reason, (int)CallPapaMode::getDeviceCount());
        CallPapaMode::setScanningState(false);
        
        // Try to restart scan if mode is still running
        if (CallPapaMode::isRunning() && !CallPapaMode::isConnected()) {
            Serial.println("[SON-OF-PIG] Restarting continuous scan...");
            delay(100);
            CallPapaMode::startScan();
        }
    }
};

static ScanCallbacks scanCallbacks;

// ============================================================================
// CLIENT CALLBACKS
// ============================================================================

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        Serial.println("[DEBUG-CB] *** onConnect CALLBACK FIRED ***");
        Serial.printf("[DEBUG-CB] Client address: %s\n", pClient->getPeerAddress().toString().c_str());
        if (!CallPapaMode::isRunning()) return;  // Guard
        Serial.println("[SON-OF-PIG] Connected to Sirloin!");
        // DON'T set state here - it will be set in connectTo() after subscriptions
        // CallPapaMode::state = CallPapaMode::State::CONNECTED;  // REMOVED - causes race condition
    }
    
    void onDisconnect(NimBLEClient* pClient, int reason) override {
        Serial.println("[DEBUG-CB] !!! onDisconnect CALLBACK FIRED !!!");
        Serial.printf("[DEBUG-CB] Disconnect reason code: %d\n", reason);
        Serial.printf("[DEBUG-CB] Time since boot: %lu ms\n", millis());
        Serial.printf("[SON-OF-PIG] Disconnected from Sirloin (reason: %d)\n", reason);
        CallPapaMode::state = CallPapaMode::State::IDLE;
        CallPapaMode::pCtrlChar = nullptr;
        CallPapaMode::pDataChar = nullptr;
        CallPapaMode::pStatusChar = nullptr;
        CallPapaMode::progress.inProgress = false;
        
        // Don't reset dialogue if in GOODBYE phase - let it complete gracefully
        if (dialogueState != DialogueState::GOODBYE_PAPA && 
            dialogueState != DialogueState::GOODBYE_SON &&
            dialogueState != DialogueState::DONE) {
            dialogueState = DialogueState::IDLE;
            dialogueTimer = 0;
        }
        
        // Reset pending flags (with mutex protection)
        taskENTER_CRITICAL(&pendingMux);
        pendingHelloReceived = false;
        pendingPMKIDCount = 0;
        pendingHSCount = 0;
        pendingDialogueId = 0;
        taskEXIT_CRITICAL(&pendingMux);
        
        // Only process if still running
        if (!CallPapaMode::isRunning()) return;
        
        // Clear syncing flag on all devices
        for (auto& d : CallPapaMode::getDevicesMutable()) {
            d.syncing = false;
        }
        
        // Restart scanning if still running
        CallPapaMode::startScan();
    }
};

static ClientCallbacks clientCallbacks;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void CallPapaMode::sendCommand(uint8_t cmd) {
    if (pCtrlChar) {
        pCtrlChar->writeValue(&cmd, 1, false);
    }
}

void CallPapaMode::sendCommand(uint8_t cmd, uint8_t arg1, uint8_t arg2) {
    if (pCtrlChar) {
        uint8_t data[3] = {cmd, arg1, arg2};
        pCtrlChar->writeValue(data, 3, false);
    }
}

void CallPapaMode::sendCommand(uint8_t cmd, uint8_t type, uint16_t index) {
    if (pCtrlChar) {
        uint8_t data[4] = {cmd, type, (uint8_t)(index & 0xFF), (uint8_t)((index >> 8) & 0xFF)};
        pCtrlChar->writeValue(data, 4, false);
    }
}

void CallPapaMode::requestNextCapture() {
    if (state != State::CONNECTED && state != State::SYNCING && state != State::WAITING_CHUNKS) {
        return;
    }
    
    // First sync all PMKIDs, then all Handshakes
    if (currentType == 0x01) {
        // Still doing PMKIDs
        if (currentIndex < remotePMKIDCount) {
            rxBufferLen = 0;
            receivedChunks = 0;
            sendCommand(CMD_START_SYNC, 0x01, currentIndex);
            state = State::SYNCING;
            progress.captureType = 0;
            progress.captureIndex = currentIndex;
            Serial.printf("[SON-OF-PIG] Requesting PMKID %d/%d\n", currentIndex + 1, remotePMKIDCount);
        } else {
            // Done with PMKIDs, start Handshakes
            currentType = 0x02;
            currentIndex = 0;
            requestNextCapture();
        }
    } else {
        // Doing Handshakes
        if (currentIndex < remoteHSCount) {
            rxBufferLen = 0;
            receivedChunks = 0;
            sendCommand(CMD_START_SYNC, 0x02, currentIndex);
            state = State::SYNCING;
            progress.captureType = 1;
            progress.captureIndex = currentIndex;
            Serial.printf("[SON-OF-PIG] Requesting Handshake %d/%d\n", currentIndex + 1, remoteHSCount);
        } else {
            // All done!
            Serial.printf("[SON-OF-PIG] SYNC COMPLETE! %d PMKIDs, %d Handshakes\n",
                          syncedPMKIDs, syncedHandshakes);
            state = State::SYNC_COMPLETE;
            progress.inProgress = false;
            
            // Start goodbye dialogue state machine
            // Set timer FIRST to avoid race condition with update() loop
            dialogueTimer = millis();
            // Memory barrier ensures timer write completes before state write (prevents reordering)
            __asm__ __volatile__("" ::: "memory");
            dialogueState = DialogueState::GOODBYE_PAPA;
            Serial.printf("[DEBUG-ISSUE3] >>> GOODBYE_PAPA set at T=%lu, dialogueTimer=%lu\n", 
                          millis(), (uint32_t)dialogueTimer.load());
            Mood::setStatusMessage(PAPA_COMPLETE_RESPONSES[currentDialogueId]);
            
            Mood::adjustHappiness(30);  // Big happiness boost for successful sync
            
            if (onSyncCompleteCb) {
                onSyncCompleteCb(syncedPMKIDs, syncedHandshakes);
            }
            
            // Purge synced captures from Sirloin to free memory
            sendCommand(CMD_PURGE_SYNCED);
        }
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void CallPapaMode::init() {
    devices.clear();
    selectedIndex = 0;
    remotePMKIDCount = 0;
    remoteHSCount = 0;
    totalSynced = 0;
    syncedPMKIDs = 0;
    syncedHandshakes = 0;
    readyFlagReceived = false;
    remotePendingCount = 0;
    connectionStartTime = 0;
    lastScanTime = 0;
    progress = {0};
    currentType = 0;
    currentIndex = 0;
    state = State::IDLE;
    lastError[0] = '\0';
    dialogueState = DialogueState::IDLE;
    dialogueTimer = 0;
    
    // Reset pending event flags
    pendingHelloReceived = false;
    pendingPMKIDCount = 0;
    pendingHSCount = 0;
    pendingDialogueId = 0;
}

void CallPapaMode::start() {
    if (running) return;
    
    Serial.println("[SON-OF-PIG] Starting SON OF A PIG mode...");
    
    init();
    
    // Disable WiFi first to free up shared antenna resources
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);  // Give WiFi time to fully stop
    
    // Initialize NimBLE if needed
    if (!bleInitialized) {
        Serial.println("[SON-OF-PIG] Initializing NimBLE...");
        NimBLEDevice::init("PORKCHOP");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max power for range
        bleInitialized = true;
    }
    
    // Always create a fresh client (previous one was deleted on stop)
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(&clientCallbacks);
        
        // Configure connection parameters (more relaxed for better compatibility)
        // min=24 (30ms), max=40 (50ms), latency=0, timeout=3200 (32s max allowed)
        pClient->setConnectionParams(24, 40, 0, 3200);
        pClient->setConnectTimeout(180);  // 3 minutes - wait for user to accept call
    }
    
    Serial.println("[SON-OF-PIG] BLE Ready");
    
    running = true;
    state = State::IDLE;
    
    // Start scanning for Sirloin devices
    startScan();
    
    Serial.println("[SON-OF-PIG] Running - scanning for Sirloin devices");
    SDLog::log("SON-OF-PIG", "SON OF A PIG mode started");
}

void CallPapaMode::stop() {
    if (!running) return;
    
    Serial.println("[SON-OF-PIG] ========== STOP() CALLED ==========");
    Serial.printf("[SON-OF-PIG] STOP: state=%d, dialogueState=%d\n", (int)state, (int)dialogueState.load());
    // Print stack trace hint
    Serial.printf("[SON-OF-PIG] STOP called at millis=%lu\n", millis());
    
    // Disconnect if connected
    disconnect();
    
    // Stop scanning
    stopScan();
    
    // DON'T delete the client or deinit BLE - causes heap corruption on ESP32-S3
    // Keep everything initialized for next time, just clear state
    
    pCtrlChar = nullptr;
    pDataChar = nullptr;
    pStatusChar = nullptr;
    targetDevice = nullptr;
    
    devices.clear();
    running = false;
    state = State::IDLE;
    
    Serial.printf("[SON-OF-PIG] Stopped - synced %d captures\n", totalSynced);
    SDLog::log("SON-OF-PIG", "SON OF A PIG stopped, synced %d captures", totalSynced);
}

void CallPapaMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // === AUTO-CONNECT WHEN SIRLOIN FOUND ===
    // Check every 500ms if we have devices and should auto-connect
    static uint32_t lastConnectCheck = 0;
    if (state == State::SCANNING && (now - lastConnectCheck) >= 500) {
        lastConnectCheck = now;
        
        auto& devs = getDevicesMutable();
        if (!devs.empty()) {
            // Find first device with pending captures (that's not in cooldown)
            int bestIdx = -1;
            for (size_t i = 0; i < devs.size(); i++) {
                // Check cooldown: skip device if we timed out on it within last 15 seconds
                if (lastTimeoutTime > 0 && (now - lastTimeoutTime) < 15000) {
                    if (devs[i].address == lastTimeoutDevice) {
                        uint32_t remainingCooldown = 15000 - (now - lastTimeoutTime);
                        Serial.printf("[SON-OF-PIG] Device %s in cooldown (%lu ms remaining)\n",
                                      devs[i].address.toString().c_str(), remainingCooldown);
                        continue;  // Skip this device
                    }
                }
                
                if (devs[i].pendingCaptures > 0 && !devs[i].syncing) {
                    bestIdx = i;
                    Serial.printf("[SON-OF-PIG] AUTO-CALLING %s (%d captures pending)\n",
                                  devs[i].address.toString().c_str(), devs[i].pendingCaptures);
                    break;
                }
            }
            
            // No devices with loot - call first anyway to check (if not in cooldown)
            if (bestIdx == -1 && state == State::SCANNING) {
                // Check if first device is in cooldown
                if (lastTimeoutTime > 0 && (now - lastTimeoutTime) < 15000 && 
                    devs[0].address == lastTimeoutDevice) {
                    uint32_t remainingCooldown = 15000 - (now - lastTimeoutTime);
                    Serial.printf("[SON-OF-PIG] Only device in cooldown (%lu ms remaining)\n", remainingCooldown);
                } else {
                    bestIdx = 0;
                    Serial.println("[SON-OF-PIG] AUTO-CALLING first device (no advertised loot)");
                }
            }
            
            if (bestIdx >= 0) {
                connectTo(bestIdx);
            }
        }
    }
    
    // === ERROR STATE RECOVERY ===
    // If connection fails, restart scanning after 2 seconds
    static uint32_t errorTime = 0;
    if (state == State::ERROR) {
        if (errorTime == 0) {
            errorTime = now;
            Serial.println("[SON-OF-PIG] Entered ERROR state, will retry in 2 seconds...");
        } else if ((now - errorTime) >= 2000) {
            Serial.println("[SON-OF-PIG] Recovering from ERROR state, restarting scan...");
            errorTime = 0;
            state = State::IDLE;
            startScan();
        }
    } else if (errorTime != 0) {
        // Reset error timer if we left error state
        errorTime = 0;
    }
    
    // === TIMEOUT HANDLING FOR CALL ACCEPTANCE ===
    // Extended timeout: 3 minutes to give user time to notice and accept
    if (state == State::CONNECTED_WAITING_READY) {
        // CRITICAL: Refresh 'now' because connectTo() may have been called earlier 
        // in this same update() cycle, and 'now' would be stale (causing underflow)
        now = millis();
        
        // Guard against underflow: if connectionStartTime is in the future, skip check
        // This can happen on the same update() cycle where connectTo() just finished
        if (connectionStartTime > now) {
            // Skip - will check on next cycle
        } else {
            uint32_t elapsed = now - connectionStartTime;
            if (!readyFlagReceived && elapsed > 180000) {
                Serial.printf("[SON-OF-PIG] TIMEOUT: elapsed=%lu ms > 180000ms\n", elapsed);
                snprintf(lastError, sizeof(lastError), "Call acceptance timeout");
                Mood::setStatusMessage("CALL TIMEOUT - DISCONNECTING");
                
                // Track this timeout to prevent immediate reconnection
                if (pClient && pClient->isConnected()) {
                    lastTimeoutDevice = pClient->getPeerAddress();
                    lastTimeoutTime = now;
                    Serial.printf("[SON-OF-PIG] Setting 15-second cooldown for device %s\n", 
                                  lastTimeoutDevice.toString().c_str());
                }
                
                disconnect();
                return;
            }
        }
    }
    
    // === PROCESS PENDING EVENTS FROM BLE CALLBACKS ===
    // This safely moves state changes from callback context to main loop
    // Protected by mutex to prevent race with BLE callback
    bool helloPending = false;
    uint16_t localPMKIDCount = 0, localHSCount = 0;
    uint8_t localDialogueId = 0;
    
    taskENTER_CRITICAL(&pendingMux);
    if (pendingHelloReceived) {
        helloPending = true;
        localPMKIDCount = pendingPMKIDCount;
        localHSCount = pendingHSCount;
        localDialogueId = pendingDialogueId;
        pendingHelloReceived = false;
    }
    taskEXIT_CRITICAL(&pendingMux);
    
    if (helloPending) {
        // Copy local values to main state
        remotePMKIDCount = localPMKIDCount;
        remoteHSCount = localHSCount;
        currentDialogueId = localDialogueId;
        
        // Reset connectionStartTime when dialogue actually starts
        connectionStartTime = millis();
        
        // NOW start dialogue state machine (safe in main loop)
        dialogueTimer = now;
        dialogueState = DialogueState::HELLO_PAPA;
        Mood::setStatusMessage(PAPA_HELLO_RESPONSES[currentDialogueId]);
        
        Serial.printf("[SON-OF-PIG] Dialogue started: Papa says '%s'\n", 
                     PAPA_HELLO_RESPONSES[currentDialogueId]);
    }
    
    // === DIALOGUE STATE MACHINE ===
    // Process dialogue exchanges without blocking BLE callbacks
    if (dialogueState != DialogueState::IDLE && dialogueState != DialogueState::SYNC_RUNNING) {
        // DEBUG: Trace state machine entry
        static uint32_t lastStateDebug = 0;
        if (now - lastStateDebug > 1000) {
            Serial.printf("[DEBUG-ISSUE3] StateMachine: dialogueState=%d, timer=%lu, elapsed=%lu\n",
                          (int)dialogueState.load(), (uint32_t)dialogueTimer.load(), now - (uint32_t)dialogueTimer.load());
            lastStateDebug = now;
        }
        
        // Watchdog: if stuck in GOODBYE phase for >10s, force completion
        if ((dialogueState == DialogueState::GOODBYE_PAPA || dialogueState == DialogueState::GOODBYE_SON) && 
            now - dialogueTimer > 10000) {
            Serial.println("[SON-OF-PIG] WATCHDOG: Dialogue stuck in GOODBYE phase, forcing DONE");
            dialogueState = DialogueState::DONE;
        }
        
        if (now - dialogueTimer >= DIALOGUE_DELAY_MS) {
            switch (dialogueState) {
                case DialogueState::HELLO_PAPA:
                    // Papa spoke, now Son responds (toast overlay)
                    toastMessage = "SON: " + String(SON_HELLO_RESPONSES[currentDialogueId]);
                    toastStartTime = now;
                    toastActive = true;
                    dialogueState = DialogueState::HELLO_SON;
                    dialogueTimer = now;
                    break;
                    
                case DialogueState::HELLO_SON:
                    // Son spoke, now show loot count
                    {
                        char msg[32];
                        snprintf(msg, sizeof(msg), "GOT %d+%d LOOT", remotePMKIDCount, remoteHSCount);
                        Mood::setStatusMessage(msg);
                    }
                    dialogueState = DialogueState::HELLO_LOOT;
                    dialogueTimer = now;
                    break;
                    
                case DialogueState::HELLO_LOOT:
                    // Loot shown, start sync - but validate connection first!
                    if (state == State::CONNECTED) {
                        dialogueState = DialogueState::SYNC_RUNNING;
                        if (!startSync()) {
                            // Sync failed - show error and go to done
                            Serial.println("[SON-OF-PIG] startSync() failed during dialogue!");
                            Mood::setStatusMessage("SYNC FAILED!");
                            dialogueState = DialogueState::DONE;
                        }
                    } else {
                        // Connection lost during dialogue
                        Serial.printf("[SON-OF-PIG] Connection lost during dialogue (state=%d)\n", (int)state);
                        Mood::setStatusMessage("CONNECTION LOST!");
                        dialogueState = DialogueState::DONE;
                    }
                    break;
                    
                case DialogueState::GOODBYE_PAPA:
                    // Papa spoke, now Son responds (toast overlay)
                    Serial.printf("[DEBUG-ISSUE3] >>> GOODBYE_PAPA -> GOODBYE_SON at T=%lu\n", millis());
                    toastMessage = "SON: " + String(SON_COMPLETE_RESPONSES[currentDialogueId]);
                    toastStartTime = now;
                    toastActive = true;
                    dialogueState = DialogueState::GOODBYE_SON;
                    dialogueTimer = now;
                    break;
                    
                case DialogueState::GOODBYE_SON:
                    // Done with dialogue
                    Serial.printf("[DEBUG-ISSUE3] >>> GOODBYE_SON -> DONE at T=%lu\n", millis());
                    dialogueState = DialogueState::DONE;
                    break;
                    
                default:
                    break;
            }
        }
        
        // CRITICAL TIMEOUT: If stuck in SYNC_RUNNING for >60s, force completion
        // This prevents "session never closes" bug when dialogue gets stuck
        if (dialogueState == DialogueState::SYNC_RUNNING) {
            if (now - dialogueTimer > 60000) {  // 60 second timeout
                Serial.println("[SON-OF-PIG] TIMEOUT: SYNC_RUNNING exceeded 60s, forcing GOODBYE");
                Serial.printf("[SON-OF-PIG] state=%d, synced=%d/%d\n", (int)state, totalSynced, remotePMKIDCount + remoteHSCount);
                
                // Force transition to goodbye phase
                dialogueTimer = now;
                __asm__ __volatile__("" ::: "memory");
                dialogueState = DialogueState::GOODBYE_PAPA;
                Mood::setStatusMessage("SYNC TIMEOUT!");
                
                // If we synced anything, still celebrate
                if (totalSynced > 0 && onSyncCompleteCb) {
                    onSyncCompleteCb(syncedPMKIDs, syncedHandshakes);
                }
            }
        }
    }
    
    // Remove stale devices (not seen in 30s)
    for (auto it = devices.begin(); it != devices.end(); ) {
        if (now - it->lastSeen > 30000 && !it->syncing) {
            it = devices.erase(it);
        } else {
            ++it;
        }
    }
}

void CallPapaMode::startScan() {
    if (state == State::SCANNING) return;
    if (state == State::CONNECTED_WAITING_READY || state == State::CONNECTED || 
        state == State::SYNCING || state == State::WAITING_CHUNKS) return;
    
    Serial.println("[SON-OF-PIG] Starting CONTINUOUS BLE scan...");
    
    // DON'T clear devices - keep accumulating discovered devices
    // devices.clear();  // REMOVED - maintain device list across scan
    
    if (!bleInitialized) {
        Serial.println("[SON-OF-PIG] BLE not initialized!");
        return;
    }
    
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
        Serial.println("[SON-OF-PIG] Failed to get scan handle");
        return;
    }
    
    // Don't clear results - we want to keep seeing devices
    // pScan->clearResults();  // REMOVED - keep accumulated results
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->setDuplicateFilter(false);  // We want to see every advertisement
    
    // Register callbacks - MUST be done before start()
    pScan->setScanCallbacks(&scanCallbacks, false);
    
    // Start continuous scan (duration=0 means indefinite/continuous)
    if (pScan->start(0, false, false)) {
        state = State::SCANNING;
        lastScanTime = millis();
        Serial.println("[SON-OF-PIG] Continuous scan started (will run indefinitely)");
    } else {
        Serial.println("[SON-OF-PIG] Failed to start scan");
    }
}

void CallPapaMode::stopScan() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
        delay(100);  // Wait for scan to fully stop and callbacks to complete
    }
    state = State::IDLE;
}

bool CallPapaMode::isScanning() { 
    return state == State::SCANNING; 
}

void CallPapaMode::setScanningState(bool scanning) {
    if (!scanning && state == State::SCANNING) {
        state = State::IDLE;
    }
}

void CallPapaMode::selectDevice(uint8_t index) {
    if (index < devices.size()) {
        selectedIndex = index;
    }
}

const SirloinDevice* CallPapaMode::getConnectedDevice() {
    if (!isConnected() || devices.empty()) return nullptr;
    
    for (const auto& d : devices) {
        if (d.syncing) return &d;
    }
    return nullptr;
}

bool CallPapaMode::connectTo(uint8_t deviceIndex) {
    if (deviceIndex >= devices.size()) return false;
    if (isConnected()) disconnect();
    
    SirloinDevice& device = devices[deviceIndex];
    
    Serial.printf("[SON-OF-PIG] Connecting to %s (%d dBm)...\n", 
                  device.address.toString().c_str(), device.rssi);
    Mood::setStatusMessage("CONNECTING...");
    
    // IMPORTANT: Stop scanning before connecting - they share the radio!
    stopScan();
    delay(200);  // Give BLE stack time to fully stop scan
    
    state = State::CONNECTING;
    
    // Try connecting with retry
    bool connected = false;
    for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
        Serial.printf("[SON-OF-PIG] Connection attempt %d/3...\n", attempt);
        
        // Ensure clean state before connecting
        if (pClient) {
            if (pClient->isConnected()) {
                pClient->disconnect();
            } else {
                // Cancel any in-flight connection from the previous attempt
                pClient->disconnect();
            }
            delay(300);  // Give BLE stack time to settle before retry
        }
        
        if (pClient && pClient->connect(device.address, false)) {  // false = don't delete on disconnect
            connected = true;
        } else {
            Serial.printf("[SON-OF-PIG] Attempt %d failed\n", attempt);
            if (attempt < 3) {
                delay(500);  // Wait before retry
            }
        }
    }
    
    if (!connected) {
        snprintf(lastError, sizeof(lastError), "Connection failed after 3 attempts");
        Serial.printf("[SON-OF-PIG] ERROR: %s\n", lastError);
        state = State::ERROR;
        // Scanning will be restarted by update() ERROR recovery logic
        return false;
    }
    
    uint32_t connTime = millis();
    Serial.printf("[DEBUG] T+%lu: Physical connection established\n", millis() - connTime);
    Serial.printf("[DEBUG] T+%lu: Client connected status: %d\n", millis() - connTime, pClient->isConnected());
    Serial.printf("[DEBUG] T+%lu: Waiting 500ms for BLE stack to settle...\n", millis() - connTime);
    delay(500);
    Serial.printf("[DEBUG] T+%lu: After delay - still connected: %d\n", millis() - connTime, pClient->isConnected());
    
    // Get service
    Serial.printf("[DEBUG] T+%lu: Getting service %s...\n", millis() - connTime, SERVICE_UUID);
    NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (!pService) {
        snprintf(lastError, sizeof(lastError), "Service not found");
        Serial.printf("[SON-OF-PIG] ERROR: %s\n", lastError);
        pClient->disconnect();
        state = State::ERROR;
        // Scanning will be restarted by update() ERROR recovery logic
        return false;
    }
    Serial.printf("[DEBUG] T+%lu: Service found at %p\n", millis() - connTime, pService);
    
    // Get characteristics
    Serial.printf("[DEBUG] T+%lu: Getting Control characteristic...\n", millis() - connTime);
    pCtrlChar = pService->getCharacteristic(CTRL_CHAR_UUID);
    Serial.printf("[DEBUG] T+%lu: Control char at %p\n", millis() - connTime, pCtrlChar);
    
    Serial.printf("[DEBUG] T+%lu: Getting Data characteristic...\n", millis() - connTime);
    pDataChar = pService->getCharacteristic(DATA_CHAR_UUID);
    Serial.printf("[DEBUG] T+%lu: Data char at %p\n", millis() - connTime, pDataChar);
    
    Serial.printf("[DEBUG] T+%lu: Getting Status characteristic...\n", millis() - connTime);
    pStatusChar = pService->getCharacteristic(STATUS_CHAR_UUID);
    Serial.printf("[DEBUG] T+%lu: Status char at %p\n", millis() - connTime, pStatusChar);
    
    if (!pCtrlChar || !pDataChar || !pStatusChar) {
        snprintf(lastError, sizeof(lastError), "Characteristics not found");
        Serial.printf("[SON-OF-PIG] ERROR: %s (pCtrl=%p, pData=%p, pStatus=%p)\n", 
                      lastError, pCtrlChar, pDataChar, pStatusChar);
        pClient->disconnect();
        state = State::ERROR;
        // Scanning will be restarted by update() ERROR recovery logic
        return false;
    }
    
    Serial.printf("[DEBUG] T+%lu: All characteristics found, checking properties...\n", millis() - connTime);
    Serial.printf("[DEBUG] T+%lu: Status canNotify: %d\n", millis() - connTime, pStatusChar->canNotify());
    Serial.printf("[DEBUG] T+%lu: Control canNotify: %d\n", millis() - connTime, pCtrlChar->canNotify());
    Serial.printf("[DEBUG] T+%lu: Data canNotify: %d\n", millis() - connTime, pDataChar->canNotify());
    Serial.printf("[DEBUG] T+%lu: Waiting 200ms before subscribing...\n", millis() - connTime);
    delay(200);
    Serial.printf("[DEBUG] T+%lu: Still connected after delay: %d\n", millis() - connTime, pClient->isConnected());
    
    // CRITICAL FIX: Check if we got disconnected during delay (prevents crash)
    if (!pClient->isConnected()) {
        Serial.println("[SON-OF-PIG] ERROR: Disconnected during setup - aborting");
        snprintf(lastError, sizeof(lastError), "Disconnected during setup");
        state = State::ERROR;
        return false;
    }
    
    // CRITICAL: Subscribe to Status characteristic FIRST (per protocol spec)
    // Status must be subscribed before Control and Data to receive READY flag
    Serial.printf("[DEBUG] T+%lu: === STARTING STATUS SUBSCRIPTION ===\n", millis() - connTime);
    if (pStatusChar->canNotify()) {
        Serial.printf("[DEBUG] T+%lu: Status can notify, attempting subscribe...\n", millis() - connTime);
        if (pStatusChar->subscribe(true, statusNotifyCallback)) {
            Serial.printf("[DEBUG] T+%lu: *** Status subscription SUCCESS ***\n", millis() - connTime);
            Serial.printf("[DEBUG] T+%lu: Connected after Status sub: %d\n", millis() - connTime, pClient->isConnected());
        } else {
            Serial.printf("[DEBUG] T+%lu: !!! Status subscription FAILED !!!\n", millis() - connTime);
            snprintf(lastError, sizeof(lastError), "Status subscribe failed");
            pClient->disconnect();
            state = State::ERROR;
            return false;
        }
    } else {
        Serial.printf("[DEBUG] T+%lu: !!! Status cannot notify !!!\n", millis() - connTime);
        snprintf(lastError, sizeof(lastError), "Status char not notifiable");
        pClient->disconnect();
        state = State::ERROR;
        return false;
    }
    
    // Subscribe to Control and Data notifications
    Serial.printf("[DEBUG] T+%lu: === STARTING CONTROL SUBSCRIPTION ===\n", millis() - connTime);
    if (pCtrlChar->canNotify()) {
        Serial.printf("[DEBUG] T+%lu: Control can notify, attempting subscribe...\n", millis() - connTime);
        if (pCtrlChar->subscribe(true, ctrlNotifyCallback)) {
            Serial.printf("[DEBUG] T+%lu: *** Control subscription SUCCESS ***\n", millis() - connTime);
            Serial.printf("[DEBUG] T+%lu: Connected after Control sub: %d\n", millis() - connTime, pClient->isConnected());
        } else {
            Serial.printf("[DEBUG] T+%lu: !!! Control subscription FAILED !!!\n", millis() - connTime);
            snprintf(lastError, sizeof(lastError), "Control subscribe failed");
            pClient->disconnect();
            state = State::ERROR;
            return false;
        }
    } else {
        Serial.printf("[DEBUG] T+%lu: !!! Control cannot notify !!!\n", millis() - connTime);
        snprintf(lastError, sizeof(lastError), "Control char not notifiable");
        pClient->disconnect();
        state = State::ERROR;
        return false;
    }
    Serial.printf("[DEBUG] T+%lu: === STARTING DATA SUBSCRIPTION ===\n", millis() - connTime);
    if (pDataChar->canNotify()) {
        Serial.printf("[DEBUG] T+%lu: Data can notify, attempting subscribe...\n", millis() - connTime);
        if (pDataChar->subscribe(true, dataNotifyCallback)) {
            Serial.printf("[DEBUG] T+%lu: *** Data subscription SUCCESS ***\n", millis() - connTime);
            Serial.printf("[DEBUG] T+%lu: Connected after Data sub: %d\n", millis() - connTime, pClient->isConnected());
        } else {
            Serial.printf("[DEBUG] T+%lu: !!! Data subscription FAILED !!!\n", millis() - connTime);
            snprintf(lastError, sizeof(lastError), "Data subscribe failed");
            pClient->disconnect();
            state = State::ERROR;
            return false;
        }
    } else {
        Serial.printf("[DEBUG] T+%lu: !!! Data cannot notify !!!\n", millis() - connTime);
        snprintf(lastError, sizeof(lastError), "Data char not notifiable");
        pClient->disconnect();
        state = State::ERROR;
        return false;
    }
    
    // Set state to CONNECTED_WAITING_READY
    // We will send CMD_HELLO only after receiving READY flag in statusNotifyCallback
    Serial.printf("[DEBUG] T+%lu: === ALL SUBSCRIPTIONS COMPLETE ===\n", millis() - connTime);
    Serial.printf("[DEBUG] T+%lu: Final connection check: %d\n", millis() - connTime, pClient->isConnected());
    Serial.printf("[DEBUG] T+%lu: Setting state to CONNECTED_WAITING_READY\n", millis() - connTime);
    
    state = State::CONNECTED_WAITING_READY;
    readyFlagReceived = false;
    connectionStartTime = millis();
    device.syncing = true;
    
    Serial.printf("[DEBUG] T+%lu: Connected! Waiting for READY flag from Status notification...\n", millis() - connTime);
    Serial.println("[SON-OF-PIG] *** SUBSCRIPTIONS ACTIVE - MONITORING FOR DISCONNECTION ***");
    Mood::setStatusMessage("WAITING FOR CALL ACCEPT...");
    
    // DO NOT send CMD_HELLO here!
    // Protocol spec: "Papa MUST wait for READY flag (0x04) before sending CMD_HELLO"
    // CMD_HELLO will be sent from statusNotifyCallback when READY flag is received
    
    return true;
}

void CallPapaMode::disconnect() {
    Serial.println("[SON-OF-PIG] ========== DISCONNECT() CALLED ==========");
    Serial.printf("[SON-OF-PIG] DISCONNECT: pClient=%p, isConnected=%d, state=%d\n", 
                  pClient, pClient ? pClient->isConnected() : -1, (int)state);
    
    // CRITICAL FIX: Unsubscribe from all characteristics BEFORE disconnect
    // This prevents mbuf pool exhaustion by freeing notification resources
    if (pClient && pClient->isConnected()) {
        if (pStatusChar) {
            Serial.println("[SON-OF-PIG] Unsubscribing from Status notifications...");
            pStatusChar->unsubscribe();
            delay(50);  // Allow BLE stack to process unsubscribe
        }
        if (pCtrlChar) {
            Serial.println("[SON-OF-PIG] Unsubscribing from Control notifications...");
            pCtrlChar->unsubscribe();
            delay(50);
        }
        if (pDataChar) {
            Serial.println("[SON-OF-PIG] Unsubscribing from Data notifications...");
            pDataChar->unsubscribe();
            delay(50);
        }
        
        Serial.println("[SON-OF-PIG] All characteristics unsubscribed, disconnecting...");
        pClient->disconnect();
    }
    
    pCtrlChar = nullptr;
    pDataChar = nullptr;
    pStatusChar = nullptr;
    
    state = State::IDLE;
    progress.inProgress = false;
    
    // Clear syncing flag on all devices
    for (auto& d : devices) {
        d.syncing = false;
    }
    
    // Reset dialogue state and pending flags (with mutex protection)
    dialogueState = DialogueState::IDLE;
    dialogueTimer = 0;
    taskENTER_CRITICAL(&pendingMux);
    pendingHelloReceived = false;
    pendingPMKIDCount = 0;
    pendingHSCount = 0;
    pendingDialogueId = 0;
    taskEXIT_CRITICAL(&pendingMux);
    
    Serial.println("[SON-OF-PIG] Disconnected");
}

bool CallPapaMode::isConnected() {
    return state == State::CONNECTED_WAITING_READY || state == State::CONNECTED || 
           state == State::SYNCING || state == State::WAITING_CHUNKS || 
           state == State::SYNC_COMPLETE;
}

bool CallPapaMode::startSync() {
    Serial.printf("[SON-OF-PIG] startSync() called, dialogueState=%d\n", (int)dialogueState.load());
    
    if (state != State::CONNECTED) {
        snprintf(lastError, sizeof(lastError), "Not connected");
        return false;
    }
    
    // Block manual sync while dialogue is in progress
    if (dialogueState != DialogueState::IDLE && 
        dialogueState != DialogueState::SYNC_RUNNING &&
        dialogueState != DialogueState::DONE) {
        Serial.println("[SON-OF-PIG] Sync blocked - dialogue in progress");
        return false;
    }
    
    if (remotePMKIDCount == 0 && remoteHSCount == 0) {
        Serial.println("[SON-OF-PIG] Nothing to sync - Papa is PISSED");
        // Show Papa's toxic roast
        Mood::setStatusMessage(PAPA_ROAST_RESPONSES[currentDialogueId % 3]);
        state = State::SYNC_COMPLETE;
        progress.inProgress = false;
        
        // Still send PURGE to trigger Sirloin's goodbye sequence
        sendCommand(CMD_PURGE_SYNCED);
        
        // Transition to goodbye after showing roast
        dialogueTimer = millis();
        __asm__ __volatile__("" ::: "memory");
        dialogueState = DialogueState::GOODBYE_PAPA;
        
        return true;
    }
    
    // Reset counters
    totalSynced = 0;
    syncedPMKIDs = 0;
    syncedHandshakes = 0;
    currentType = 0x01;  // Start with PMKIDs
    currentIndex = 0;
    
    Serial.printf("[SON-OF-PIG] Starting sync: %d PMKIDs, %d Handshakes\n",
                  remotePMKIDCount, remoteHSCount);
    
    Mood::setStatusMessage("SYNCING...");
    
    requestNextCapture();
    return true;
}

void CallPapaMode::abortSync() {
    if (state == State::SYNCING || state == State::WAITING_CHUNKS) {
        sendCommand(CMD_ABORT);
        state = State::CONNECTED;
        progress.inProgress = false;
    }
}

bool CallPapaMode::isSyncing() { 
    return state == State::SYNCING || state == State::WAITING_CHUNKS; 
}

bool CallPapaMode::isSyncComplete() { 
    return state == State::SYNC_COMPLETE; 
}

bool CallPapaMode::isSyncDialogueComplete() {
    return dialogueState == DialogueState::DONE;
}

uint32_t CallPapaMode::getCallDuration() {
    if (!isConnected()) return 0;
    return millis() - connectionStartTime;
}

uint8_t CallPapaMode::getDialoguePhase() {
    // Map DialogueState to phase numbers for UI
    // 0-2 = active dialogue, 3+ = done
    switch (dialogueState.load()) {
        case DialogueState::HELLO_PAPA:
        case DialogueState::HELLO_SON:
        case DialogueState::HELLO_LOOT:
            return 0;  // Phase 0: HELLO
        case DialogueState::SYNC_RUNNING:
            return 1;  // Phase 1: SYNCING
        case DialogueState::GOODBYE_PAPA:
        case DialogueState::GOODBYE_SON:
            return 2;  // Phase 2: GOODBYE
        case DialogueState::DONE:
            return 3;  // Phase 3: COMPLETE
        default:
            return 255;  // IDLE (no dialogue)
    }
}

// ============================================================================
// SAVING CAPTURES
// ============================================================================

// Deserialize and save PMKID
bool CallPapaMode::savePMKID(const uint8_t* data, uint16_t len) {
    // Guard: Skip if no SD card available (graceful degradation)
    if (!Config::isSDAvailable()) {
        Serial.println("[SON-OF-PIG] No SD card - PMKID not saved");
        return false;
    }
    
    // PMKID format: BSSID[6] + STATION[6] + SSID_LEN[1] + SSID[32] + PMKID[16] + TIMESTAMP[4] = 65 bytes
    if (len < 61) {  // Minimum: 6+6+1+0+16+4 = 33, but we expect at least some SSID
        Serial.printf("[SON-OF-PIG] PMKID data too short: %d bytes\n", len);
        return false;
    }
    
    const uint8_t* bssid = data;
    const uint8_t* station = data + 6;
    uint8_t ssidLen = data[12];
    if (ssidLen > 32) ssidLen = 32;
    const char* ssid = (const char*)(data + 13);
    const uint8_t* pmkid = data + 13 + 32;  // Fixed offset (32 bytes reserved for SSID)
    
    // Ensure /M5PORKCHOP/handshakes directory exists
    if (!SD.exists("/M5PORKCHOP/handshakes")) {
        SD.mkdir("/M5PORKCHOP/handshakes");
    }
    
    // Build filename
    char filename[64];
    snprintf(filename, sizeof(filename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X.22000",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    
    // Check for duplicate
    if (SD.exists(filename)) {
        Serial.printf("[SON-OF-PIG] PMKID already exists: %s\n", filename);
        return true;  // Consider as success (already have it)
    }
    
    File f = SD.open(filename, FILE_WRITE);
    if (!f) {
        Serial.printf("[SON-OF-PIG] Failed to create PMKID file: %s\n", filename);
        return false;
    }
    
    // Build hex strings
    char pmkidHex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(pmkidHex + i*2, "%02x", pmkid[i]);
    }
    
    char macAP[13];
    sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    
    char macClient[13];
    sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            station[0], station[1], station[2], station[3], station[4], station[5]);
    
    char essidHex[65];
    for (int i = 0; i < ssidLen; i++) {
        sprintf(essidHex + i*2, "%02x", (uint8_t)ssid[i]);
    }
    essidHex[ssidLen * 2] = 0;
    
    // WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
    f.printf("WPA*01*%s*%s*%s*%s***01\n", pmkidHex, macAP, macClient, essidHex);
    f.close();
    
    // Save SSID to companion txt file
    char txtFilename[64];
    snprintf(txtFilename, sizeof(txtFilename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X_pmkid.txt",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    
    File txtFile = SD.open(txtFilename, FILE_WRITE);
    if (txtFile) {
        char ssidCopy[33];
        strncpy(ssidCopy, ssid, ssidLen);
        ssidCopy[ssidLen] = '\0';
        txtFile.println(ssidCopy);
        txtFile.close();
    }
    
    Serial.printf("[SON-OF-PIG] PMKID saved: %s (SSID: %.*s)\n", filename, ssidLen, ssid);
    SDLog::log("SON-OF-PIG", "PMKID synced from Sirloin: %.*s", ssidLen, ssid);
    
    return true;
}

// Deserialize and save Handshake
bool CallPapaMode::saveHandshake(const uint8_t* data, uint16_t len) {
    // Guard: Skip if no SD card available (graceful degradation)
    if (!Config::isSDAvailable()) {
        Serial.println("[SON-OF-PIG] No SD card - Handshake not saved");
        return false;
    }
    
    // Handshake format (simplified for BLE transfer):
    // BSSID[6] + STATION[6] + SSID_LEN[1] + SSID[32] + MASK[1] + BEACON_LEN[2] + BEACON[n] + FRAMES...
    if (len < 48) {
        Serial.printf("[SON-OF-PIG] Handshake data too short: %d bytes\n", len);
        return false;
    }
    
    const uint8_t* bssid = data;
    const uint8_t* station = data + 6;
    uint8_t ssidLen = data[12];
    if (ssidLen > 32) ssidLen = 32;
    const char* ssid = (const char*)(data + 13);
    uint8_t mask = data[45];  // 13 + 32 = 45
    uint16_t beaconLen = data[46] | (data[47] << 8);
    
    // Ensure /M5PORKCHOP/handshakes directory exists
    if (!SD.exists("/M5PORKCHOP/handshakes")) {
        SD.mkdir("/M5PORKCHOP/handshakes");
    }
    
    // Build PCAP filename
    char pcapFilename[64];
    snprintf(pcapFilename, sizeof(pcapFilename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X.pcap",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    
    // Check for duplicate
    if (SD.exists(pcapFilename)) {
        Serial.printf("[SON-OF-PIG] Handshake already exists: %s\n", pcapFilename);
        return true;
    }
    
    // Write PCAP file
    File f = SD.open(pcapFilename, FILE_WRITE);
    if (!f) {
        Serial.printf("[SON-OF-PIG] Failed to create PCAP: %s\n", pcapFilename);
        return false;
    }
    
    // PCAP global header
    struct __attribute__((packed)) {
        uint32_t magic_number;
        uint16_t version_major;
        uint16_t version_minor;
        int32_t thiszone;
        uint32_t sigfigs;
        uint32_t snaplen;
        uint32_t network;
    } pcapHeader = {
        0xa1b2c3d4,  // Magic number
        2, 4,        // Version 2.4
        0, 0,        // Timezone, sigfigs
        65535,       // Snaplen
        127          // LINKTYPE_IEEE802_11_RADIO (radiotap)
    };
    f.write((uint8_t*)&pcapHeader, sizeof(pcapHeader));
    
    // Parse and write frames from data
    uint16_t offset = 48 + beaconLen;  // Skip header + beacon
    
    // Track first EAPOL timestamp for beacon (avoid timestamp mismatch)
    uint32_t firstEAPOLTimestamp = 0;
    
    // Parse and collect first EAPOL timestamp before writing packets
    // This ensures beacon and EAPOL frames have consistent timestamps
    uint16_t scanOffset = 48 + beaconLen;
    while (scanOffset < len && firstEAPOLTimestamp == 0) {
        if (scanOffset + 2 > len) break;
        uint16_t scanEapolLen = data[scanOffset] | (data[scanOffset + 1] << 8);
        scanOffset += 2;
        if (scanOffset + scanEapolLen > len) break;
        scanOffset += scanEapolLen;
        
        if (scanOffset + 2 > len) break;
        uint16_t scanFullLen = data[scanOffset] | (data[scanOffset + 1] << 8);
        scanOffset += 2;
        if (scanOffset + scanFullLen + 6 > len) break;  // +6 for msg_num, rssi, timestamp
        scanOffset += scanFullLen + 2;  // Skip full frame + msg_num + rssi
        
        if (scanOffset + 4 <= len) {
            firstEAPOLTimestamp = data[scanOffset] | (data[scanOffset + 1] << 8) |
                                 (data[scanOffset + 2] << 16) | (data[scanOffset + 3] << 24);
            break;
        }
        scanOffset += 4;
    }
    
    // Write beacon if present (use first EAPOL timestamp for consistency)
    if (beaconLen > 0) {
        const uint8_t* beaconData = data + 48;
        
        // Radiotap header (minimal 8 bytes)
        uint8_t radiotap[] = {0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00};
        
        // PCAP packet header - use first EAPOL timestamp (not millis!)
        uint32_t ts = firstEAPOLTimestamp > 0 ? firstEAPOLTimestamp : (millis() / 1000);
        struct __attribute__((packed)) {
            uint32_t ts_sec;
            uint32_t ts_usec;
            uint32_t incl_len;
            uint32_t orig_len;
        } pktHeader = {ts, 0, (uint32_t)(8 + beaconLen), (uint32_t)(8 + beaconLen)};
        
        f.write((uint8_t*)&pktHeader, sizeof(pktHeader));
        f.write(radiotap, 8);
        f.write(beaconData, beaconLen);
    }
    
    // Parse EAPOL frames from BLE transfer
    // Format per frame (from Sirloin's capture.cpp addHandshake):
    //   EAPOL_LEN[2] + EAPOL_DATA[n] + FULL_FRAME_LEN[2] + FULL_FRAME[m] + MSG_NUM[1] + RSSI[1] + TIMESTAMP[4]
    uint8_t framesProcessed = 0;
    while (offset < len && framesProcessed < 4) {
        // Need at least 2 bytes for EAPOL_LEN
        if (offset + 2 > len) break;
        
        // Read EAPOL payload length
        uint16_t eapolLen = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        
        // Validate EAPOL data bounds
        if (offset + eapolLen > len) {
            Serial.printf("[SON-OF-PIG] EAPOL data exceeds buffer (offset:%d eapolLen:%d len:%d)\n", 
                         offset, eapolLen, len);
            break;
        }
        
        // Skip EAPOL payload (we use fullFrame instead)
        offset += eapolLen;
        
        // Read full 802.11 frame length
        if (offset + 2 > len) break;
        uint16_t fullFrameLen = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        
        // Validate full frame data bounds
        if (offset + fullFrameLen > len) {
            Serial.printf("[SON-OF-PIG] Full frame exceeds buffer (offset:%d fullFrameLen:%d len:%d)\n", 
                         offset, fullFrameLen, len);
            break;
        }
        
        const uint8_t* fullFrameData = data + offset;
        offset += fullFrameLen;
        
        // Read metadata (MSG_NUM + RSSI + TIMESTAMP = 6 bytes)
        if (offset + 6 > len) break;
        uint8_t msgNum = data[offset++];
        int8_t rssi = (int8_t)data[offset++];
        uint32_t timestamp;
        memcpy(&timestamp, data + offset, 4);
        offset += 4;
        
        // Write PCAP packet with fullFrame (complete 802.11 frame)
        if (fullFrameLen > 0) {
            // Radiotap header (minimal 8 bytes)
            uint8_t radiotap[] = {0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00};
            
            uint32_t totalLen = 8 + fullFrameLen;
            struct __attribute__((packed)) {
                uint32_t ts_sec;
                uint32_t ts_usec;
                uint32_t incl_len;
                uint32_t orig_len;
            } pktHeader = {
                timestamp,      // Already Unix epoch seconds from Sirloin RTC
                0,              // No microsecond precision available
                totalLen,
                totalLen
            };
            
            f.write((uint8_t*)&pktHeader, sizeof(pktHeader));
            f.write(radiotap, 8);
            f.write(fullFrameData, fullFrameLen);
            
            Serial.printf("[SON-OF-PIG] EAPOL M%d written (%d bytes, RSSI:%d)\n", 
                         msgNum, fullFrameLen, rssi);
        }
        
        framesProcessed++;
    }
    
    if (framesProcessed == 0) {
        Serial.println("[SON-OF-PIG] WARNING: No frames processed!");
    }
    
    f.close();
    
    // Save SSID to companion txt file
    char txtFilename[64];
    snprintf(txtFilename, sizeof(txtFilename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X.txt",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    
    File txtFile = SD.open(txtFilename, FILE_WRITE);
    if (txtFile) {
        char ssidCopy[33];
        strncpy(ssidCopy, ssid, ssidLen);
        ssidCopy[ssidLen] = '\0';
        txtFile.println(ssidCopy);
        txtFile.close();
    }
    
    Serial.printf("[SON-OF-PIG] Handshake saved: %s (SSID: %.*s)\n", pcapFilename, ssidLen, ssid);
    SDLog::log("SON-OF-PIG", "Handshake synced from Sirloin: %.*s", ssidLen, ssid);
    
    return true;
}

bool CallPapaMode::hasValidDevices() {
    return !devices.empty();
}

bool CallPapaMode::isToastActive() {
    if (!toastActive) return false;
    uint32_t elapsed = millis() - toastStartTime;
    if (elapsed > TOAST_DURATION_MS) {
        toastActive = false;
        return false;
    }
    return true;
}

const char* CallPapaMode::getToastMessage() {
    return toastMessage.c_str();
}
