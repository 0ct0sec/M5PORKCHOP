/**
 * SFX - Non-blocking Sound Effects Implementation for Porkchop
 *
 * ==[ CHEF'S AUDIO ENGINE ]== 
 * - Note sequences: {freq, duration, pause} steps
 * - update() ticks without blocking
 * - Ring buffer for callback-safe event queuing
 * 
 * Adapted from Sirloin audio system.
 */

#include "sfx.h"
#include "../core/config.h"
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace SFX {

// ==[ SOUND DEFINITIONS ]== arrays of {freq, duration, pause}
// freq=0 means silence, duration=0 means END of sequence
//
// ==[ OPTION D: HYBRID PAPA PIG ]==
// Clean terminal sounds for frequent events, pig personality for celebrations.
// Professional but warm. The tool of a seasoned hacker pig.
//
struct Note {
    uint16_t freq;      // Hz (0 = silence)
    uint16_t duration;  // ms
    uint16_t pause;     // ms after this note
};

// CLICK: Soft mechanical switch tick
static const Note SND_CLICK[] = {
    {1050, 6, 0},
    {0, 0, 0}
};

// MENU_CLICK: Slightly lower, cushioned click
static const Note SND_MENU_CLICK[] = {
    {900, 7, 0},
    {0, 0, 0}
};

// TERMINAL_TICK: Mother-style hum pulse (deterministic round-robin)
static const Note SND_TERM_TICK_A[] = {
    {260, 12, 2},
    {540, 3, 0},
    {0, 0, 0}
};
static const Note SND_TERM_TICK_B[] = {
    {240, 13, 2},
    {500, 3, 0},
    {0, 0, 0}
};
static const Note SND_TERM_TICK_C[] = {
    {280, 11, 2},
    {600, 3, 0},
    {0, 0, 0}
};
static const Note SND_TERM_TICK_D[] = {
    {220, 14, 2},
    {460, 3, 2},
    {220, 10, 0},
    {0, 0, 0}
};
static const Note SND_TERM_TICK_E[] = {
    {300, 10, 2},
    {620, 3, 2},
    {250, 12, 0},
    {0, 0, 0}
};

// NETWORK_NEW: Short, quiet ping (fires often)
static const Note SND_NETWORK[] = {
    {820, 5, 0},
    {0, 0, 0}
};

// CLIENT_FOUND: Slightly brighter than network, still short
static const Note SND_CLIENT_FOUND[] = {
    {1000, 6, 0},
    {0, 0, 0}
};

// DEAUTH: TX pulse — soft descending pip (the pig pokes the airwaves)
// Short + warm interval (~perfect 4th) = pleasant at high repetition
static const Note SND_DEAUTH[] = {
    {700, 18, 0},     // bright onset
    {520, 14, 0},     // warm resolve (descending = energy going "out")
    {0, 0, 0}
};

// PMKID: "Truffle found" - pig's ears perk up, quick ascending pair
static const Note SND_PMKID[] = {
    {1000, 50, 15},
    {1300, 50, 0},
    {0, 0, 0}
};

// HANDSHAKE: "Got 'em" - complete phrase with warm resolution
// 800→1000→1200 then resolve back to 1000 (closure)
static const Note SND_HANDSHAKE[] = {
    {800, 60, 15},
    {1000, 60, 15},
    {1200, 80, 15},
    {1000, 100, 0},  // Resolve - the satisfying "done"
    {0, 0, 0}
};

// ACHIEVEMENT: "Papa proud" - warm, earned feeling
static const Note SND_ACHIEVEMENT[] = {
    {600, 80, 25},
    {900, 80, 25},
    {1200, 100, 0},
    {0, 0, 0}
};

// LEVEL_UP: "Oink of glory" - ascending major, proper celebration
static const Note SND_LEVEL_UP[] = {
    {500, 80, 20},
    {700, 80, 20},
    {1000, 80, 20},
    {1200, 120, 0},
    {0, 0, 0}
};

// JACKPOT_XP: Exciting but not annoying - quick rising phrase
static const Note SND_JACKPOT[] = {
    {700, 50, 15},
    {900, 50, 15},
    {1100, 50, 15},
    {1400, 100, 0},
    {0, 0, 0}
};

// ULTRA_STREAK: Big moment - extended celebration
static const Note SND_ULTRA_STREAK[] = {
    {500, 60, 15},
    {700, 60, 15},
    {900, 60, 15},
    {1100, 80, 20},
    {1400, 150, 0},
    {0, 0, 0}
};

// CALL_RING: Phone pip - attention getter
static const Note SND_RING[] = {
    {900, 80, 40},
    {1100, 80, 0},
    {0, 0, 0}
};

// SYNC_COMPLETE: Success - clean resolution
static const Note SND_SYNC_COMPLETE[] = {
    {800, 70, 20},
    {1000, 70, 20},
    {1200, 100, 0},
    {0, 0, 0}
};

// ERROR: Soft low double tap
static const Note SND_ERROR[] = {
    {330, 50, 20},
    {250, 60, 0},
    {0, 0, 0}
};

// BOOT: Nostromo-style long boot sequence (2-3s)
static const Note SND_BOOT[] = {
    {250, 650, 140},  // low hum pulse
    {600, 12, 30},
    {700, 12, 30},
    {520, 12, 60},
    {240, 180, 80},  // tape thud
    {800, 12, 30},
    {640, 12, 30},
    {500, 12, 60},
    {900, 10, 30},
    {700, 10, 30},
    {850, 10, 60},
    {280, 230, 70},  // tape thud
    {310, 320, 90},
    {360, 360, 0},
    {0, 0, 0}
};

// PIGSYNC_BOOT: Shorter wake sequence for FA/TH/ER
static const Note SND_PIGSYNC_BOOT[] = {
    {270, 480, 140},
    {540, 12, 40},
    {660, 12, 40},
    {560, 12, 80},
    {240, 160, 70},  // tape thud
    {820, 10, 40},
    {700, 10, 60},
    {310, 210, 70},
    {340, 220, 70},
    {280, 240, 0},
    {0, 0, 0}
};

// SIREN: Quick alternating for visual effect sync
static const Note SND_SIREN[] = {
    {500, 35, 0},
    {800, 35, 0},
    {500, 35, 0},
    {800, 35, 0},
    {0, 0, 0}
};

// ==[ SPECTRUM MODE SOUNDS ]==

// SIGNAL_LOST: "Gone" - sad descending
static const Note SND_SIGNAL_LOST[] = {
    {800, 80, 25},
    {500, 120, 0},
    {0, 0, 0}
};

// CHANNEL_LOCK: Quick confirmation tick
static const Note SND_CHANNEL_LOCK[] = {
    {900, 40, 0},
    {0, 0, 0}
};

// REVEAL_START: Ascending pair - "searching"
static const Note SND_REVEAL_START[] = {
    {700, 40, 15},
    {1000, 50, 0},
    {0, 0, 0}
};

// ==[ CHALLENGE SOUNDS ]==

// CHALLENGE_COMPLETE: "Nice work" - similar to achievement, lighter
static const Note SND_CHALLENGE_COMPLETE[] = {
    {700, 60, 20},
    {900, 60, 20},
    {1100, 80, 0},
    {0, 0, 0}
};

// CHALLENGE_SWEEP: "Legendary" - the big one with resolve
static const Note SND_CHALLENGE_SWEEP[] = {
    {800, 70, 20},
    {1000, 70, 20},
    {1200, 70, 20},
    {1500, 100, 15},
    {1200, 80, 0},  // Resolve down - closure
    {0, 0, 0}
};

// YOU_DIED: "Dark Souls" style death sound
// Impact (43Hz), then F3 wobble (172/178), with dissonant B3/Eb4, fading to sub
static const Note SND_YOU_DIED[] = {
    {220, 200, 20},  // Impact (was 43Hz sub-bass, raised to audible)
    {344, 80, 0},    // F4 wobble 1
    {356, 80, 0},    // F4 wobble 1
    {344, 80, 0},    // F4 wobble 2
    {356, 80, 0},    // F4 wobble 2
    {494, 60, 0},    // B4 (poison/dissonance)
    {344, 80, 0},    // F4 wobble 3
    {356, 80, 0},    // F4 wobble 3
    {622, 60, 0},    // Eb5 (metallic edge)
    {348, 400, 0},   // F4 sustain (The "Doom Tone")
    {260, 400, 0},   // Drop
    {220, 800, 0},   // Tail fade
    {0, 0, 0}
};

// ==[ UI FEEDBACK SOUNDS ]==

// MODE_ENTER: Quick ascending pair - entering new mode
static const Note SND_MODE_ENTER[] = {
    {700, 30, 10},
    {1000, 40, 0},
    {0, 0, 0}
};

// MODE_EXIT: Quick descending pair - leaving mode
static const Note SND_MODE_EXIT[] = {
    {900, 30, 10},
    {600, 40, 0},
    {0, 0, 0}
};

// CONFIRM: Warm double tap - settings saved, action confirmed
static const Note SND_CONFIRM[] = {
    {800, 40, 15},
    {1100, 50, 0},
    {0, 0, 0}
};

// TYPING_KEY: Ultra-short keystroke tick
static const Note SND_TYPING_KEY[] = {
    {1200, 4, 0},
    {0, 0, 0}
};

// BACK_NAV: Soft descending - going back
static const Note SND_BACK_NAV[] = {
    {800, 25, 0},
    {0, 0, 0}
};

// ==[ PIG VOCALIZATIONS ]==
// Stepped pitch descent creates nasal "oink" quality (bfxr-inspired)

// OINK_HAPPY: Satisfied pig snuffle — nasal wobble with breathy descent
static const Note SND_OINK_HAPPY[] = {
    {320, 30, 0},     // nasal onset
    {280, 25, 5},     // wobble down + breath gap
    {310, 20, 0},     // pitch instability (pigs aren't pitch-perfect)
    {240, 30, 0},     // settle lower
    {200, 25, 0},     // snuffle tail
    {0, 0, 0}
};

// OINK_GRUNT: Deep guttural burst — low as the piezo can handle
// Cardputer piezo rolls off hard below ~300Hz, so 300-450Hz is our "bass"
static const Note SND_OINK_GRUNT[] = {
    {420, 40, 0},     // guttural attack (low as audible)
    {340, 35, 0},     // drop into rumble
    {400, 20, 5},     // brief push up (vocal wobble)
    {300, 40, 0},     // settle into chest tone
    {0, 0, 0}
};

// OINK_SQUEAL: Alarmed pig — rapid ascending with vibrato
static const Note SND_OINK_SQUEAL[] = {
    {700, 25, 0},     // alarm onset
    {900, 25, 0},     // rapid rise
    {850, 20, 0},     // wobble back (vocal instability)
    {1100, 30, 0},    // peak alarm
    {1200, 35, 0},    // sustain high
    {0, 0, 0}
};

// OINK_CURIOUS: Questioning sniff — upward with micro-pauses (sniffing air)
static const Note SND_OINK_CURIOUS[] = {
    {350, 25, 12},    // sniff
    {380, 20, 10},    // sniff (slightly higher — interest building)
    {500, 35, 5},     // question rise
    {580, 40, 0},     // hold the question (rising intonation = curiosity)
    {0, 0, 0}
};

// ==[ AMBIENT SCANNING SOUNDS ]==

// SONAR_PING: Minimal single blip - periodic scan feedback
static const Note SND_SONAR_PING[] = {
    {1000, 20, 0},
    {0, 0, 0}
};

// RADAR_SWEEP: Subtle rising sweep - longer scan feedback
static const Note SND_RADAR_SWEEP[] = {
    {280, 30, 0},
    {350, 30, 0},
    {500, 30, 0},
    {700, 40, 0},
    {0, 0, 0}
};

// SCAN_TICK: Quiet periodic tick - background scanning
static const Note SND_SCAN_TICK[] = {
    {600, 8, 0},
    {0, 0, 0}
};

// ==[ AMBIENT BIRD SOUNDS ]==

// BIRD_HIT: Short electric zap - wave hits bird
static const Note SND_BIRD_HIT[] = {
    {1400, 25, 0},
    {900, 35, 0},
    {600, 20, 0},
    {0, 0, 0}
};

// BIRD_IMPACT: Low thud + crackle - bird hits ground
static const Note SND_BIRD_IMPACT[] = {
    {220, 60, 0},
    {350, 25, 10},
    {280, 20, 0},
    {0, 0, 0}
};

// ==[ MORSE REMOVED ]==
// Morse GG was too long (600ms+), replaced with warm resolve in HANDSHAKE

// ==[ PER-SOUND VOLUME SCALING ]==
// Frequent/ambient sounds play quieter than celebrations.
// Schultz (1997): predicted events → subdued feedback; rare events → full punch.
static uint8_t currentVolumeScale = 100;  // 0-100%, set before each sound

static uint8_t eventVolumeScale(Event e) {
    switch (e) {
        // AMBIENT (35%) — below conscious attention threshold
        case BIRD_HIT:
        case BIRD_IMPACT:
        case SCAN_TICK:
        case SONAR_PING:
            return 35;
        // FREQUENT (50%) — acknowledged but not alarming
        case TERMINAL_TICK:
        case NETWORK_NEW:
        case CLIENT_FOUND:
        case RADAR_SWEEP:
        case CLICK:
        case DEAUTH:
            return 50;
        // FULL (100%) — celebrations, captures, UI, pig voices
        default:
            return 100;
    }
}

// ==[ VOLUME MAPPING ]==
static void applyVolume() {
    // Cardputer piezo is LOUD — keep the whole curve low.
    // Level 1 = whisper (stealth), 3 = comfortable, 5 = noisy room.
    static const uint8_t kVolMap[] = {0, 20, 45, 80, 140, 210};
    uint8_t lvl = Config::personality().soundLevel;
    if (lvl > 5) lvl = 5;
    uint16_t vol = kVolMap[lvl];
    vol = (vol * currentVolumeScale) / 100;
    M5.Speaker.setVolume((uint8_t)vol);
}

// ==[ STATE MACHINE ]==
static const Note* currentSequence = nullptr;
static uint8_t currentStep = 0;
static uint32_t stepStartTime = 0;
static bool inNote = false;  // true = playing tone, false = in pause

// ==[ EVENT RING BUFFER ]== prevents event loss under rapid fire
static constexpr uint8_t QUEUE_SIZE = 4;
static Event eventQueue[QUEUE_SIZE];
static volatile uint8_t queueHead = 0;  // next write position
static volatile uint8_t queueTail = 0;  // next read position
static portMUX_TYPE queueMutex = portMUX_INITIALIZER_UNLOCKED;

// ==[ IMPLEMENTATION ]==

void init() {
    currentSequence = nullptr;
    currentStep = 0;
    queueHead = 0;
    queueTail = 0;
    
    // Initialize queue
    for (int i = 0; i < QUEUE_SIZE; i++) {
        eventQueue[i] = NONE;
    }
}

void play(Event event) {
    if (Config::personality().soundLevel == 0) return;
    if (event == NONE) return;
    
    // Priority events (captures/celebrations) interrupt anything else
    bool isPriority = (event == PMKID || event == HANDSHAKE || event == ACHIEVEMENT || 
                       event == LEVEL_UP || event == JACKPOT_XP || event == ULTRA_STREAK ||
                       event == CHALLENGE_SWEEP);
    if (isPriority && currentSequence != nullptr) {
        // Interrupt current sound for priority feedback
        M5.Speaker.stop();
        delayMicroseconds(100);  // Brief settle time for audio driver stability
        currentSequence = nullptr;
        currentStep = 0;
        // Clear queue on priority
        taskENTER_CRITICAL(&queueMutex);
        queueHead = queueTail = 0;
        taskEXIT_CRITICAL(&queueMutex);
    }
    
    // Enqueue event (ring buffer - drops oldest if full)
    taskENTER_CRITICAL(&queueMutex);
    uint8_t nextHead = (queueHead + 1) % QUEUE_SIZE;
    if (nextHead == queueTail) {
        // Buffer full - advance tail (drop oldest)
        queueTail = (queueTail + 1) % QUEUE_SIZE;
    }
    eventQueue[queueHead] = event;
    queueHead = nextHead;
    taskEXIT_CRITICAL(&queueMutex);
}

static void startSequence(const Note* seq) {
    currentSequence = seq;
    currentStep = 0;
    stepStartTime = millis();
    inNote = true;

    // Apply current volume before starting playback
    applyVolume();

    // Start first note
    if (seq[0].freq > 0 && seq[0].duration > 0) {
        M5.Speaker.tone(seq[0].freq, seq[0].duration);
    }
}

bool update() {
    // Skip if sound disabled
    if (Config::personality().soundLevel == 0) {
        // Clear any queued events
        taskENTER_CRITICAL(&queueMutex);
        queueHead = queueTail;
        taskEXIT_CRITICAL(&queueMutex);
        currentSequence = nullptr;
        return false;
    }
    
    // Process queued event if nothing playing
    taskENTER_CRITICAL(&queueMutex);
    bool hasEvents = (queueTail != queueHead && currentSequence == nullptr);
    Event e = NONE;
    if (hasEvents) {
        e = eventQueue[queueTail];
        queueTail = (queueTail + 1) % QUEUE_SIZE;
    }
    taskEXIT_CRITICAL(&queueMutex);
    
    if (hasEvents) {
        currentVolumeScale = eventVolumeScale(e);
        switch (e) {
            case DEAUTH:
                startSequence(SND_DEAUTH);
                break;
            case HANDSHAKE:
                startSequence(SND_HANDSHAKE);
                break;
            case PMKID:
                startSequence(SND_PMKID);
                break;
            case NETWORK_NEW:
                startSequence(SND_NETWORK);
                break;
            case ACHIEVEMENT:
                startSequence(SND_ACHIEVEMENT);
                break;
            case LEVEL_UP:
                startSequence(SND_LEVEL_UP);
                break;
            case JACKPOT_XP:
                startSequence(SND_JACKPOT);
                break;
            case ULTRA_STREAK:
                startSequence(SND_ULTRA_STREAK);
                break;
            case CALL_RING:
                startSequence(SND_RING);
                break;
            case SYNC_COMPLETE:
                startSequence(SND_SYNC_COMPLETE);
                break;
            case ERROR:
                startSequence(SND_ERROR);
                break;
            case CLICK:
                startSequence(SND_CLICK);
                break;
            case MENU_CLICK:
                startSequence(SND_MENU_CLICK);
                break;
            case TERMINAL_TICK:
                {
                    static uint8_t termTickIndex = 0;
                    const Note* seq = SND_TERM_TICK_A;
                    switch (termTickIndex % 5) {
                        case 1: seq = SND_TERM_TICK_B; break;
                        case 2: seq = SND_TERM_TICK_C; break;
                        case 3: seq = SND_TERM_TICK_D; break;
                        case 4: seq = SND_TERM_TICK_E; break;
                        default: break;
                    }
                    termTickIndex++;
                    startSequence(seq);
                }
                break;
            case BOOT:
                startSequence(SND_BOOT);
                break;
            case PIGSYNC_BOOT:
                startSequence(SND_PIGSYNC_BOOT);
                break;
            case SIREN:
                startSequence(SND_SIREN);
                break;
            case CLIENT_FOUND:
                startSequence(SND_CLIENT_FOUND);
                break;
            case SIGNAL_LOST:
                startSequence(SND_SIGNAL_LOST);
                break;
            case CHANNEL_LOCK:
                startSequence(SND_CHANNEL_LOCK);
                break;
            case REVEAL_START:
                startSequence(SND_REVEAL_START);
                break;
            case CHALLENGE_COMPLETE:
                startSequence(SND_CHALLENGE_COMPLETE);
                break;
            case CHALLENGE_SWEEP:
                startSequence(SND_CHALLENGE_SWEEP);
                break;
            case YOU_DIED:
                startSequence(SND_YOU_DIED);
                break;
            // UI feedback
            case MODE_ENTER:
                startSequence(SND_MODE_ENTER);
                break;
            case MODE_EXIT:
                startSequence(SND_MODE_EXIT);
                break;
            case CONFIRM:
                startSequence(SND_CONFIRM);
                break;
            case TYPING_KEY:
                startSequence(SND_TYPING_KEY);
                break;
            case BACK_NAV:
                startSequence(SND_BACK_NAV);
                break;
            // Pig vocalizations
            case OINK_HAPPY:
                startSequence(SND_OINK_HAPPY);
                break;
            case OINK_GRUNT:
                startSequence(SND_OINK_GRUNT);
                break;
            case OINK_SQUEAL:
                startSequence(SND_OINK_SQUEAL);
                break;
            case OINK_CURIOUS:
                startSequence(SND_OINK_CURIOUS);
                break;
            // Ambient scanning
            case SONAR_PING:
                startSequence(SND_SONAR_PING);
                break;
            case RADAR_SWEEP:
                startSequence(SND_RADAR_SWEEP);
                break;
            case SCAN_TICK:
                startSequence(SND_SCAN_TICK);
                break;
            // Ambient birds
            case BIRD_HIT:
                startSequence(SND_BIRD_HIT);
                break;
            case BIRD_IMPACT:
                startSequence(SND_BIRD_IMPACT);
                break;
            default:
                break;
        }
    }
    
    // Process current sequence
    if (currentSequence == nullptr) {
        taskENTER_CRITICAL(&queueMutex);
        bool eventsWaiting = (queueTail != queueHead);
        taskEXIT_CRITICAL(&queueMutex);
        return eventsWaiting;  // More events waiting?
    }
    
    uint32_t now = millis();
    const Note& note = currentSequence[currentStep];
    
    // Check if sequence ended (duration=0 marks end)
    if (note.duration == 0) {
        currentSequence = nullptr;
        currentStep = 0;
        taskENTER_CRITICAL(&queueMutex);
        bool eventsWaiting = (queueTail != queueHead);
        taskEXIT_CRITICAL(&queueMutex);
        return eventsWaiting;
    }
    
    if (inNote) {
        // In note phase - wait for duration
        if (now - stepStartTime >= note.duration) {
            // Note finished, enter pause phase
            inNote = false;
            stepStartTime = now;
            
            // If no pause, advance immediately
            if (note.pause == 0) {
                currentStep++;
                inNote = true;
                stepStartTime = now;
                
                const Note& next = currentSequence[currentStep];
                if (next.duration > 0 && next.freq > 0) {
                    M5.Speaker.tone(next.freq, next.duration);
                }
            }
        }
    } else {
        // In pause phase - wait for pause duration
        if (now - stepStartTime >= note.pause) {
            // Pause finished, advance to next note
            currentStep++;
            inNote = true;
            stepStartTime = now;
            
            const Note& next = currentSequence[currentStep];
            if (next.duration > 0 && next.freq > 0) {
                M5.Speaker.tone(next.freq, next.duration);
            }
        }
    }
    
    return true;
}

bool isPlaying() {
    taskENTER_CRITICAL(&queueMutex);
    bool playing = currentSequence != nullptr || (queueTail != queueHead);
    taskEXIT_CRITICAL(&queueMutex);
    return playing;
}

void stop() {
    currentSequence = nullptr;
    currentStep = 0;
    taskENTER_CRITICAL(&queueMutex);
    queueHead = queueTail = 0;
    taskEXIT_CRITICAL(&queueMutex);
    M5.Speaker.stop();
}

void tone(uint16_t freq, uint16_t duration) {
    if (Config::personality().soundLevel == 0) return;
    applyVolume();
    M5.Speaker.tone(freq, duration);
}

}  // namespace SFX
