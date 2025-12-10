// Mock Arduino types and functions for native unit testing
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>
#include <cstdlib>

// Arduino-style type definitions
typedef uint8_t byte;

// Time functions - use static counter for deterministic testing
inline uint32_t millis() {
    static uint32_t ms = 0;
    return ms;
}

inline void setMillis(uint32_t ms) {
    // For test control - not in real Arduino
    static uint32_t* ptr = nullptr;
    // This is a workaround - tests can call millis() and it returns 0
}

inline uint32_t micros() {
    return millis() * 1000;
}

inline void delay(uint32_t ms) {
    // No-op in tests
}

inline void delayMicroseconds(uint32_t us) {
    // No-op in tests
}

// Random - use stdlib for tests
inline long random(long max) {
    return std::rand() % max;
}

inline long random(long min, long max) {
    return min + (std::rand() % (max - min));
}

inline void randomSeed(unsigned long seed) {
    std::srand(seed);
}

// Math macros/functions
#ifndef PI
#define PI 3.14159265358979323846
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif

#ifndef abs
#define abs(x) ((x)>0?(x):-(x))
#endif

#ifndef constrain
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#endif

#ifndef map
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
#endif

// String class stub (simplified)
class String {
public:
    String() : str("") {}
    String(const char* s) : str(s ? s : "") {}
    String(const String& s) : str(s.str) {}
    String(int value) : str(std::to_string(value)) {}
    String(unsigned int value) : str(std::to_string(value)) {}
    String(long value) : str(std::to_string(value)) {}
    String(unsigned long value) : str(std::to_string(value)) {}
    String(float value, int decimalPlaces = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimalPlaces, value);
        str = buf;
    }
    
    const char* c_str() const { return str.c_str(); }
    size_t length() const { return str.length(); }
    bool isEmpty() const { return str.empty(); }
    
    String& operator=(const char* s) { str = s ? s : ""; return *this; }
    String& operator=(const String& s) { str = s.str; return *this; }
    String& operator+=(const char* s) { if (s) str += s; return *this; }
    String& operator+=(const String& s) { str += s.str; return *this; }
    String& operator+=(char c) { str += c; return *this; }
    
    String operator+(const char* s) const { return String((str + (s ? s : "")).c_str()); }
    String operator+(const String& s) const { return String((str + s.str).c_str()); }
    
    bool operator==(const char* s) const { return str == (s ? s : ""); }
    bool operator==(const String& s) const { return str == s.str; }
    bool operator!=(const char* s) const { return !(*this == s); }
    bool operator!=(const String& s) const { return !(*this == s); }
    
    char operator[](size_t index) const { return str[index]; }
    char& operator[](size_t index) { return str[index]; }
    
    int indexOf(char c) const { 
        size_t pos = str.find(c);
        return pos == std::string::npos ? -1 : (int)pos;
    }
    
    String substring(size_t from) const { return String(str.substr(from).c_str()); }
    String substring(size_t from, size_t to) const { return String(str.substr(from, to - from).c_str()); }
    
    void trim() {
        size_t start = str.find_first_not_of(" \t\r\n");
        size_t end = str.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) str = "";
        else str = str.substr(start, end - start + 1);
    }
    
    void toLowerCase() {
        for (char& c : str) c = tolower(c);
    }
    
    void toUpperCase() {
        for (char& c : str) c = toupper(c);
    }
    
    int toInt() const { return std::atoi(str.c_str()); }
    float toFloat() const { return std::atof(str.c_str()); }
    
private:
    std::string str;
};

// Serial stub
class SerialClass {
public:
    void begin(long baud) {}
    void end() {}
    void print(const char* s) {}
    void print(int n) {}
    void print(unsigned int n) {}
    void print(long n) {}
    void print(unsigned long n) {}
    void print(float n, int decimals = 2) {}
    void print(double n, int decimals = 2) {}
    void print(const String& s) {}
    void println() {}
    void println(const char* s) {}
    void println(int n) {}
    void println(unsigned int n) {}
    void println(long n) {}
    void println(unsigned long n) {}
    void println(float n, int decimals = 2) {}
    void println(double n, int decimals = 2) {}
    void println(const String& s) {}
    void printf(const char* format, ...) {}
    int available() { return 0; }
    int read() { return -1; }
    void write(uint8_t b) {}
    void write(const uint8_t* buf, size_t len) {}
    void flush() {}
    operator bool() { return true; }
};

// GPIO stubs
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define HIGH 1
#define LOW 0

inline void pinMode(uint8_t pin, uint8_t mode) {}
inline void digitalWrite(uint8_t pin, uint8_t val) {}
inline int digitalRead(uint8_t pin) { return LOW; }
inline int analogRead(uint8_t pin) { return 0; }
inline void analogWrite(uint8_t pin, int val) {}

// Define Serial instance for linking
inline SerialClass Serial;
