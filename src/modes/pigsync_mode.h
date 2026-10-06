/**
 * PigSync Mode (Porkchop/POPS side)
 *
 * SON OF A PIG - Reliable sync with Sirloin devices
 */

#ifndef PIGSYNC_MODE_H
#define PIGSYNC_MODE_H

#include <Arduino.h>
#include <vector>

// Discovered Sirloin device
struct SirloinDevice {
    uint8_t mac[6];
    int8_t rssi;
    uint16_t pendingCaptures;
    uint8_t flags;
    uint32_t lastSeen;
    bool syncing;
    char name[16];
    
    // Phase 3: Extended info from BeaconGrunt
    uint8_t batteryPercent;
    uint8_t storagePercent;
    uint8_t moodTier;        // 0-7 from grunt flags
    uint32_t rtcTime;        // Unix time from grunt
    uint16_t uptimeMin;      // Uptime in minutes
    bool hasGruntInfo;       // True if we received grunt data
};

// Transfer progress
struct SyncProgress {
    uint16_t currentChunk;
    uint16_t totalChunks;
    uint32_t bytesReceived;
    uint32_t startTime;
    uint8_t captureType;
    uint16_t captureIndex;
    bool inProgress;
};

class PigSyncMode {
public:
    // State machine - public for UI access
    enum class State {
        IDLE,
        SCANNING,
        CONNECTING,
        RINGING,
        CONNECTED_WAITING_READY,
        CONNECTED,
        SYNCING,
        WAITING_CHUNKS,
        SYNC_COMPLETE,
        ERROR
    };
    // ==[ LIFECYCLE ]==
    static void init();
    static bool ensureEspNowReady();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    // ==[ DISCOVERY ]==
    static void startDiscovery();
    static void stopDiscovery();
    static bool isScanning();
    static const std::vector<SirloinDevice>& getDevices() { return devices; }
    static uint8_t getDeviceCount() { return devices.size(); }
    static std::vector<SirloinDevice>& getDevicesMutable() { return devices; }
    static bool isSirloinAvailable() { return !devices.empty(); }
    static bool hasValidDevices();

    // ==[ UI HELPERS ]==
    static const SirloinDevice* getDevice(uint8_t index) {
        return (index < devices.size()) ? &devices[index] : nullptr;
    }
    static void getDeviceDisplayName(uint8_t index, char* buffer, size_t bufferSize);
    static void getStatusMessage(char* buffer, size_t bufferSize);
    static void handleKeyboardInput();

    // ==[ CONNECTION ]==
    static bool connectTo(uint8_t deviceIndex);
    static void disconnect();
    static bool isConnected();
    static bool isConnecting();  // True during CONNECTING state
    static const SirloinDevice* getConnectedDevice();
    static State getState() { return state; }
    
    // ==[ SYNC OPERATIONS ]==
    static bool startSync();
    static void abortSync();
    static bool isSyncing();
    static bool isSyncComplete();
    static uint8_t getSyncProgress();

    // ==[ PROGRESS ]==
    static SyncProgress getProgress() { return progress; }
    static const char* getLastError() { return lastError; }
    
    // ==[ CAPTURE STATS ]==
    static uint16_t getRemotePMKIDCount() { return remotePMKIDCount; }
    static uint16_t getRemoteHSCount() { return remoteHSCount; }
    static uint16_t getRemoteHandshakeCount() { return remoteHSCount; }  // Alias
    static uint16_t getTotalSynced() { return totalSynced; }
    static uint16_t getSyncedPMKIDs() { return syncedPMKIDs; }
    static uint16_t getSyncedHandshakes() { return syncedHandshakes; }
    static uint16_t getSyncedCount() { return syncedPMKIDs + syncedHandshakes; }
    static uint16_t getTotalToSync() { return remotePMKIDCount + remoteHSCount; }
    
    // ==[ BOUNTY STATS ]==
    static uint8_t getLastBountyMatches() { return lastBountyMatches; }
    static int8_t getLastSirloinMood() { return (int8_t)(remoteMood - 128); }
    
    // ==[ UI SELECTION ]==
    static void selectDevice(uint8_t index) { selectedIndex = index; }
    static uint8_t getSelectedIndex() { return selectedIndex; }
    static uint8_t getDataChannel() { return dataChannel; }
    static bool consumeNameReveal(char* buffer, size_t bufferSize);

