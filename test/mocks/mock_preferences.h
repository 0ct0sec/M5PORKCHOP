// Mock ESP32 Preferences (NVS) for native unit testing
#pragma once

#include <cstdint>
#include <map>
#include <string>

// Simplified Preferences mock that stores values in memory
class Preferences {
public:
    Preferences() : opened(false), ns("") {}
    
    bool begin(const char* name, bool readOnly = false) {
        ns = name;
        opened = true;
        return true;
    }
    
    void end() {
        opened = false;
    }
    
    bool clear() {
        storage.clear();
        return true;
    }
    
    bool remove(const char* key) {
        storage.erase(makeKey(key));
        return true;
    }
    
    // Put methods
    size_t putChar(const char* key, int8_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 1;
    }
    
    size_t putUChar(const char* key, uint8_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 1;
    }
    
    size_t putShort(const char* key, int16_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 2;
    }
    
    size_t putUShort(const char* key, uint16_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 2;
    }
    
    size_t putInt(const char* key, int32_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 4;
    }
    
    size_t putUInt(const char* key, uint32_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 4;
    }
    
    size_t putLong(const char* key, int32_t value) {
        return putInt(key, value);
    }
    
    size_t putULong(const char* key, uint32_t value) {
        return putUInt(key, value);
    }
    
    size_t putLong64(const char* key, int64_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 8;
    }
    
    size_t putULong64(const char* key, uint64_t value) {
        storage[makeKey(key)] = std::to_string(value);
        return 8;
    }
    
    size_t putFloat(const char* key, float value) {
        storage[makeKey(key)] = std::to_string(value);
        return 4;
    }
    
    size_t putDouble(const char* key, double value) {
        storage[makeKey(key)] = std::to_string(value);
        return 8;
    }
    
    size_t putBool(const char* key, bool value) {
        storage[makeKey(key)] = value ? "1" : "0";
        return 1;
    }
    
    size_t putString(const char* key, const char* value) {
        storage[makeKey(key)] = value;
        return strlen(value);
    }
    
    // Get methods with defaults
    int8_t getChar(const char* key, int8_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return (int8_t)std::stoi(it->second);
    }
    
    uint8_t getUChar(const char* key, uint8_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return (uint8_t)std::stoul(it->second);
    }
    
    int16_t getShort(const char* key, int16_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return (int16_t)std::stoi(it->second);
    }
    
    uint16_t getUShort(const char* key, uint16_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return (uint16_t)std::stoul(it->second);
    }
    
    int32_t getInt(const char* key, int32_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return (int32_t)std::stoi(it->second);
    }
    
    uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return (uint32_t)std::stoul(it->second);
    }
    
    int32_t getLong(const char* key, int32_t defaultValue = 0) {
        return getInt(key, defaultValue);
    }
    
    uint32_t getULong(const char* key, uint32_t defaultValue = 0) {
        return getUInt(key, defaultValue);
    }
    
    int64_t getLong64(const char* key, int64_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return std::stoll(it->second);
    }
    
    uint64_t getULong64(const char* key, uint64_t defaultValue = 0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return std::stoull(it->second);
    }
    
    float getFloat(const char* key, float defaultValue = 0.0f) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return std::stof(it->second);
    }
    
    double getDouble(const char* key, double defaultValue = 0.0) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return std::stod(it->second);
    }
    
    bool getBool(const char* key, bool defaultValue = false) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) return defaultValue;
        return it->second == "1";
    }
    
    size_t getString(const char* key, char* value, size_t maxLen) {
        auto it = storage.find(makeKey(key));
        if (it == storage.end()) { value[0] = '\0'; return 0; }
        strncpy(value, it->second.c_str(), maxLen - 1);
        value[maxLen - 1] = '\0';
        return strlen(value);
    }
    
    // Test helper - clear all storage across all namespaces
    static void clearAll() {
        storage.clear();
    }
    
private:
    bool opened;
    std::string ns;
    static std::map<std::string, std::string> storage;
    
    std::string makeKey(const char* key) {
        return ns + ":" + key;
    }
};

// Static storage definition - inline to avoid multiple definition errors
inline std::map<std::string, std::string> Preferences::storage;
