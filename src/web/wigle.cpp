// WiGLE wardriving service client implementation

#include "wigle.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <base64.h>
#include "../core/config.h"
#include "../core/sdlog.h"

// Static member initialization
char WiGLE::lastError[64] = "";
char WiGLE::statusMessage[64] = "READY";
std::vector<String> WiGLE::uploadedFiles;
bool WiGLE::listLoaded = false;

void WiGLE::init() {
    uploadedFiles.clear();
    listLoaded = false;
    strcpy(lastError, "");
    strcpy(statusMessage, "READY");
    loadUploadedList();
}

// ============================================================================
// WiFi Connection (Standalone)
// ============================================================================

bool WiGLE::connect() {
    if (WiFi.status() == WL_CONNECTED) {
        strcpy(statusMessage, "ALREADY CONNECTED");
        return true;
    }
    
    String ssid = Config::wifi().otaSSID;
    String password = Config::wifi().otaPassword;
    
    if (ssid.isEmpty()) {
        strcpy(lastError, "NO WIFI SSID CONFIGURED");
        strcpy(statusMessage, "NO WIFI SSID");
        return false;
    }
    
    strcpy(statusMessage, "CONNECTING...");
    Serial.printf("[WIGLE] Connecting to %s\n", ssid.c_str());
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Wait for connection with timeout
    uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
        delay(100);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(statusMessage, sizeof(statusMessage), "IP: %s", WiFi.localIP().toString().c_str());
        Serial.printf("[WIGLE] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    
    strcpy(lastError, "CONNECTION TIMEOUT");
    strcpy(statusMessage, "CONNECT FAILED");
    Serial.println("[WIGLE] Connection failed");
    WiFi.disconnect(true);
    return false;
}

void WiGLE::disconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    strcpy(statusMessage, "DISCONNECTED");
    Serial.println("[WIGLE] Disconnected");
}

bool WiGLE::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

// ============================================================================
// Upload Tracking
// ============================================================================

bool WiGLE::loadUploadedList() {
    if (listLoaded) return true;  // Already loaded, skip SD read
    
    uploadedFiles.clear();
    
    if (!SD.exists(UPLOADED_FILE)) {
        listLoaded = true;
        return true;  // No file yet, that's OK
    }
    
    File f = SD.open(UPLOADED_FILE, FILE_READ);
    if (!f) return false;
    
    while (f.available() && uploadedFiles.size() < 200) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.isEmpty() && line.length() < 100) {  // Sanity check
            uploadedFiles.push_back(line);
        }
    }
    
    f.close();
    listLoaded = true;
    Serial.printf("[WIGLE] Loaded %d uploaded files from tracking\n", uploadedFiles.size());
    return true;
}

bool WiGLE::saveUploadedList() {
    File f = SD.open(UPLOADED_FILE, FILE_WRITE);
    if (!f) return false;
    
    for (const auto& filename : uploadedFiles) {
        f.println(filename);
    }
    
    f.close();
    return true;
}

bool WiGLE::isUploaded(const char* filename) {
    loadUploadedList();
    String name = getFilenameFromPath(filename);
    for (const auto& uploaded : uploadedFiles) {
        if (uploaded == name) return true;
    }
    return false;
}

void WiGLE::markUploaded(const char* filename) {
    String name = getFilenameFromPath(filename);
    // Check if already in list
    for (const auto& uploaded : uploadedFiles) {
        if (uploaded == name) return;
    }
    // Cap at 200 entries to prevent unbounded memory growth
    if (uploadedFiles.size() >= 200) {
        uploadedFiles.erase(uploadedFiles.begin());  // Remove oldest
    }
    uploadedFiles.push_back(name);
    saveUploadedList();
}

void WiGLE::removeFromUploaded(const char* filename) {
    String name = getFilenameFromPath(filename);
    for (auto it = uploadedFiles.begin(); it != uploadedFiles.end(); ++it) {
        if (*it == name) {
            uploadedFiles.erase(it);
            saveUploadedList();
            Serial.printf("[WIGLE] Removed from uploaded tracking: %s\n", name.c_str());
            return;
        }
    }
}

uint16_t WiGLE::getUploadedCount() {
    loadUploadedList();
    return uploadedFiles.size();
}

String WiGLE::getFilenameFromPath(const char* path) {
    String fullPath = path;
    int lastSlash = fullPath.lastIndexOf('/');
    if (lastSlash >= 0) {
        return fullPath.substring(lastSlash + 1);
    }
    return fullPath;
}

bool WiGLE::hasCredentials() {
    return !Config::wifi().wigleApiName.isEmpty() && 
           !Config::wifi().wigleApiToken.isEmpty();
}

// ============================================================================
// API Operations
// ============================================================================

