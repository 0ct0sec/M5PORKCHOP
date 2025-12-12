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
static const uint32_t SNIFF_DURATION_MS = 100;  // Hold sniff for 100ms

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
    isSniffing = true;
    sniffStartTime = millis();
}

void Avatar::draw(M5Canvas& canvas) {
    uint32_t now = millis();
    
    // Check if sniff animation should end
    if (isSniffing && (now - sniffStartTime > SNIFF_DURATION_MS)) {
        isSniffing = false;
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
        default:                    
            frame = facingRight ? AVATAR_NEUTRAL_R : AVATAR_NEUTRAL_L; break;
    }
    
    drawFrame(canvas, frame, 3, shouldBlink, facingRight, isSniffing);
}

void Avatar::drawFrame(M5Canvas& canvas, const char** frame, uint8_t lines, bool blink, bool faceRight, bool sniff) {
    canvas.setTextDatum(top_left);
    canvas.setTextSize(3);
    canvas.setTextColor(COLOR_ACCENT);
    
    // Use animated currentX position (set during transition or at rest)
    int startX = currentX;
    int startY = 5;
    int lineHeight = 22;
    
    for (uint8_t i = 0; i < lines; i++) {
        // Handle body line (i=2) for dynamic tail
        if (i == 2) {
            char bodyLine[16];
            bool tailOnLeft = false;  // Track if tail prefix added (needs X offset)
            if (transitioning) {
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
                // Replace nose 00 with oo for sniff
                // Nose is at positions 3-4 for right-facing "(X 00)"
                // Nose is at positions 1-2 for left-facing "(00 X)"
                if (faceRight) {
                    modifiedLine[3] = 'o';  // First 0 -> o
                    modifiedLine[4] = 'o';  // Second 0 -> o
                } else {
                    modifiedLine[1] = 'o';  // First 0 -> o
                    modifiedLine[2] = 'o';  // Second 0 -> o
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
    if (moving) {
        grassDirection = directionRight;
        
        // Pig should face OPPOSITE to grass direction (treadmill effect)
        // Grass moves right = pig faces left (on right side of screen)
        // Grass moves left = pig faces right (on left side of screen)
        bool targetFacing = !directionRight;
        
        if (facingRight != targetFacing || transitioning) {
            // Need to transition first
            pendingGrassStart = true;
            grassMoving = false;
            
            if (!transitioning) {
                // Start walk transition to correct position
                transitioning = true;
                transitionStartTime = millis();
                transitionFromX = currentX;
                transitionToX = targetFacing ? 2 : 130;  // Left side if facing right, right side if facing left
                transitionToFacingRight = targetFacing;
            }
        } else {
            // Already in correct position, start grass immediately
            grassMoving = true;
            pendingGrassStart = false;
        }
    } else {
        // Stop grass immediately
        grassMoving = false;
        pendingGrassStart = false;
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
