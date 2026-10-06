/**
 * SFX - Non-blocking Sound Effects Module for Porkchop
 *
 * ==[ CHEF'S AUDIO ]== central beeps, no delay(), callback-safe enqueue.
 * 
 * CRITICAL: All sounds are non-blocking. Call SFX::update() from main loop.
 * Safe to call SFX::play() from anywhere including WiFi promiscuous callbacks.
 */

#ifndef SFX_H
#define SFX_H

#include <stdint.h>

namespace SFX {

// ==[ EVENTS ]== safe to call from anywhere
enum Event {
    NONE = 0,
    
    // === OINK MODE ===
    DEAUTH,             // deauth sent - low kick drum
    HANDSHAKE,          // complete handshake - victory arpeggio + morse GG
    PMKID,              // PMKID captured - quick double-tap
    NETWORK_NEW,        // new network found - soft tick
    
    // === SPECTRUM MODE ===
    CLIENT_FOUND,       // new client detected - high pip
    SIGNAL_LOST,        // signal lost - descending tones
    CHANNEL_LOCK,       // channel locked for monitoring
    REVEAL_START,       // client reveal mode started
    
    // === GAMIFICATION ===
    ACHIEVEMENT,        // achievement unlocked - fanfare
    LEVEL_UP,           // level up - ascending celebration
    JACKPOT_XP,         // 5x XP jackpot - rising arp
    ULTRA_STREAK,       // 20 capture streak - epic fanfare
    CHALLENGE_COMPLETE, // daily challenge done - rising tones
    CHALLENGE_SWEEP,    // all 3 challenges done - victory fanfare
    
    // === BLE SYNC ===
    CALL_RING,          // incoming call from Sirloin
    SYNC_COMPLETE,      // sync finished successfully
    
    // === SYSTEM ===
    ERROR,              // error buzz
    CLICK,              // UI click
    MENU_CLICK,         // menu navigation click
    TERMINAL_TICK,      // short terminal tick (boot variation)
    BOOT,               // device boot sequence
    PIGSYNC_BOOT,       // extended boot sequence for PIGSYNC
    
    // === SPECIAL ===
    SIREN,              // police siren effect (replaces flashSiren audio)
    YOU_DIED,           // Dark Souls style death sound

    // === UI FEEDBACK ===
    MODE_ENTER,         // mode transition in - quick ascending pair
    MODE_EXIT,          // mode transition out - quick descending pair
    CONFIRM,            // positive confirmation (settings saved)
    TYPING_KEY,         // ultra-short keystroke tick
    BACK_NAV,           // back/escape navigation

    // === PIG VOCALIZATIONS ===
    OINK_HAPPY,         // nasal descending ~300-150Hz
    OINK_GRUNT,         // low guttural burst ~120-100Hz
    OINK_SQUEAL,        // high ascending ~800-1200Hz
    OINK_CURIOUS,       // questioning upward ~400-600Hz

    // === AMBIENT SCANNING ===
    SONAR_PING,         // minimal single blip
    RADAR_SWEEP,        // subtle rising sweep
    SCAN_TICK,          // quiet periodic tick

    // === AMBIENT BIRDS ===
    BIRD_HIT,           // wave zaps bird - short electric zap
    BIRD_IMPACT         // bird hits ground - low thud + crackle
};

// Initialize audio system (call once at startup)
void init();

// Queue a sound event (callback-safe, ring buffer)
// Safe to call from WiFi promiscuous callback, BLE callback, anywhere
void play(Event event);

// Pump audio from main loop - MUST be called regularly (~every 10-50ms)
// Returns true if still playing
bool update();

// Is anything currently playing?
bool isPlaying();

// Stop current playback and clear queue
void stop();

// Direct tone access (for special cases)
void tone(uint16_t freq, uint16_t duration);

}  // namespace SFX

#endif  // SFX_H
