// Piglet ASCII avatar implementation

#include "avatar.h"
#include "../ui/display.h"

// Static members
AvatarState Avatar::currentState = AvatarState::NEUTRAL;
bool Avatar::isBlinking = false;
bool Avatar::earsUp = true;
uint32_t Avatar::lastBlinkTime = 0;
uint32_t Avatar::blinkInterval = 3000;
int Avatar::moodIntensity = 0;  // Phase 8: -100 to 100

// Walk transition state
bool Avatar::transitioning = false;
uint32_t Avatar::transitionStartTime = 0;
int Avatar::transitionFromX = 2;
int Avatar::transitionToX = 2;
bool Avatar::transitionToFacingRight = true;
int Avatar::currentX = 2;

// Sniff animation state
bool Avatar::isSniffing = false;
static uint32_t sniffStartTime = 0;
static const uint32_t SNIFF_DURATION_MS = 600;  // 600ms for proper sniff cycle
static uint8_t sniffFrame = 0;  // Alternates between nose shapes (oo, oO, Oo)

// Walk transition timing
static const uint32_t TRANSITION_DURATION_MS = 400;  // 400ms smooth walk animation (best practices §18)

// Attack shake state (visual feedback for captures)
static bool attackShakeActive = false;
static bool attackShakeStrong = false;
static uint32_t attackShakeRefreshTime = 0;

// Grass animation state
bool Avatar::grassMoving = false;
bool Avatar::grassDirection = true;  // true = grass scrolls right
bool Avatar::pendingGrassStart = false;  // Wait for transition before starting grass
uint32_t Avatar::lastGrassUpdate = 0;
uint16_t Avatar::grassSpeed = 80;  // Default fast for OINK
char Avatar::grassPattern[32] = {0};
// Internal state for looking direction
static bool facingRight = true;  // Default: pig looks right
static uint32_t lastFlipTime = 0;
static uint32_t flipInterval = 5000;

// Look behavior (stationary observation)
static uint32_t lastLookTime = 0;
static uint32_t lookInterval = 2000;  // Look around every 2-5s when stationary
bool Avatar::onRightSide = false;  // Track which side of screen pig is on (class static)

// --- DERPY STYLE with direction ---
// Right-looking frames (snout 00 on right side of face, pig looks RIGHT)
const char* AVATAR_NEUTRAL_R[] = {
    " ?  ? ",
    "(o 00)",
    "(    )"
};

const char* AVATAR_HAPPY_R[] = {
    " ^  ^ ",
    "(^ 00)",
    "(    )"
};

const char* AVATAR_EXCITED_R[] = {
    " !  ! ",
    "(@ 00)",
    "(    )"
};

const char* AVATAR_HUNTING_R[] = {
    " |  | ",
    "(= 00)",
    "(    )"
};

const char* AVATAR_SLEEPY_R[] = {
    " v  v ",
    "(- 00)",
    "(    )"
};

const char* AVATAR_SAD_R[] = {
    " .  . ",
    "(T 00)",
    "(    )"
};

const char* AVATAR_ANGRY_R[] = {
    " \\  / ",
    "(# 00)",
    "(    )"
};

// DEVIOUS: sneaky/scheming expression for HOGWASH mode (narrowed eyes, sly look)
const char* AVATAR_DEVIOUS_R[] = {
    " >  > ",
    "(~ 00)",
    "(    )"
};

// Left-looking frames (snout 00 on left side of face, pig looks LEFT, z pigtail)
const char* AVATAR_NEUTRAL_L[] = {
    " ?  ? ",
    "(00 o)",
    "(    )z"
};

const char* AVATAR_HAPPY_L[] = {
    " ^  ^ ",
    "(00 ^)",
    "(    )z"
};

const char* AVATAR_EXCITED_L[] = {
    " !  ! ",
    "(00 @)",
    "(    )z"
};

const char* AVATAR_HUNTING_L[] = {
    " |  | ",
    "(00 =)",
    "(    )z"
};

const char* AVATAR_SLEEPY_L[] = {
    " v  v ",
    "(00 -)",
    "(    )z"
};

const char* AVATAR_SAD_L[] = {
    " .  . ",
    "(00 T)",
    "(    )z"
};

