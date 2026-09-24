#pragma once

#include <Arduino.h>
#include <vector>

struct WdgWarsSyncResult {
    bool success;
    uint8_t uploaded;
    uint8_t failed;
    uint8_t skipped;
    char error[48];
};

typedef void (*WdgWarsProgressCallback)(const char*, uint8_t, uint8_t);

class WDGWars {
public:
    static bool hasCredentials();
    static bool isUploaded(const char* filename);
    static void removeFromUploaded(const char* filename);
    static void freeUploadedListMemory();
    static WdgWarsSyncResult syncFiles(WdgWarsProgressCallback cb = nullptr);

private:
    struct UploadedFile { char name[48]; };
    static std::vector<UploadedFile> uploadedFiles;
    static bool listLoaded;
    static char lastError[64];
    static int lastHttpStatus;
    static uint16_t retryAfterSeconds;
    static bool loadUploadedList();
    static bool saveUploadedList();
    static void markAsUploaded(const char* filename);
    static bool uploadSingleFile(const char* path);
    static void waitForRateLimit(uint16_t seconds, WdgWarsProgressCallback cb);
};
