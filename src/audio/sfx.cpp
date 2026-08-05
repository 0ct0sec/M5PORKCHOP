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
    {140, 10, 0},
    {0, 0, 0}
};
static const Note SND_TERM_TICK_E[] = {
    {300, 10, 2},
    {620, 3, 2},
    {160, 12, 0},
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

// DEAUTH: Low punch - impactful, visceral "kick"
static const Note SND_DEAUTH[] = {
    {400, 70, 0},
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
    {240, 50, 20},
    {180, 60, 0},
    {0, 0, 0}
};

// BOOT: Nostromo-style long boot sequence (2-3s)
static const Note SND_BOOT[] = {
    {140, 650, 140},  // low hum pulse
    {600, 12, 30},
    {700, 12, 30},
    {520, 12, 60},
    {120, 180, 80},  // tape thud
    {800, 12, 30},
    {640, 12, 30},
    {500, 12, 60},
    {900, 10, 30},
    {700, 10, 30},
    {850, 10, 60},
    {170, 230, 70},  // tape thud
    {210, 320, 90},
    {240, 360, 0},
    {0, 0, 0}
};

// PIGSYNC_BOOT: Shorter wake sequence for FA/TH/ER
static const Note SND_PIGSYNC_BOOT[] = {
    {160, 480, 140},
    {540, 12, 40},
    {660, 12, 40},
    {560, 12, 80},
    {120, 160, 70},  // tape thud
    {820, 10, 40},
    {700, 10, 60},
    {190, 210, 70},
    {220, 220, 70},
    {180, 240, 0},
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
    {43, 200, 20},   // Impact F1 (Sub-bass thud)
    {172, 80, 0},    // F3 wobble 1
    {178, 80, 0},    // F3 wobble 1
    {172, 80, 0},    // F3 wobble 2
    {178, 80, 0},    // F3 wobble 2
    {247, 60, 0},    // B3 (poison/dissonance)
    {172, 80, 0},    // F3 wobble 3
    {178, 80, 0},    // F3 wobble 3
    {311, 60, 0},    // Eb4 (metallic edge)
    {174, 400, 0},   // F3 sustain (The "Doom Tone")
    {87, 400, 0},    // F2 drop
    {43, 800, 0},    // F1 - tail (Sub-bass fade)
    {0, 0, 0}
};

// ==[ FLOCK / COUNTER-SURVEILLANCE PIG SOUNDS ]==
// Thematic replacements for the siren/beeps. Tones stay short so the queue
// keeps ticking non-blocking. Sequences authored for PORKCHOP (Jeb's spec).

// PIG_SQUEAL: piercing rising panic squeal (~0.7s). The 2300-2900 up/down
// wobble is the harsh quaver; the descending tail is the breath-out.
static const Note SND_PIG_SQUEAL[] = {
    {600, 30, 0}, {900, 30, 0}, {1300, 35, 0}, {1800, 40, 0}, {2300, 45, 0},
    {2600, 50, 0}, {2300, 30, 0}, {2700, 45, 0}, {2400, 30, 0}, {2800, 55, 0},
    {2500, 35, 0}, {2900, 70, 0}, {2400, 40, 0}, {1900, 45, 0}, {1400, 55, 0},
    {0, 0, 0}
};

// PIG_ALARM: the squeal twice with a breath-gap between - the "camera found"
// alarm. ~1.5s total.
static const Note SND_PIG_ALARM[] = {
    {600, 30, 0}, {900, 30, 0}, {1300, 35, 0}, {1800, 40, 0}, {2300, 45, 0},
    {2600, 50, 0}, {2300, 30, 0}, {2700, 45, 0}, {2400, 30, 0}, {2800, 55, 0},
    {2500, 35, 0}, {2900, 70, 0}, {2400, 40, 0}, {1900, 45, 0}, {1400, 55, 60},
    {600, 30, 0}, {900, 30, 0}, {1300, 35, 0}, {1800, 40, 0}, {2300, 45, 0},
    {2600, 50, 0}, {2300, 30, 0}, {2700, 45, 0}, {2400, 30, 0}, {2800, 55, 0},
    {2500, 35, 0}, {2900, 70, 0}, {2400, 40, 0}, {1900, 45, 0}, {1400, 55, 0},
    {0, 0, 0}
};

// PIG_GRUNT: low double-grunt, the proximity beep replacement (gentle zone).
static const Note SND_PIG_GRUNT[] = {
    {210, 70, 40}, {150, 95, 0},
    {0, 0, 0}
};

// PIG_RAVEN: low angry double-squeal (no top end) x2 - distinct from Flock.
static const Note SND_PIG_RAVEN[] = {
    {500, 40, 0}, {800, 40, 0}, {1200, 50, 0}, {1600, 60, 0},
    {1300, 40, 0}, {1700, 55, 0}, {1200, 50, 0}, {700, 60, 60},
    {500, 40, 0}, {800, 40, 0}, {1200, 50, 0}, {1600, 60, 0},
    {1300, 40, 0}, {1700, 55, 0}, {1200, 50, 0}, {700, 60, 0},
    {0, 0, 0}
};

// ==[ MORSE REMOVED ]==
// Morse GG was too long (600ms+), replaced with warm resolve in HANDSHAKE

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
    if (!Config::personality().soundEnabled) return;
    if (event == NONE) return;
    
    // Priority events (captures/celebrations) interrupt anything else
    bool isPriority = (event == PMKID || event == HANDSHAKE || event == ACHIEVEMENT ||
                       event == LEVEL_UP || event == JACKPOT_XP || event == ULTRA_STREAK ||
                       event == CHALLENGE_SWEEP ||
                       event == PIG_ALARM || event == PIG_RAVEN);  // camera/raven cut through
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
    
    // Start first note
    if (seq[0].freq > 0 && seq[0].duration > 0) {
        M5.Speaker.tone(seq[0].freq, seq[0].duration);
    }
}

bool update() {
    // Skip if sound disabled
    if (!Config::personality().soundEnabled) {
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
            case PIG_SQUEAL:
                startSequence(SND_PIG_SQUEAL);
                break;
            case PIG_ALARM:
                startSequence(SND_PIG_ALARM);
                break;
            case PIG_GRUNT:
                startSequence(SND_PIG_GRUNT);
                break;
            case PIG_RAVEN:
                startSequence(SND_PIG_RAVEN);
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
    if (!Config::personality().soundEnabled) return;
    M5.Speaker.tone(freq, duration);
}

}  // namespace SFX
