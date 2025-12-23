// WiFi File Server for SD card access
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

enum class FileServerState {
    IDLE,
    CONNECTING,
    RUNNING,
    RECONNECTING
};

class FileServer {
public:
    static void init();
    static bool start(const char* ssid, const char* password);
    static void stop();
    static void update();
    
    static bool isRunning() { return state == FileServerState::RUNNING; }
    static bool isConnecting() { return state == FileServerState::CONNECTING || state == FileServerState::RECONNECTING; }
    static bool isConnected() { return WiFi.status() == WL_CONNECTED; }
    static String getIP() { return WiFi.localIP().toString(); }
    static const char* getStatus() { return statusMessage; }
    static uint64_t getSDFreeSpace();
    static uint64_t getSDTotalSpace();
    
private:
    static WebServer* server;
    static FileServerState state;
    static char statusMessage[64];
    static char targetSSID[64];
    static char targetPassword[64];
    static uint32_t connectStartTime;
    static uint32_t lastReconnectCheck;
    
    // State machine
    static void updateConnecting();
    static void updateRunning();
    static void startServer();
    
    // HTTP handlers
    static void handleRoot();
    static void handleFileList();
    static void handleDownload();
    static void handleUpload();
    static void handleUploadProcess();
    static void handleDelete();
    static void handleBulkDelete();
    static void handleMkdir();
    static void handleSDInfo();
    static void handleNotFound();
    
    // HTML template
    static const char* getHTML();
};