const char* AVATAR_ANGRY_L[] = {
    " \\  / ",
    "(00 #)",
    "(    )z"
};

const char* AVATAR_DEVIOUS_L[] = {
    " <  < ",
    "(00 ~)",
    "(    )z"
};

void Avatar::init() {
    currentState = AvatarState::NEUTRAL;
    isBlinking = false;
    isSniffing = false;
    earsUp = true;
    lastBlinkTime = millis();
    blinkInterval = random(4000, 8000);
    
    // Init direction - default facing right (toward speech bubble)
    facingRight = true;
    onRightSide = false;  // Start on left side
    lastFlipTime = millis();
    flipInterval = random(10000, 30000);  // Walk timer: 10-30s
    lastLookTime = millis();
    lookInterval = random(8000, 20000);  // Look timer: 8-20s
    
    // Init grass pattern - full screen width at size 2 (~24 chars)
    grassMoving = false;
    grassDirection = true;
    pendingGrassStart = false;
    grassSpeed = 80;
    lastGrassUpdate = millis();
    for (int i = 0; i < 26; i++) {
        // Random grass pattern /\/\\//\/
        grassPattern[i] = (random(0, 2) == 0) ? '/' : '\\';
    }
    grassPattern[26] = '\0';
}

void Avatar::setState(AvatarState state) {
    currentState = state;
}

void Avatar::setMoodIntensity(int intensity) {
    moodIntensity = constrain(intensity, -100, 100);
}

bool Avatar::isFacingRight() {
    return facingRight;
}

bool Avatar::isOnRightSide() {
    return onRightSide;
}

bool Avatar::isTransitioning() {
    return transitioning;
}

int Avatar::getCurrentX() {
    return currentX;
}

void Avatar::blink() {
    isBlinking = true;
}

void Avatar::wiggleEars() {
    earsUp = !earsUp;
}

void Avatar::sniff() {
    if (!isSniffing) {
        sniffFrame = 0;  // Reset frame on new sniff
    }
    isSniffing = true;
    sniffStartTime = millis();
}

