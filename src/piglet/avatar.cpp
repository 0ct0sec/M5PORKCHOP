// Piglet ASCII avatar implementation

#include "avatar.h"
#include "../ui/display.h"

// Static members
AvatarState Avatar::currentState = AvatarState::NEUTRAL;
bool Avatar::isBlinking = false;
bool Avatar::earsUp = true;
uint32_t Avatar::lastBlinkTime = 0;
uint32_t Avatar::blinkInterval = 3000;

// Avatar ASCII frames (3 lines each, no legs, centered on 240px width)
const char* AVATAR_NEUTRAL[] = {
    "  ^  ^  ",
    " (o oo) ",
    "-(____)-"
};

const char* AVATAR_HAPPY[] = {
    "  ^  ^  ",
    " (^ o^) ",
    "-(____)-"
};

const char* AVATAR_EXCITED[] = {
    "  !  !  ",
    " (@o @) ",
    "<(____)>"
};

const char* AVATAR_HUNTING[] = {
    "  >  <  ",
    " (>o <) ",
    "\\(____)/"
};

const char* AVATAR_SLEEPY[] = {
    "  v  v  ",
    " (-o -) ",
    "-(____)-z"
};

const char* AVATAR_SAD[] = {
    "  v  v  ",
    " (T oT) ",
    "-(____)- "
};

const char* AVATAR_ANGRY[] = {
    "  \\  /  ",
    " (>o <) ",
    "#(__)#"
};

const char* AVATAR_BLINK[] = {
    "  ^  ^  ",
    " (- o-) ",
    "-(____)-"
};

void Avatar::init() {
    currentState = AvatarState::NEUTRAL;
    isBlinking = false;
    earsUp = true;
    lastBlinkTime = millis();
    blinkInterval = random(4000, 8000);  // Slower blink animation
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
    // Check if we should blink (slower animation)
    uint32_t now = millis();
    if (now - lastBlinkTime > blinkInterval) {
        isBlinking = true;
        lastBlinkTime = now;
        blinkInterval = random(4000, 8000);  // Slower blink
    }
    
    // Select frame based on state
    const char** frame;
    if (isBlinking && currentState != AvatarState::SLEEPY) {
        frame = AVATAR_BLINK;
        isBlinking = false;  // One-shot blink
    } else {
        switch (currentState) {
            case AvatarState::HAPPY:    frame = AVATAR_HAPPY; break;
            case AvatarState::EXCITED:  frame = AVATAR_EXCITED; break;
            case AvatarState::HUNTING:  frame = AVATAR_HUNTING; break;
            case AvatarState::SLEEPY:   frame = AVATAR_SLEEPY; break;
            case AvatarState::SAD:      frame = AVATAR_SAD; break;
            case AvatarState::ANGRY:    frame = AVATAR_ANGRY; break;
            default:                    frame = AVATAR_NEUTRAL; break;
        }
    }
    
    drawFrame(canvas, frame, 3);  // 3 lines now (no legs)
}

void Avatar::drawFrame(M5Canvas& canvas, const char** frame, uint8_t lines) {
    canvas.setTextDatum(top_center);
    canvas.setTextSize(3);  // Bigger font for avatar
    canvas.setTextColor(COLOR_ACCENT);
    
    int startY = 8;  // Start from top of main canvas
    int lineHeight = 24;  // Larger line height for bigger font
    
    for (uint8_t i = 0; i < lines; i++) {
        canvas.drawString(frame[i], DISPLAY_W / 2, startY + i * lineHeight);
    }
}