    // ==[ UI CHANNEL DISPLAY ]==
    static uint8_t getDiscoveryChannel() { return 1; } // Protocol hardcodes ch1, UI shows this
    
    // ==[ DIALOGUE COMPLETE CHECK ]==
    static bool isSyncDialogueComplete() { return dialoguePhase >= 3; }
    
    // ==[ SCAN ALIASES (for compatibility) ]==
    static void startScan() { startDiscovery(); }
    static void stopScan() { stopDiscovery(); }
    
    // ==[ DIALOGUE SYSTEM ]==
    static uint32_t getCallDuration();
    static uint8_t getDialoguePhase();
    static uint8_t getDialogueId() { return dialogueId; }
    
    // Papa's lines (we show these)
    static const char* getPapaHelloPhrase();
    static const char* getPapaGoodbyePhrase();
    
    // Son's lines (shown as toast from SON)
    static const char* getSonHelloPhrase();
    static const char* getSonGoodbyePhrase();
    
    // ==[ SESSION INFO ]==
    static uint16_t getSessionId() { return sessionId; }

    // ==[ CALLBACKS ]==
    typedef void (*CaptureCallback)(uint8_t type, const uint8_t* data, uint16_t len);
    typedef void (*SyncCompleteCallback)(uint16_t pmkids, uint16_t handshakes);

    static void setOnCapture(CaptureCallback cb) { onCaptureCb = cb; }
    static void setOnSyncComplete(SyncCompleteCallback cb) { onSyncCompleteCb = cb; }

private:
    static bool running;
    static bool initialized;
    static uint8_t selectedIndex;
    
    static std::vector<SirloinDevice> devices;
    static uint8_t connectedMac[6];
    static bool connected;
    
    // Remote device counts
    static uint16_t remotePMKIDCount;
    static uint16_t remoteHSCount;
    static uint16_t totalSynced;
    static uint16_t syncedPMKIDs;
    static uint16_t syncedHandshakes;

    static State state;
    
    // Sync state
    static uint8_t currentType;
    static uint16_t currentIndex;
    static uint16_t totalChunks;
    static uint16_t receivedChunks;
    
    // Constants (must be before arrays that use them)
    static const uint16_t RX_BUFFER_SIZE = 2048;
    
    // Transfer state
    static SyncProgress progress;
    static uint8_t rxBuffer[RX_BUFFER_SIZE];
    static uint16_t rxBufferLen;
    static char lastError[64];
    
    // Dialogue
    static uint8_t dialogueId;
    static uint8_t dialoguePhase;
    static uint32_t callStartTime;
    static uint32_t phraseStartTime;
    static char papaGoodbyeSelected[64];
    
    // Discovery timing
    static uint32_t lastDiscoveryTime;
    static uint32_t discoveryStartTime;
    static bool scanning;
    static uint32_t connectStartTime;
    static uint32_t lastHelloTime;     // Track CMD_HELLO retries
    static uint8_t helloRetryCount;    // CMD_HELLO retry count
    static uint32_t readyStartTime;    // Track timeout for RSP_READY
    static uint8_t channelRetryCount;  // Retries for channel switch fallback
    static uint32_t syncCompleteTime;  // Track when sync completed for cleanup
    
    // Session
    static uint16_t sessionId;   // 16-bit session token
    static uint8_t remoteMood;
    static uint8_t lastBountyMatches;
    static uint8_t dataChannel;  // Negotiated data channel
    
    // Callbacks
    static CaptureCallback onCaptureCb;
    static SyncCompleteCallback onSyncCompleteCb;
    
    // Protocol helpers
    static void sendCommand(uint8_t type);
    static void sendDiscover();
    static void sendHello();
    static void sendReady();
    static void sendStartSync(uint8_t captureType, uint16_t index);
    static void sendAckChunk(uint16_t seq);
    static void sendMarkSynced(uint8_t captureType, uint16_t index);
    static void sendPurge();
    static void sendBounties();
    static void sendTimeSync();  // Phase 3: Request RTC time from Sirloin
    static void requestNextCapture();
    
    // Saving
    static bool savePMKID(const uint8_t* data, uint16_t len);
    static bool saveHandshake(const uint8_t* data, uint16_t len);
};

#endif // PIGSYNC_MODE_H
