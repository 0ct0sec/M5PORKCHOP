// Piglet mood implementation

#include "mood.h"
#include "../core/config.h"
#include "../ui/display.h"

// Static members
String Mood::currentPhrase = "OINK!";
int Mood::happiness = 50;
uint32_t Mood::lastPhraseChange = 0;
uint32_t Mood::phraseInterval = 5000;
uint32_t Mood::lastActivityTime = 0;

// Phrase categories
const char* PHRASES_HAPPY[] = {
    "OINK OINK!",
    "Sniffin' packets!",
    "Got a good one!",
    "More handshakes!",
    "I'm a good piggy!",
    "Delicious data~",
    "OOOIINK!",
    "Truffle found!"
};

const char* PHRASES_EXCITED[] = {
    "JACKPOT!!!",
    "WPA2 YUMMY!",
    "HASHCAT FOOD!",
    "CAPTURE THIS!",
    "OMG OMG OMG!",
    "BACON BITS!!"
};

const char* PHRASES_HUNTING[] = {
    "Searching...",
    "Sniff sniff...",
    "Where's that AP?",
    "Patience piggy...",
    "Monitoring...",
    "Waiting..."
};

const char* PHRASES_SLEEPY[] = {
    "zzZzZ...",
    "*yawn*",
    "So quiet...",
    "Bored oink...",
    "Need WiFi...",
    "Sleepy piggy..."
};

const char* PHRASES_SAD[] = {
    "No networks...",
    "GPS lost...",
    "Lonely piggy...",
    "Need friends...",
    "Where is wifi?",
    "Sad oink..."
};

const char* PHRASES_IDLE[] = {
    "Ready to hunt!",
    "Press [O] OINK",
    "Press [W] WARHOG",
    "Waiting orders",
    "Porkchop ready!",
    "What's cooking?"
};

void Mood::init() {
    currentPhrase = "OINK!";
    happiness = 50;
    lastPhraseChange = millis();
    phraseInterval = 5000;
    lastActivityTime = millis();
}

void Mood::update() {
    uint32_t now = millis();
    
    // Check for inactivity
    uint32_t inactiveSeconds = (now - lastActivityTime) / 1000;
    if (inactiveSeconds > 60) {
        onNoActivity(inactiveSeconds);
    }
    
    // Natural happiness decay
    if (now - lastPhraseChange > phraseInterval) {
        happiness = constrain(happiness - 1, -100, 100);
        selectPhrase();
        lastPhraseChange = now;
    }
    
    updateAvatarState();
}

void Mood::onHandshakeCaptured(const char* apName) {
    happiness = min(happiness + 30, 100);
    lastActivityTime = millis();
    
    // Show AP name in phrase if available
    if (apName && strlen(apName) > 0) {
        String ap = String(apName);
        if (ap.length() > 12) ap = ap.substring(0, 12) + "..";
        const char* templates[] = {
            "Got %s!",
            "%s pwned!",
            "Yummy %s!",
            "%s captured!"
        };
        int idx = random(0, 4);
        char buf[48];
        snprintf(buf, sizeof(buf), templates[idx], ap.c_str());
        currentPhrase = buf;
    } else {
        int idx = random(0, sizeof(PHRASES_EXCITED) / sizeof(PHRASES_EXCITED[0]));
        currentPhrase = PHRASES_EXCITED[idx];
    }
    lastPhraseChange = millis();
    
    // Double beep for handshake! (user requested two beeps)
    M5.Speaker.tone(1500, 100);  // First beep
    delay(120);
    M5.Speaker.tone(2000, 100);  // Second beep (higher pitch)
}

void Mood::onNewNetwork(const char* apName) {
    happiness = min(happiness + 10, 100);
    lastActivityTime = millis();
    
    // Show AP name in phrase if available
    if (apName && strlen(apName) > 0) {
        String ap = String(apName);
        if (ap.length() > 12) ap = ap.substring(0, 12) + "..";
        const char* templates[] = {
            "Found %s!",
            "Sniffed %s",
            "Hello %s!",
            "New: %s"
        };
        int idx = random(0, 4);
        char buf[48];
        snprintf(buf, sizeof(buf), templates[idx], ap.c_str());
        currentPhrase = buf;
    } else {
        int idx = random(0, sizeof(PHRASES_HAPPY) / sizeof(PHRASES_HAPPY[0]));
        currentPhrase = PHRASES_HAPPY[idx];
    }
    lastPhraseChange = millis();
}