bool WiGLE::uploadFile(const char* csvPath) {
    if (!isConnected()) {
        strcpy(lastError, "NOT CONNECTED TO WIFI");
        return false;
    }
    
    if (!hasCredentials()) {
        strcpy(lastError, "NO WIGLE API CREDENTIALS");
        return false;
    }
    
    if (!SD.exists(csvPath)) {
        snprintf(lastError, sizeof(lastError), "FILE NOT FOUND");
        return false;
    }
    
    File csvFile = SD.open(csvPath, FILE_READ);
    if (!csvFile) {
        strcpy(lastError, "CANNOT OPEN FILE");
        return false;
    }
    
    size_t fileSize = csvFile.size();
    // WiGLE limit is 180MB, but we'll be more conservative on ESP32
    if (fileSize > 500000) {  // 500KB limit for ESP32 memory safety
        strcpy(lastError, "FILE TOO LARGE (>500KB)");
        csvFile.close();
        return false;
    }
    
    strcpy(statusMessage, "UPLOADING...");
    Serial.printf("[WIGLE] Uploading %s (%d bytes)\n", csvPath, fileSize);
    
    // Build multipart form data boundaries
    String boundary = "----PorkchopWiGLE" + String(millis());
    String filename = getFilenameFromPath(csvPath);
    
    // Build body parts (headers only, file streamed separately)
    String bodyStart = "--" + boundary + "\r\n";
    bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
    bodyStart += "Content-Type: text/csv\r\n\r\n";
    
    String bodyEnd = "\r\n--" + boundary + "--\r\n";
    
    size_t contentLength = bodyStart.length() + fileSize + bodyEnd.length();
    
    // Build Basic Auth header
    String apiName = Config::wifi().wigleApiName;
    String apiToken = Config::wifi().wigleApiToken;
    String credentials = apiName + ":" + apiToken;
    String authHeader = "Basic " + base64::encode(credentials);
    
    WiFiClientSecure client;
    client.setInsecure();  // Skip certificate validation
    client.setTimeout(60);  // 60 second timeout for large files
    
    if (!client.connect(API_HOST, 443)) {
        strcpy(lastError, "Connection failed");
        csvFile.close();
        return false;
    }
    
    // Send HTTP headers
    client.print("POST " + String(UPLOAD_PATH) + " HTTP/1.1\r\n");
    client.print("Host: " + String(API_HOST) + "\r\n");
    client.print("Authorization: " + authHeader + "\r\n");
    client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    client.print("Content-Length: " + String(contentLength) + "\r\n");
    client.print("Connection: close\r\n\r\n");
    
    // Send multipart header
    client.print(bodyStart);
    
    // Stream file in chunks (4KB at a time) - avoid loading entire file in RAM
    const size_t CHUNK_SIZE = 4096;
    uint8_t chunk[CHUNK_SIZE];
    size_t bytesRemaining = fileSize;
    size_t bytesSent = 0;
    
    while (bytesRemaining > 0 && csvFile.available()) {
        size_t toRead = (bytesRemaining > CHUNK_SIZE) ? CHUNK_SIZE : bytesRemaining;
        size_t bytesRead = csvFile.read(chunk, toRead);
        
        if (bytesRead == 0) {
            Serial.println("[WIGLE] Read error during upload");
            break;
        }
        
        size_t written = client.write(chunk, bytesRead);
        if (written != bytesRead) {
            Serial.printf("[WIGLE] Write error: %d of %d bytes\n", written, bytesRead);
            strcpy(lastError, "WRITE ERROR");
            csvFile.close();
            client.stop();
            return false;
        }
        
        bytesSent += bytesRead;
        bytesRemaining -= bytesRead;
        
        // Yield to prevent watchdog timeout on large files
        yield();
    }
    
    csvFile.close();
    
    if (bytesSent != fileSize) {
        Serial.printf("[WIGLE] Incomplete upload: %d of %d bytes\n", bytesSent, fileSize);
        strcpy(lastError, "INCOMPLETE UPLOAD");
        client.stop();
        return false;
    }
    
    // Send multipart footer
    client.print(bodyEnd);
    client.flush();  // Ensure all data is sent before waiting for response
    
    Serial.printf("[WIGLE] Sent %d bytes, waiting for response...\n", bytesSent);
    
    // Read response - increase timeout for larger files (WiGLE processing time)
    uint32_t timeout = millis();
    while (client.connected() && !client.available() && millis() - timeout < 60000) {
        delay(10);
        yield();  // Keep watchdog happy
    }
    
    if (!client.available()) {
        strcpy(lastError, "RESPONSE TIMEOUT");
        client.stop();
        return false;
    }
    
    // Read full response for debugging
    String statusLine = client.readStringUntil('\n');
    Serial.printf("[WIGLE] Response: %s\n", statusLine.c_str());
    
    // Skip headers to get to body
    while (client.available()) {
        String line = client.readStringUntil('\n');
        if (line == "\r" || line.isEmpty()) break;
    }
    
    // Read body (JSON response) - limit to 2KB to prevent memory issues on error pages
    String body;
    body.reserve(512);  // Pre-allocate typical response size
    size_t bodyBytesRead = 0;
    while (client.available() && bodyBytesRead < 2048) {
        char c = client.read();
        body += c;
        bodyBytesRead++;
    }
    client.stop();
    
    Serial.printf("[WIGLE] Body: %s\n", body.c_str());
    
    // Check for success (200 response and "success":true in body)
    if (statusLine.indexOf("200") > 0 && body.indexOf("\"success\":true") >= 0) {
        strcpy(statusMessage, "UPLOAD OK");
        Serial.println("[WIGLE] Upload successful");
        SDLog::log("WIGLE", "Upload OK: %s", filename.c_str());
        
        // Mark as uploaded
        markUploaded(csvPath);
        
        return true;
    }
    
    // Parse error message if present
    int msgStart = body.indexOf("\"message\":\"");
    if (msgStart >= 0) {
        msgStart += 11;
        int msgEnd = body.indexOf("\"", msgStart);
        if (msgEnd > msgStart) {
            String errMsg = body.substring(msgStart, msgEnd);
            snprintf(lastError, sizeof(lastError), "%.60s", errMsg.c_str());
        }
    } else {
        snprintf(lastError, sizeof(lastError), "HTTP %s", statusLine.substring(9, 12).c_str());
    }
    
    strcpy(statusMessage, "UPLOAD FAILED");
    SDLog::log("WIGLE", "Upload failed: %s", filename.c_str());
    Serial.printf("[WIGLE] Upload failed: %s\n", lastError);
    return false;
}

const char* WiGLE::getLastError() {
    return lastError;
}

const char* WiGLE::getStatus() {
    return statusMessage;
}