void Avatar::draw(M5Canvas& canvas) {
    uint32_t now = millis();
    
    // Sniff animation times out after SNIFF_DURATION_MS
    // Update sniff frame for animation (cycle every 100ms)
    if (isSniffing) {
        if (now - sniffStartTime > SNIFF_DURATION_MS) {
            isSniffing = false;
            sniffFrame = 0;
        } else {
            sniffFrame = ((now - sniffStartTime) / 100) % 3;  // 3 frames: oo, oO, Oo
        }
    }
    
    // Handle walk transition animation
    if (transitioning) {
        uint32_t elapsed = now - transitionStartTime;
        if (elapsed >= TRANSITION_DURATION_MS) {
            // Transition complete
            transitioning = false;
            currentX = transitionToX;
            facingRight = transitionToFacingRight;
            onRightSide = (currentX > 60);  // Track which side we're on
            // Start grass now if it was pending
            if (pendingGrassStart) {
                grassMoving = true;
                pendingGrassStart = false;
            }
            // Reset look timer for new position
            lastLookTime = now;
            lookInterval = random(2000, 5000);
        } else {
            // Animate X position (ease in-out)
            float t = (float)elapsed / TRANSITION_DURATION_MS;
            // Smooth step: 3t^2 - 2t^3
            float smoothT = t * t * (3.0f - 2.0f * t);
            currentX = transitionFromX + (int)((transitionToX - transitionFromX) * smoothT);
        }
    }

    // Phase 8: Mood intensity affects animation timing
    // High positive = excited (faster blinks, more looking around)
    // High negative = lethargic (slower blinks, less movement)
    
    // Calculate intensity-adjusted blink interval
    // Base: 4000-8000ms, excited (-50%): 2000-4000ms, sad (+50%): 6000-12000ms
    float blinkMod = 1.0f - (moodIntensity / 200.0f);  // 0.5 to 1.5
    uint32_t minBlink = (uint32_t)(4000 * blinkMod);
    uint32_t maxBlink = (uint32_t)(8000 * blinkMod);
    
    // Check if we should blink (single frame blink)
    if (now - lastBlinkTime > blinkInterval) {
        isBlinking = true;
        lastBlinkTime = now;
        blinkInterval = random(minBlink, maxBlink);
    }

    // Calculate intensity-adjusted intervals
    // Excited pig looks around more, sad pig stares
    float flipMod = 1.0f - (moodIntensity / 150.0f);  // ~0.33 to ~1.66
    uint32_t minWalk = (uint32_t)(15000 * flipMod);   // Walk timer: 15s base
    uint32_t maxWalk = (uint32_t)(40000 * flipMod);   // Walk timer: 40s base
    uint32_t minLook = (uint32_t)(8000 * flipMod);    // Look timer: 8s base (was 2s)
    uint32_t maxLook = (uint32_t)(20000 * flipMod);   // Look timer: 20s base (was 6s)
    
    // Stationary behavior: LOOK around (no X change) and occasionally WALK to new position
    // Disable all movement while grass is moving (treadmill mode)
    if (!transitioning && !grassMoving && !pendingGrassStart) {
        // LOOK timer - quick head turns while staying in place
        if (now - lastLookTime > lookInterval) {
            // 50% chance to look the other way
            if (random(0, 2) == 0) {
                facingRight = !facingRight;  // Instant flip, no transition
            }
            lastLookTime = now;
            lookInterval = random(minLook, maxLook);
        }
        
        // WALK timer - move to new position on screen
        if (now - lastFlipTime > flipInterval) {
            // Decide new position (opposite side)
            bool goToRightSide = !onRightSide;
            // Face direction of travel during walk
            bool faceDirectionDuringWalk = goToRightSide;  // Face right if going right
            
            // Start walk transition
            transitioning = true;
            transitionStartTime = now;
            transitionFromX = currentX;
            transitionToX = goToRightSide ? 130 : 2;  // Right side or left side
            transitionToFacingRight = faceDirectionDuringWalk;
            
            lastFlipTime = now;
            flipInterval = random(minWalk, maxWalk);
        }
    }
    
    // Select frame based on state and direction (blink modifies eye only, not ears)
    const char** frame;
    bool shouldBlink = isBlinking && currentState != AvatarState::SLEEPY;
    
    // Clear blink flag after reading (single frame blink)
    if (isBlinking) {
        isBlinking = false;
    }
    
    switch (currentState) {
        case AvatarState::HAPPY:    
            frame = facingRight ? AVATAR_HAPPY_R : AVATAR_HAPPY_L; break;
        case AvatarState::EXCITED:  
            frame = facingRight ? AVATAR_EXCITED_R : AVATAR_EXCITED_L; break;
        case AvatarState::HUNTING:  
            frame = facingRight ? AVATAR_HUNTING_R : AVATAR_HUNTING_L; break;
        case AvatarState::SLEEPY:   
            frame = facingRight ? AVATAR_SLEEPY_R : AVATAR_SLEEPY_L; break;
        case AvatarState::SAD:      
            frame = facingRight ? AVATAR_SAD_R : AVATAR_SAD_L; break;
        case AvatarState::ANGRY:    
            frame = facingRight ? AVATAR_ANGRY_R : AVATAR_ANGRY_L; break;
        case AvatarState::DEVIOUS:    
            frame = facingRight ? AVATAR_DEVIOUS_R : AVATAR_DEVIOUS_L; break;
        default:                    
            frame = facingRight ? AVATAR_NEUTRAL_R : AVATAR_NEUTRAL_L; break;
    }
    
    drawFrame(canvas, frame, 3, shouldBlink, facingRight, isSniffing);
}

