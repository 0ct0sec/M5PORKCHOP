// SD Card Logger implementation

#include "sdlog.h"
#include "config.h"
#include <SD.h>
#include <stdarg.h>

bool SDLog::logEnabled = false;
bool SDLog::initialized = false;
String SDLog::currentLogFile = "";

void SDLog::init() {
    initialized = true;
    // Logging starts disabled, user enables via settings
}

void SDLog::setEnabled(bool enabled) {
    Serial.printf("[SDLOG] setEnabled(%s), SD available: %s\n", 
                  enabled ? "true" : "false", 
                  Config::isSDAvailable() ? "true" : "false");
    
    logEnabled = enabled && Config::isSDAvailable();
    
    if (logEnabled && currentLogFile.length() == 0) {
        ensureLogFile();
    }
    
    if (logEnabled) {
        Serial.printf("[SDLOG] Logging now ENABLED to: %s\n", currentLogFile.c_str());
        log("SDLOG", "SD logging enabled");
    } else {
        Serial.printf("[SDLOG] Logging DISABLED\n");
    }
}

void SDLog::ensureLogFile() {
    if (currentLogFile.length() > 0) return;
    if (!Config::isSDAvailable()) return;
    
    // Create logs directory if needed
    if (!SD.exists("/M5PORKCHOP/logs")) {
        SD.mkdir("/M5PORKCHOP/logs");
    }
    
    // Use fixed filename - easier to find and read
    currentLogFile = "/M5PORKCHOP/logs/porkchop.log";
    
    // Create file with header
    File f = SD.open(currentLogFile.c_str(), FILE_WRITE);
    if (f) {
        f.println("=== PORKCHOP LOG ===");
        f.printf("Started at millis: %lu\n", millis());
        f.println("====================");
        f.close();
        Serial.printf("[SDLOG] Log file: %s\n", currentLogFile.c_str());
    } else {
        Serial.printf("[SDLOG] Failed to create: %s\n", currentLogFile.c_str());
        currentLogFile = "";
    }
}

void SDLog::log(const char* tag, const char* format, ...) {
    // Check state - using member variable directly (not inline function)
    if (!logEnabled) {
        return;
    }
    if (currentLogFile.length() == 0) {
        // Try to create log file if it doesn't exist
        ensureLogFile();
        if (currentLogFile.length() == 0) {
            Serial.printf("[SDLOG] ERROR: Could not create log file\n");
            return;
        }
    }
    
    // Format the message first (before SD operations)
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    // Debug: show what we're logging
    Serial.printf("[SDLOG->SD] [%s] %s\n", tag, buffer);
    
    // Try to open file with retry on failure (SD can be busy with other operations)
    File f;
    for (int retry = 0; retry < 3; retry++) {
        f = SD.open(currentLogFile.c_str(), FILE_APPEND);
        if (f) break;
        delay(5);  // Brief delay before retry
    }
    
    if (!f) {
        Serial.printf("[SDLOG] Failed to open log file for append\n");
        return;
    }
    
    // Write timestamp, tag, and message
    f.printf("[%lu][%s] %s\n", millis(), tag, buffer);
    f.close();
}

void SDLog::logRaw(const char* message) {
    if (!logEnabled) return;
    if (currentLogFile.length() == 0) {
        ensureLogFile();
        if (currentLogFile.length() == 0) return;
    }
    
    // Try to open file with retry on failure
    File f;
    for (int retry = 0; retry < 3; retry++) {
        f = SD.open(currentLogFile.c_str(), FILE_APPEND);
        if (f) break;
        delay(5);
    }
    
    if (f) {
        f.println(message);
        f.close();
    }
}

void SDLog::flush() {
    // Files are closed after each write, so nothing to flush
}

void SDLog::close() {
    if (logEnabled && currentLogFile.length() > 0) {
        log("SDLOG", "Log closed");
    }
    currentLogFile = "";
}
