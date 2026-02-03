// Weather effects module - clouds, rain, thunder, wind
// Mood-tied weather system ported from Sirloin

#include "weather.h"
#include "avatar.h"
#include "../ui/display.h"
#include <esp_random.h>

namespace Weather {

// === CLOUD PARALLAX STATE ===
static char cloudPattern[40] = {0};
static bool cloudMoving = true;  // Always drift
static bool cloudDirection = true;  // true = right
static uint32_t lastCloudUpdate = 0;
static uint16_t cloudSpeed = 14400;  // Ultra slow atmospheric drift (matches sirloin)
static uint32_t lastCloudParallax = 0;
static const uint8_t CLOUD_PARALLAX_GRASS_SHIFTS = 6;  // Shift clouds every N grass shifts

// === RAIN STATE ===
struct RainDrop {
    float x;
    float y;
    uint8_t speed; // pixels per update for visible rain
};
static const int RAIN_DROP_COUNT = 25;  // Increased for denser rain
static RainDrop rainDrops[RAIN_DROP_COUNT] = {{0}};
static bool rainActive = false;
static bool rainDecided = false;  // Prevents per-frame re-randomization
static int lastMoodTier = -1;     // Track tier (not raw mood) with hysteresis
static uint32_t lastRainUpdate = 0;
static const uint16_t RAIN_SPEED_MS = 30;  // Fast updates for snappy rain

// === THUNDER STATE ===
static bool thunderFlashing = false;
static uint32_t lastThunderStorm = 0;
static uint32_t thunderFlashStart = 0;
static uint8_t thunderFlashesRemaining = 0;
static uint8_t thunderFlashState = 0;  // 0=off, 1=on
static uint32_t thunderMinInterval = 50000;  // 50-90s between storms (adjusts with mood)
static uint32_t thunderMaxInterval = 90000;

// === WIND STATE ===
struct WindParticle {
    float x;
    float y;
    float speed;
    bool active;
};
static WindParticle windParticles[6] = {{0}};
static bool windActive = false;
static uint32_t lastWindGust = 0;
static uint32_t windGustDuration = 0;
static uint32_t windGustInterval = 15000;  // 15-30s between gusts
static uint32_t lastWindUpdate = 0;

// === MOOD-BASED WEATHER CONTROL ===
static int currentMood = 50;  // Cached mood level

// Forward declaration
static void resetCloudPattern();
static void shiftCloudPattern(bool direction, bool allowMutation);

// === INITIALIZATION ===
void init() {
    // Init cloud pattern - scattered dots/dashes with spacing
    resetCloudPattern();
    
    // Init wind particles (inactive)
    for (int i = 0; i < 6; i++) {
        windParticles[i].active = false;
    }
    
    lastCloudUpdate = millis();
    lastCloudParallax = lastCloudUpdate;
    lastWindGust = millis();
    lastThunderStorm = millis();
}

static void resetCloudPattern() {
    // Generate textured cloud pattern with multi-segment clusters
    const char cloudChars[] = {'.', '-', '_'};
    
    // Fill with spaces first
    for (int i = 0; i < 39; i++) {
        cloudPattern[i] = ' ';
    }
    cloudPattern[39] = '\0';
    
    int pos = 0;
    while (pos < 36) {
        // Create a cloud entity (2-4 segments for texture)
        int segments = random(2, 5);
        
        for (int s = 0; s < segments && pos < 39; s++) {
            char segChar = cloudChars[random(0, 3)];
            int segLen = random(1, 6);  // 1 to 5 chars per segment
            
            for (int k = 0; k < segLen && pos < 39; k++) {
                cloudPattern[pos++] = segChar;
            }
        }
        
        // Add gap between clouds
        int gap = random(4, 10);  // 4 to 9 spaces
        pos += gap;
    }
}

static void shiftCloudPattern(bool direction, bool allowMutation) {
    if (direction) {
        // Shift right
        char last = cloudPattern[38];
        for (int i = 38; i > 0; i--) {
            cloudPattern[i] = cloudPattern[i - 1];
        }
        cloudPattern[0] = last;
    } else {
        // Shift left
        char first = cloudPattern[0];
        for (int i = 0; i < 38; i++) {
            cloudPattern[i] = cloudPattern[i + 1];
        }
        cloudPattern[38] = first;
    }

    if (allowMutation && random(0, 50) == 0) {
        int pos = random(0, 39);
        if (cloudPattern[pos] != ' ') {
            const char cloudChars[] = {'.', '-', '_'};
            cloudPattern[pos] = cloudChars[random(0, 3)];
        }
    }
}

// === WEATHER STATE CONTROL ===
// Determine which mood tier we're in (with hysteresis to prevent oscillation)
// Hysteresis: need to cross threshold by 5 points to change tier
static int getMoodTier(int mood, int currentTier) {
    // If no current tier, use raw thresholds
    if (currentTier < 0) {
        if (mood <= -40) return 2;      // SAD: high rain chance
        if (mood <= -20) return 1;      // MEH: low rain chance  
        return 0;                        // HAPPY/NEUTRAL: no rain
    }
    
    // Apply hysteresis based on direction of change
    switch (currentTier) {
        case 0:  // Currently HAPPY - need to drop below -25 to become MEH
            if (mood <= -25) return (mood <= -45) ? 2 : 1;
            return 0;
        case 1:  // Currently MEH - need -45 for SAD, -15 for HAPPY
            if (mood <= -45) return 2;
            if (mood > -15) return 0;
            return 1;
        case 2:  // Currently SAD - need to rise above -35 to become MEH
            if (mood > -35) return (mood > -15) ? 0 : 1;
            return 2;
    }
    return 0;
}

void setMoodLevel(int momentum) {
    currentMood = momentum;
    int newTier = getMoodTier(momentum, lastMoodTier);
    
    // Only re-roll rain when actually changing tiers (not every frame!)
    bool shouldReroll = !rainDecided || (newTier != lastMoodTier);
    
    if (shouldReroll) {
        lastMoodTier = newTier;
        rainDecided = true;
        
        bool shouldRain = false;
        
        if (newTier == 2) {
            // SAD/DEPRESSED: 70% rain chance (increased)
            shouldRain = (random(0, 100) < 70);
            // More frequent storms
            thunderMinInterval = 30000;  // 30-60s
            thunderMaxInterval = 60000;
        } else if (newTier == 1) {
            // MEH: 35% rain chance (increased from 20%)
            shouldRain = (random(0, 100) < 35);
            // Occasional storms
            thunderMinInterval = 60000;  // 60-120s
            thunderMaxInterval = 120000;
        } else {
            // Happy/neutral: no rain, clear the sky
            shouldRain = false;
            thunderMinInterval = 999999;  // Effectively disabled
            thunderMaxInterval = 999999;
        }
        
        setRaining(shouldRain);
    }
}

void setRaining(bool active) {
    if (active && !rainActive) {
        // Spawn raindrops staggered across entire screen height for immediate rain
        for (int i = 0; i < RAIN_DROP_COUNT; i++) {
            rainDrops[i].x = (float)random(0, 240);
            // Distribute drops across visible area (stop above grass at Y=88)
            rainDrops[i].y = (float)random(16, 85);
            // Fast rain (5-8 pixels per update)
            rainDrops[i].speed = random(5, 9);
        }
    } else if (!active && rainActive) {
        // Stop any in-flight thunder to avoid stuck flash on clear skies
        thunderFlashing = false;
        thunderFlashState = 0;
        thunderFlashesRemaining = 0;
        lastThunderStorm = millis();
    }
    rainActive = active;
} 

void triggerThunderStorm() {
    thunderFlashesRemaining = 3;
    lastThunderStorm = millis();
}

// Forward declarations for static update functions
static void updateClouds(uint32_t now);
static void updateRain(uint32_t now);
static void updateThunder(uint32_t now);
static void updateWind(uint32_t now);

// === ANIMATION UPDATES ===
void update() {
    uint32_t now = millis();
    
    // Update clouds (always)
    updateClouds(now);
    
    // Update rain (if active)
    if (rainActive) {
        updateRain(now);
    }
    
    // Update thunder (if raining)
    if (rainActive) {
        updateThunder(now);
    }
    
    // Update wind gusts (periodic)
    updateWind(now);
}

static void updateClouds(uint32_t now) {
    if (cloudMoving && now - lastCloudUpdate >= cloudSpeed) {
        lastCloudUpdate = now;
        shiftCloudPattern(cloudDirection, true);
    }

    // Parallax: when grass is moving, nudge clouds in the same direction (slower).
    if (Avatar::isGrassMoving()) {
        uint32_t parallaxInterval = (uint32_t)Avatar::getGrassSpeed() * CLOUD_PARALLAX_GRASS_SHIFTS;
        if (parallaxInterval < 150) parallaxInterval = 150;

        if (now - lastCloudParallax >= parallaxInterval) {
            lastCloudParallax = now;
            shiftCloudPattern(Avatar::isGrassDirectionRight(), false);
        }
    } else {
        lastCloudParallax = now;
    }
}

static void updateRain(uint32_t now) {
    if (now - lastRainUpdate < RAIN_SPEED_MS) return;
    lastRainUpdate = now;
    
    // Calculate horizontal drift based on grass movement (parallax effect)
    float horizontalDrift = 0.0f;
    if (Avatar::isGrassMoving()) {
        uint16_t grassSpeedMs = Avatar::getGrassSpeed();
        if (grassSpeedMs == 0) grassSpeedMs = 1;
        const float grassShiftPixels = 240.0f / 26.0f;  // screen width / grass pattern chars
        float grassPixelsPerMs = grassShiftPixels / (float)grassSpeedMs;
        float grassPixelsPerUpdate = grassPixelsPerMs * (float)RAIN_SPEED_MS;
        horizontalDrift = grassPixelsPerUpdate * 0.4f;  // 40% of grass speed
        if (Avatar::isGrassDirectionRight()) {
            horizontalDrift *= -1.0f;
        }
    }
    
    for (int i = 0; i < RAIN_DROP_COUNT; i++) {
        rainDrops[i].y += (float)rainDrops[i].speed;
        rainDrops[i].x += horizontalDrift;
        
        // Wrap horizontally if drifted off screen
        if (rainDrops[i].x < 0.0f) rainDrops[i].x += 240.0f;
        if (rainDrops[i].x >= 240.0f) rainDrops[i].x -= 240.0f;
        
        // Respawn just below clouds when reaching bottom
        // Grass starts at Y=91, stop rain 3px above it
        if (rainDrops[i].y >= 88.0f) {
            rainDrops[i].y = (float)random(16, 23);  // Just below cloud layer
            rainDrops[i].x = (float)random(0, 240);
            rainDrops[i].speed = random(5, 9);  // Fast rain
        }
    }
}

static void updateThunder(uint32_t now) {
    // Check if time for new storm
    if (!thunderFlashing && thunderFlashesRemaining == 0) {
        if (now - lastThunderStorm > thunderMinInterval) {
            uint32_t interval = random(thunderMinInterval, thunderMaxInterval);
            if (now - lastThunderStorm >= interval) {
                // Start new storm
                thunderFlashesRemaining = random(2, 4);  // 2-3 flashes
                lastThunderStorm = now;
            }
        }
    }
    
    // Execute flash sequence
    if (thunderFlashesRemaining > 0 && !thunderFlashing) {
        thunderFlashing = true;
        thunderFlashStart = now;
        thunderFlashState = 1;  // Flash ON
        thunderFlashesRemaining--;
    }
    
    if (thunderFlashing) {
        uint32_t elapsed = now - thunderFlashStart;
        
        // Faster flicker: shorter ON/OFF windows
        if (thunderFlashState == 1 && elapsed > random(30, 60)) {
            // Turn flash OFF
            thunderFlashState = 0;
            thunderFlashStart = now;
        } else if (thunderFlashState == 0 && elapsed > random(20, 40)) {
            // Flash complete
            thunderFlashing = false;
            thunderFlashState = 0;
        }
    }
}

static void updateWind(uint32_t now) {
    // Check for new wind gust
    if (!windActive && now - lastWindGust > windGustInterval) {
        // 30% chance of wind gust
        if (random(0, 100) < 30) {
            windActive = true;
            windGustDuration = random(2000, 4000);  // 2-4 second gust
            lastWindGust = now;
            
            // Spawn wind particles
            for (int i = 0; i < 6; i++) {
                windParticles[i].x = -10.0f - random(0, 50);  // Off-screen left
                windParticles[i].y = (float)random(20, 90);
                windParticles[i].speed = (float)random(3, 6);
                windParticles[i].active = true;
            }
        } else {
            // Reset interval for next check
            windGustInterval = random(15000, 30000);
            lastWindGust = now;
        }
    }
    
    // Update active wind particles
    if (windActive) {
        if (now - lastWindGust > windGustDuration) {
            // Gust finished
            windActive = false;
            windGustInterval = random(15000, 30000);
            for (int i = 0; i < 6; i++) {
                windParticles[i].active = false;
            }
        } else {
            // Animate particles
            if (now - lastWindUpdate > 50) {  // ~20fps
                lastWindUpdate = now;
                for (int i = 0; i < 6; i++) {
                    if (windParticles[i].active) {
                        windParticles[i].x += windParticles[i].speed;
                        // Add slight vertical wobble
                        windParticles[i].y += (random(0, 3) - 1) * 0.5f;
                        
                        // Deactivate when off-screen right
                        if (windParticles[i].x > 250.0f) {
                            windParticles[i].active = false;
                        }
                    }
                }
            }
        }
    }
}

// === THUNDER FLASH QUERY ===
bool isThunderFlashing() {
    return thunderFlashing && thunderFlashState == 1;
}

bool isRaining() {
    return rainActive;
}

// === DRAWING ===
void drawClouds(M5Canvas& canvas, uint16_t colorFG) {
    // During thunder flash, use inverted color (matches sirloin's getDrawColor)
    uint16_t drawColor = isThunderFlashing() ? getColorBG() : colorFG;
    
    canvas.setTextSize(2);
    canvas.setTextColor(drawColor);
    canvas.setTextDatum(top_left);
    
    // Draw in sky below top bar, above pig's head
    int cloudY = 2;  // Near top of main canvas
    canvas.drawString(cloudPattern, 0, cloudY);
}

void draw(M5Canvas& canvas, uint16_t colorFG, uint16_t colorBG) {
    // During thunder flash, invert colors for rain/wind (matches sirloin)
    uint16_t drawColor = isThunderFlashing() ? colorBG : colorFG;
    
    // Draw rain
    if (rainActive) {
        for (int i = 0; i < RAIN_DROP_COUNT; i++) {
            int x = (int)rainDrops[i].x;
            int y = (int)rainDrops[i].y;
            
            // Skip if above visible area (drops falling into view)
            if (y < 0) continue;
            
            // Draw 6-pixel tall × 2-pixel wide raindrop (slightly taller for visibility)
            for (int dy = 0; dy < 6; dy++) {
                if (y + dy < 88) {  // Clip 3px above grass (grass starts at Y=91)
                    canvas.drawPixel(x, y + dy, drawColor);
                    if (x + 1 < 240) canvas.drawPixel(x + 1, y + dy, drawColor);
                }
            }
        }
    }
    
    // Draw wind particles (ASCII dots)
    if (windActive) {
        canvas.setTextSize(2);
        canvas.setTextColor(drawColor);
        for (int i = 0; i < 6; i++) {
            if (windParticles[i].active) {
                int x = (int)windParticles[i].x;
                int y = (int)windParticles[i].y;
                if (x >= 0 && x < 240) {
                    // Draw as ASCII dot for consistency
                    canvas.drawChar('.', x, y);
                }
            }
        }
    }
}

}  // namespace Weather
