// Piglet ASCII avatar implementation

#include "avatar.h"
#include "../ui/display.h"

// Static members
AvatarState Avatar::currentState = AvatarState::NEUTRAL;
bool Avatar::isBlinking = false;
bool Avatar::earsUp = true;
uint32_t Avatar::lastBlinkTime = 0;
uint32_t Avatar::blinkInterval = 3000;

// Grass animation state
bool Avatar::grassMoving = false;
uint32_t Avatar::lastGrassUpdate = 0;
uint16_t Avatar::grassSpeed = 80;  // Default fast for OINK
char Avatar::grassPattern[32] = {0};

// Internal state for looking direction
static bool facingRight = false;
static uint32_t lastFlipTime = 0;
static uint32_t flipInterval = 5000;

// --- DERPY STYLE with direction ---
// Left facing frames (eye on left, snout 00 on right)
const char* AVATAR_NEUTRAL_L[] = {
    " ?  ? ",
    "(o 00)",
    "(    )"
};

const char* AVATAR_HAPPY_L[] = {
    " ^  ^ ",
    "(^ 00)",
    "(    )"
};

const char* AVATAR_EXCITED_L[] = {
    " !  ! ",
    "(@ 00)",
    "(    )"
};

const char* AVATAR_HUNTING_L[] = {
    " /  \\ ",
    "(> 00)",
    "(    )"
};

const char* AVATAR_SLEEPY_L[] = {
    " v  v ",
    "(- 00)",
    "(    )"
};

const char* AVATAR_SAD_L[] = {
    " .  . ",
    "(T 00)",
    "(    )"
};

const char* AVATAR_ANGRY_L[] = {
    " \\  / ",
    "(# 00)",
    "(    )"
};

const char* AVATAR_BLINK_L[] = {
    " ?  ? ",
    "(- 00)",
    "(    )"
};

// Right facing frames (snout 00 on left, eye on right)
const char* AVATAR_NEUTRAL_R[] = {
    " ?  ? ",
    "(00 o)",
    "(    )z"
};

const char* AVATAR_HAPPY_R[] = {
    " ^  ^ ",
    "(00 ^)",
    "(    )z"
};

const char* AVATAR_EXCITED_R[] = {
    " !  ! ",
    "(00 @)",
    "(    )z"
};

const char* AVATAR_HUNTING_R[] = {
    " /  \\ ",
    "(00 <)",
    "(    )z"
};

const char* AVATAR_SLEEPY_R[] = {
    " v  v ",
    "(00 -)",
    "(    )z"
};

const char* AVATAR_SAD_R[] = {
    " .  . ",
    "(00 T)",
    "(    )z"
};

const char* AVATAR_ANGRY_R[] = {
    " \\  / ",
    "(00 #)",
    "(    )z"
};

const char* AVATAR_BLINK_R[] = {
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
    
    // Init direction
    facingRight = false;
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

void Avatar::blink() {
    isBlinking = true;
}

void Avatar::wiggleEars() {
    earsUp = !earsUp;
}

void Avatar::draw(M5Canvas& canvas) {
    uint32_t now = millis();

    // Check if we should blink
    if (now - lastBlinkTime > blinkInterval) {
        isBlinking = true;
        lastBlinkTime = now;
        blinkInterval = random(4000, 8000);
    }

    // Check if we should flip direction (look around randomly)
    if (now - lastFlipTime > flipInterval) {
        facingRight = random(0, 2) == 1;  // Random direction
        lastFlipTime = now;
        flipInterval = random(5000, 15000);
    }
    
    // Select frame based on state and direction
    const char** frame;
    
    if (isBlinking && currentState != AvatarState::SLEEPY) {
        frame = facingRight ? AVATAR_BLINK_R : AVATAR_BLINK_L;
        isBlinking = false;
    } else {
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
    }
    
    drawFrame(canvas, frame, 3);
}

void Avatar::drawFrame(M5Canvas& canvas, const char** frame, uint8_t lines) {
    canvas.setTextDatum(top_left);
    canvas.setTextSize(3);
    canvas.setTextColor(COLOR_ACCENT);
    
    int startX = 2;
    int startY = 5;
    int lineHeight = 22;
    
    for (uint8_t i = 0; i < lines; i++) {
        canvas.drawString(frame[i], startX, startY + i * lineHeight);
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
    // Pig faces right = grass scrolls right (background moves past pig)
    // Pig faces left = grass scrolls left (background moves past pig)
    if (facingRight) {
        // Shift right
        char last = grassPattern[25];
        for (int i = 25; i > 0; i--) {
            grassPattern[i] = grassPattern[i - 1];
        }
        grassPattern[0] = last;
    } else {
        // Shift left
        char first = grassPattern[0];
        for (int i = 0; i < 25; i++) {
            grassPattern[i] = grassPattern[i + 1];
        }
        grassPattern[25] = first;
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
