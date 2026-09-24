// XferServer - WiFi file transfer server for SD card access
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

enum class XferServerState {
    IDLE,
    CONNECTING,
    RUNNING,
    RECONNECTING
};

class XferServer {
public:
    static void init();
    static bool start(const char* ssid, const char* password);
    static void stop();
    static void update();
    
    static bool isRunning() { return state == XferServerState::RUNNING; }
    static bool isConnecting() { return state == XferServerState::CONNECTING || state == XferServerState::RECONNECTING; }
    static bool isConnected() { return WiFi.status() == WL_CONNECTED; }
    static String getIP() { return WiFi.localIP().toString(); }
    static const char* getStatus() { return statusMessage; }
    static uint64_t getSDFreeSpace();
    static uint64_t getSDTotalSpace();
    static uint64_t getSessionRxBytes() { return sessionRxBytes; }
    static uint64_t getSessionTxBytes() { return sessionTxBytes; }
    static uint32_t getSessionUploadCount() { return sessionUploadCount; }
    static uint32_t getSessionDownloadCount() { return sessionDownloadCount; }
    
    // File operation helpers (shared with SD formatting)
    static bool deletePathRecursive(const String& path);
    
private:
    static WebServer* server;
    static XferServerState state;
    static char statusMessage[64];
    static char targetSSID[64];
    static char targetPassword[64];
    static uint32_t connectStartTime;
    static uint32_t lastReconnectCheck;
    static uint64_t sessionRxBytes;
    static uint64_t sessionTxBytes;
    static uint32_t sessionUploadCount;
    static uint32_t sessionDownloadCount;
    
    // State machine
    static void updateConnecting();
    static void updateRunning();
    static void startServer();
    
    // HTTP handlers
    static void handleRoot();
    static void handleStyle();
    static void handleScript();
    static void handleSwine();
    static void handleFileList();
    static void handleDownload();
    static void handleUpload();
    static void handleUploadProcess();
    static void handleDelete();
    static void handleBulkDelete();
    static void handleMkdir();
    static void handleSDInfo();
    static void handleRename();
    static void handleCopy();
    static void handleMove();
    static void handleNotFound();
    static void handleCreds();
    static void handleCredsSave();
    
    // File operation helpers
    static bool copyFileChunked(const String& srcPath, const String& dstPath);
    static bool copyPathRecursive(const String& srcPath, const String& dstPath, uint8_t depth = 0);
    
    // HTML template
    static const char* getHTML();
};
