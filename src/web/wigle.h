// WiGLE wardriving service client
// https://wigle.net/
#pragma once

#include <Arduino.h>
#include <vector>

// Upload status for tracking
enum class WigleUploadStatus {
    NOT_UPLOADED,
    UPLOADED,
    PROCESSING,
    COMPLETE
};

class WiGLE {
public:
    static void init();
    
    // WiFi connection (standalone, uses otaSSID/otaPassword from config)
    static bool connect();
    static void disconnect();
    static bool isConnected();
    
    // API operations (require WiFi connection)
    static bool uploadFile(const char* csvPath);  // POST WiGLE CSV file
    
    // Upload tracking
    static bool isUploaded(const char* filename);     // Check if already uploaded
    static void markUploaded(const char* filename);   // Mark as uploaded
    static void removeFromUploaded(const char* filename); // Remove from tracking
    static uint16_t getUploadedCount();               // Total uploads tracked
    
    // Status
    static const char* getLastError();
    static const char* getStatus();
    
    // API credentials check
    static bool hasCredentials();
    
private:
    static char lastError[64];
    static char statusMessage[64];
    
    // Uploaded files tracking
    static std::vector<String> uploadedFiles;
    static bool listLoaded;  // Guard for lazy loading
    
    // File paths
    static constexpr const char* UPLOADED_FILE = "/M5PORKCHOP/wigle/wigle_uploaded.txt";
    
    // API endpoints
    static constexpr const char* API_HOST = "api.wigle.net";
    static constexpr const char* UPLOAD_PATH = "/api/v2/file/upload";
    
    // Helpers
    static bool loadUploadedList();
    static bool saveUploadedList();
    static String getFilenameFromPath(const char* path);
};