void Avatar::drawFrame(M5Canvas& canvas, const char** frame, uint8_t lines, bool blink, bool faceRight, bool sniff) {
    canvas.setTextDatum(top_left);
    canvas.setTextSize(3);
    canvas.setTextColor(COLOR_ACCENT);
    
    uint32_t now = millis();
    
    // Watchdog: if caller stops refreshing attack shake, auto-disable after 250ms
    if (attackShakeRefreshTime == 0 || (now - attackShakeRefreshTime) > 250) {
        attackShakeActive = false;
        attackShakeStrong = false;
    }
    
    // Calculate vertical shake offset
    int shakeY = 0;
    if (attackShakeActive) {
        // Combat shake: random ±4px (normal) / ±6px (strong) - best practices §33
        const int amp = attackShakeStrong ? 6 : 4;
        shakeY = (esp_random() % 2 == 0) ? amp : -amp;
    } else if (transitioning || grassMoving) {
        // Walk bounce: 2px at 100ms intervals - best practices §32
        shakeY = ((now / 100) % 2 == 0) ? 2 : 0;
    }
    
    // Use animated currentX position (set during transition or at rest)
    int startX = currentX;
    int startY = 5 + shakeY;  // Apply shake offset
    int lineHeight = 22;
    
    for (uint8_t i = 0; i < lines; i++) {
        // Handle body line (i=2) for dynamic tail
        if (i == 2) {
            char bodyLine[16];
            bool tailOnLeft = false;  // Track if tail prefix added (needs X offset)
            if (grassMoving || pendingGrassStart) {
                // Treadmill mode: always show tail
                if (faceRight) {
                    strncpy(bodyLine, "z(    )", sizeof(bodyLine));  // Tail on left when facing right
                    tailOnLeft = true;
                } else {
                    strncpy(bodyLine, "(    )z", sizeof(bodyLine));  // Tail on right when facing left
                }
            } else if (transitioning) {
                // During transition: show tail on trailing side
                bool movingRight = (transitionToX > transitionFromX);
                if (movingRight) {
                    strncpy(bodyLine, "z(    )", sizeof(bodyLine));  // Tail trails on left
                    tailOnLeft = true;
                } else {
                    strncpy(bodyLine, "(    )z", sizeof(bodyLine));  // Tail trails on right
                }
            } else {
                // Stationary: show tail when facing AWAY from screen center
                // Right side + facing right = tail on left (facing away)
                // Right side + facing left = no tail (facing center)
                // Left side + facing left = tail on right (facing away)
                // Left side + facing right = no tail (facing center)
                bool facingAway = (onRightSide && faceRight) || (!onRightSide && !faceRight);
                if (facingAway) {
                    if (onRightSide) {
                        strncpy(bodyLine, "z(    )", sizeof(bodyLine));  // Right side, tail on left
                        tailOnLeft = true;
                    } else {
                        strncpy(bodyLine, "(    )z", sizeof(bodyLine));  // Left side, tail on right
                    }
                } else {
                    strncpy(bodyLine, "(    )", sizeof(bodyLine));  // Facing center, no tail
                }
            }
            bodyLine[sizeof(bodyLine) - 1] = '\0';
            // When tail is on left (z prefix), offset X back by 1 char width (18px at size 3)
            // to keep body aligned with head
            int bodyX = tailOnLeft ? (startX - 18) : startX;
            canvas.drawString(bodyLine, bodyX, startY + i * lineHeight);
        } else if (i == 1 && (blink || sniff)) {
            // Face line - modify eye and/or nose
            // Face format: "(X 00)" for right-facing, "(00 X)" for left-facing
            char modifiedLine[16];
            strncpy(modifiedLine, frame[i], sizeof(modifiedLine) - 1);
            modifiedLine[sizeof(modifiedLine) - 1] = '\0';
            
            if (blink) {
                // Replace eye character with '-' for blink
                if (faceRight) {
                    modifiedLine[1] = '-';  // Eye position in "(X 00)"
                } else {
                    modifiedLine[4] = '-';  // Eye position in "(00 X)"
                }
            }
            
            if (sniff) {
                // Animated sniff - cycle through nose shapes
                char n1, n2;
                switch (sniffFrame) {
                    case 0: n1 = 'o'; n2 = 'o'; break;  // oo
                    case 1: n1 = 'o'; n2 = 'O'; break;  // oO
                    case 2: n1 = 'O'; n2 = 'o'; break;  // Oo
                    default: n1 = 'o'; n2 = 'o'; break;
                }
                // Nose is at positions 3-4 for right-facing "(X 00)"
                // Nose is at positions 1-2 for left-facing "(00 X)"
                if (faceRight) {
                    modifiedLine[3] = n1;
                    modifiedLine[4] = n2;
                } else {
                    modifiedLine[1] = n1;
                    modifiedLine[2] = n2;
                }
            }
            
            canvas.drawString(modifiedLine, startX, startY + i * lineHeight);
        } else {
            canvas.drawString(frame[i], startX, startY + i * lineHeight);
        }
    }
    
    // Draw grass below piglet
    drawGrass(canvas);
}

