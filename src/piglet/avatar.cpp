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

// Sniff animation state
bool Avatar::isSniffing = false;
static uint32_t sniffStartTime = 0;
static const uint32_t SNIFF_DURATION_MS = 100;  // Hold sniff for 100ms

// Grass animation state
bool Avatar::grassMoving = false;
uint32_t Avatar::lastGrassUpdate = 0;
uint16_t Avatar::grassSpeed = 80;  // Default fast for OINK
char Avatar::grassPattern[32] = {0};
// Internal state for looking direction
static bool facingRight = true;  // Default: pig looks right
static uint32_t lastFlipTime = 0;
static uint32_t flipInterval = 5000;

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

const char* AVATAR_BLINK_R[] = {
    " ?  ? ",
    "(- 00)",
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

const char* AVATAR_BLINK_L[] = {
    " ?  ? ",
    "(00 -)",
    "(    )z"
};

void Avatar::init() {
    currentState = AvatarState::NEUTRAL;
    isBlinking = false;
    earsUp = true;
    lastBlinkTime = millis();
    blinkInterval = random(4000, 8000);
    
    // Init direction - default facing right (toward speech bubble)
    facingRight = true;
    lastFlipTime = millis();
    flipInterval = random(3000, 10000);
    
    // Init grass pattern - full screen width at size 2 (~24 chars)
    grassMoving = false;
    grassSpeed = 80;
    lastGrassUpdate = millis();
    for (int i = 0; i < 26; i++) {
        // 70% chance of '1', 30% chance of '0'
        grassPattern[i] = (random(0, 10) < 7) ? '1' : '0';
    }
    grassPattern[26] = '\0';
}

void Avatar::setState(AvatarState state) {
    currentState = state;
}

void Avatar::setMoodIntensity(int intensity) {
    moodIntensity = constrain(intensity, -100, 100);
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

    // Calculate intensity-adjusted flip interval
    // Excited pig looks around more, sad pig stares
    float flipMod = 1.0f - (moodIntensity / 150.0f);  // ~0.33 to ~1.66
    uint32_t minFlip = (uint32_t)(5000 * flipMod);
    uint32_t maxFlip = (uint32_t)(15000 * flipMod);
    
    // Check if we should flip direction (look around randomly)
    if (now - lastFlipTime > flipInterval) {
        facingRight = random(0, 2) == 1;  // Random direction
        lastFlipTime = now;
        flipInterval = random(minFlip, maxFlip);
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
    
    int startX = 2;
    int startY = 5;
    int lineHeight = 22;
    
    for (uint8_t i = 0; i < lines; i++) {
        if (i == 1 && (blink || sniff)) {
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

void Avatar::setGrassMoving(bool moving) {
    grassMoving = moving;
}

void Avatar::setGrassSpeed(uint16_t ms) {
    grassSpeed = ms;
}

void Avatar::setGrassPattern(const char* pattern) {
    strncpy(grassPattern, pattern, 26);
    grassPattern[26] = '\0';
}

void Avatar::resetGrassPattern() {
    // Reset to random binary pattern (like init)
    for (int i = 0; i < 26; i++) {
        grassPattern[i] = (random(0, 10) < 7) ? '1' : '0';
    }
    grassPattern[26] = '\0';
}

void Avatar::updateGrass() {
    if (!grassMoving) return;
    
    uint32_t now = millis();
    if (now - lastGrassUpdate < grassSpeed) return;
    lastGrassUpdate = now;
    
    // Shift pattern based on pig facing direction
    // Pig faces right = grass scrolls LEFT (ground moves under pig's feet)
    // Pig faces left = grass scrolls RIGHT (ground moves under pig's feet)
    if (facingRight) {
        // Shift left (pig walking right, ground moves left under feet)
        char first = grassPattern[0];
        for (int i = 0; i < 25; i++) {
            grassPattern[i] = grassPattern[i + 1];
        }
        grassPattern[25] = first;
    } else {
        // Shift right (pig walking left, ground moves right under feet)
        char last = grassPattern[25];
        for (int i = 25; i > 0; i--) {
            grassPattern[i] = grassPattern[i - 1];
        }
        grassPattern[0] = last;
    }
    
    // Occasionally mutate a character for variety
    if (random(0, 30) == 0) {
        int pos = random(0, 26);
        grassPattern[pos] = (random(0, 10) < 7) ? '1' : '0';
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