void Mood::onMLPrediction(float confidence) {
    lastActivityTime = millis();
    
    // High confidence = happy
    if (confidence > 0.8f) {
        happiness = min(happiness + 15, 100);
        int idx = random(0, sizeof(PHRASES_EXCITED) / sizeof(PHRASES_EXCITED[0]));
        currentPhrase = PHRASES_EXCITED[idx];
    } else if (confidence > 0.5f) {
        happiness = min(happiness + 5, 100);
        int idx = random(0, sizeof(PHRASES_HAPPY) / sizeof(PHRASES_HAPPY[0]));
        currentPhrase = PHRASES_HAPPY[idx];
    }
    
    lastPhraseChange = millis();
}

void Mood::onNoActivity(uint32_t seconds) {
    if (seconds > 300) {
        // Very bored after 5 minutes
        happiness = max(happiness - 2, -100);
        if (happiness < -20) {
            int idx = random(0, sizeof(PHRASES_SLEEPY) / sizeof(PHRASES_SLEEPY[0]));
            currentPhrase = PHRASES_SLEEPY[idx];
        }
    } else if (seconds > 120) {
        // Getting bored after 2 minutes
        happiness = max(happiness - 1, -100);
    }
}

void Mood::onWiFiLost() {
    happiness = max(happiness - 20, -100);
    lastActivityTime = millis();
    
    int idx = random(0, sizeof(PHRASES_SAD) / sizeof(PHRASES_SAD[0]));
    currentPhrase = PHRASES_SAD[idx];
    lastPhraseChange = millis();
}

void Mood::onGPSFix() {
    happiness = min(happiness + 10, 100);
    lastActivityTime = millis();
    currentPhrase = "GPS lock! Let's go!";
    lastPhraseChange = millis();
}

void Mood::onGPSLost() {
    happiness = max(happiness - 10, -100);
    currentPhrase = "Lost GPS...";
    lastPhraseChange = millis();
}

void Mood::onLowBattery() {
    currentPhrase = "Feed me power!";
    lastPhraseChange = millis();
}

void Mood::selectPhrase() {
    const char** phrases;
    int count;
    
    if (happiness > 70) {
        phrases = PHRASES_EXCITED;
        count = sizeof(PHRASES_EXCITED) / sizeof(PHRASES_EXCITED[0]);
    } else if (happiness > 30) {
        phrases = PHRASES_HAPPY;
        count = sizeof(PHRASES_HAPPY) / sizeof(PHRASES_HAPPY[0]);
    } else if (happiness > -10) {
        phrases = PHRASES_HUNTING;
        count = sizeof(PHRASES_HUNTING) / sizeof(PHRASES_HUNTING[0]);
    } else if (happiness > -50) {
        phrases = PHRASES_SLEEPY;
        count = sizeof(PHRASES_SLEEPY) / sizeof(PHRASES_SLEEPY[0]);
    } else {
        phrases = PHRASES_SAD;
        count = sizeof(PHRASES_SAD) / sizeof(PHRASES_SAD[0]);
    }
    
    int idx = random(0, count);
    currentPhrase = phrases[idx];
}

void Mood::updateAvatarState() {
    if (happiness > 70) {
        Avatar::setState(AvatarState::EXCITED);
    } else if (happiness > 30) {
        Avatar::setState(AvatarState::HAPPY);
    } else if (happiness > -10) {
        Avatar::setState(AvatarState::NEUTRAL);
    } else if (happiness > -50) {
        Avatar::setState(AvatarState::SLEEPY);
    } else {
        Avatar::setState(AvatarState::SAD);
    }
}

void Mood::draw(M5Canvas& canvas) {
    // Draw mood phrase below avatar
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    canvas.drawString(currentPhrase, DISPLAY_W / 2, MAIN_H - 15);
}

const String& Mood::getCurrentPhrase() {
    return currentPhrase;
}

int Mood::getCurrentHappiness() {
    return happiness;
}