void Avatar::setGrassMoving(bool moving, bool directionRight) {
    // Early exit if already in requested state (prevents per-frame overhead)
    if (moving && (grassMoving || pendingGrassStart)) {
        return;  // Already moving or pending - don't interrupt
    }
    if (!moving && !grassMoving && !pendingGrassStart) {
        return;  // Already stopped
    }
    
    if (moving) {
        grassDirection = directionRight;
        
        // Lock pig facing direction to match treadmill physics
        // grassDirection=true: grass scrolls right, pig walks left, face LEFT
        // grassDirection=false: grass scrolls left, pig walks right, face RIGHT
        facingRight = !directionRight;
        
        // Just start grass immediately - no transition blocking
        // The treadmill walk effect only triggers on MODE START, not per-frame sync
        // If pig is already transitioning, grass will start when transition ends
        if (transitioning) {
            pendingGrassStart = true;
            grassMoving = false;
        } else {
            grassMoving = true;
            pendingGrassStart = false;
        }
    } else {
        // Stop grass and coast back to resting position (X=2, facing left)
        grassMoving = false;
        pendingGrassStart = false;
        
        // Coast back to left edge resting position
        startWindupSlide(2, false);  // X=2, face left when done
    }
}

void Avatar::setGrassSpeed(uint16_t ms) {
    grassSpeed = ms;
}

void Avatar::setGrassPattern(const char* pattern) {
    strncpy(grassPattern, pattern, 26);
    grassPattern[26] = '\0';
}

void Avatar::resetGrassPattern() {
    // Reset to random grass pattern /\/\\//\/
    for (int i = 0; i < 26; i++) {
        grassPattern[i] = (random(0, 2) == 0) ? '/' : '\\';
    }
    grassPattern[26] = '\0';
}

void Avatar::updateGrass() {
    if (!grassMoving) return;
    
    uint32_t now = millis();
    if (now - lastGrassUpdate < grassSpeed) return;
    lastGrassUpdate = now;
    
    // Shift pattern based on grassDirection (set when grass started)
    // grassDirection=true: grass scrolls RIGHT (pig faces left, walking left through world)
    // grassDirection=false: grass scrolls LEFT (pig faces right, walking right through world)
    if (grassDirection) {
        // Shift right (grass scrolls right)
        char last = grassPattern[25];
        for (int i = 25; i > 0; i--) {
            grassPattern[i] = grassPattern[i - 1];
        }
        grassPattern[0] = last;
    } else {
        // Shift left (grass scrolls left)
        char first = grassPattern[0];
        for (int i = 0; i < 25; i++) {
            grassPattern[i] = grassPattern[i + 1];
        }
        grassPattern[25] = first;
    }
    
    // Occasionally mutate a character for variety
    if (random(0, 30) == 0) {
        int pos = random(0, 26);
        grassPattern[pos] = (random(0, 2) == 0) ? '/' : '\\';
    }
}

void Avatar::drawGrass(M5Canvas& canvas) {
    updateGrass();
    
    canvas.setTextSize(2);  // Same as menu items
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_left);
    
    // Draw at bottom of avatar area, full screen width
    int grassY = 73;  // Below the pig face
    canvas.drawString(grassPattern, 0, grassY);
}

// --- Phase 8: Direction control helpers ---
void Avatar::setFacingLeft() {
    facingRight = false;
}

void Avatar::setFacingRight() {
    facingRight = true;
}

// --- Phase 2: Attack shake control ---
void Avatar::setAttackShake(bool active, bool strong) {
    attackShakeActive = active;
    attackShakeStrong = strong;
    attackShakeRefreshTime = active ? millis() : 0;
}

// --- Phase 6: Windup slide for coast-back ---
void Avatar::startWindupSlide(int targetX, bool faceRight) {
    // Start a smooth transition to target position
    // Uses standard TRANSITION_DURATION_MS (300ms) from draw() logic
    if (currentX != targetX) {
        transitioning = true;
        transitionFromX = currentX;
        transitionToX = targetX;
        transitionStartTime = millis();
        transitionToFacingRight = faceRight;
    }
    // Set facing direction for when transition completes
    facingRight = faceRight;
}
