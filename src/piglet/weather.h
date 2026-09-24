// Weather effects module - clouds, rain, thunder, wind
// Mood-tied weather system
#pragma once

#include <M5Unified.h>

namespace Weather {

// === INITIALIZATION ===
void init();

// === WEATHER STATE CONTROL ===
// Call from Mood system to set weather based on momentum
void setMoodLevel(int momentum);  // -100 to 100, affects rain/storm probability

// Manual overrides (for testing or special events)
void setRaining(bool active);
void triggerThunderStorm();

// === ANIMATION UPDATES ===
// Call each frame to update weather effects
void update();

// === DRAWING ===
// Draw all weather layers (clouds, rain, wind particles)
// Call after Avatar::draw() to overlay effects
void draw(M5Canvas& canvas, uint16_t colorFG, uint16_t colorBG);

// Draw just clouds (parallax layer, call before avatar if desired)
void drawClouds(M5Canvas& canvas, uint16_t colorFG);

// Draw ambient pixel birds (call between Avatar::draw and drawClouds)
void drawBirds(M5Canvas& canvas, uint16_t colorFG);

// === THUNDER FLASH ===
// Query for thunder flash state (affects screen colors)
bool isThunderFlashing();
bool isRaining();

}  // namespace Weather
