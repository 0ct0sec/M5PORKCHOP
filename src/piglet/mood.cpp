// Piglet mood implementation

#include "mood.h"
#include "../core/config.h"
#include "../ui/display.h"

// Static members
String Mood::currentPhrase = "oink";
int Mood::happiness = 50;
uint32_t Mood::lastPhraseChange = 0;
uint32_t Mood::phraseInterval = 5000;
uint32_t Mood::lastActivityTime = 0;

// Phrase categories
const char* PHRASES_HAPPY[] = {
    "snout pwns all",
    "oink oink oink",
    "got that truffle",
    "packets nom nom",
    "hog on a roll",
    "mud life best life",
    "truffle shuffle",
    "chaos tastes good"
};

const char* PHRASES_EXCITED[] = {
    "OINK OINK OINK",
    "pwned em good",
    "truffle in the bag",
    "gg no re",
    "snout goes brrrr",
    "0day buffet"
};

const char* PHRASES_HUNTING[] = {
    "snout to ground",
    "sniff n drift",
    "hunting truffles",
    "curious piggy",
    "diggin deep",
    "where da truffles"
};

const char* PHRASES_SLEEPY[] = {
    "bored piggy",
    "null n void",
    "no truffles here",
    "/dev/null",
    "zzz oink zzz",
    "sleepy piggy"
};

const char* PHRASES_SAD[] = {
    "starving piggy",
    "404 no truffle",
    "lost n confused",
    "empty trough",
    "sad lil piggy",
    "need dem truffles"
};

const char* PHRASES_IDLE[] = {
    "oink?",
    "[O] hunt",
    "[W] roam",
    "[B] spam BLE",
    "piggy awaits",
    "hack the planet",
    "snout on standby"
};

// WARHOG wardriving phrases
const char* PHRASES_WARHOG[] = {
    "hog on patrol",
    "mobile n hostile",
    "snout mappin",
    "oink n log",
    "piggy on a roll",
    "wardrive n thrive",
    "gps locked",
    "loggin truffles",
    "wigle wiggle",
    "truffle coords",
    "roamin piggy",
    "mappin turf"
};

const char* PHRASES_WARHOG_FOUND[] = {
    "truffle logged",
    "stash it good",
    "oink logged",
    "coords yoinked",
    "for the herd",
    "another one",
    "bagged n tagged",
    "mine now lol"
};

// Piggy Blues BLE spam phrases
const char* PHRASES_PIGGYBLUES[] = {
    "FLOODING THE SPECTRUM",
    "BEACON STORM INBOUND",
    "SNIFFING AIRWAVES",
    "YOUR NOTIFICATIONS ARE MINE",
    "PACKETS AWAY",
    "OWNING THE 2.4GHZ",
    "SIGNAL HIJACK ACTIVE",
    "BLUEJACKING IN PROGRESS"
};

// Deauth success - short MAC format %02X%02X
const char* PHRASES_DEAUTH_SUCCESS[] = {
    "%s oinked out",
    "%s got rekt",
    "%s yeeted",
    "%s bye bye",
    "%s snout bonk",
    "%s evicted",
    "%s oink oink",
    "%s trampled",
    "%s skill issue",
    "%s squealed"
};

void Mood::init() {
    currentPhrase = "oink";
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
            "%s pwned",
            "%s gg ez",
            "rekt %s",
            "%s is mine"
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
    
    // Celebratory beep for handshake capture (higher pitch than deauth)
    if (Config::personality().soundEnabled) {
        M5.Speaker.tone(1500, 150);  // Distinctive handshake beep
    }
}

void Mood::onNewNetwork(const char* apName, int8_t rssi, uint8_t channel) {
    happiness = min(happiness + 10, 100);
    lastActivityTime = millis();
    
    // Show AP name with info in funny phrases
    if (apName && strlen(apName) > 0) {
        String ap = String(apName);
        if (ap.length() > 10) ap = ap.substring(0, 10) + "..";
        
        const char* templates[] = {
            "sniffed %s ch%d",
            "%s %ddb yum",
            "found %s oink",
            "oink %s",
            "new truffle %s"
        };
        int idx = random(0, 5);
        char buf[64];
        if (idx == 1 || idx == 3) {
            snprintf(buf, sizeof(buf), templates[idx], ap.c_str(), rssi);
        } else if (idx == 0 || idx == 2) {
            snprintf(buf, sizeof(buf), templates[idx], ap.c_str(), channel);
        } else {
            snprintf(buf, sizeof(buf), templates[idx], ap.c_str());
        }
        currentPhrase = buf;
    } else {
        // Hidden network
        char buf[48];
        snprintf(buf, sizeof(buf), "sneaky truffle CH%d %ddB", channel, rssi);
        currentPhrase = buf;
    }
    lastPhraseChange = millis();
}

void Mood::setStatusMessage(const String& msg) {
    currentPhrase = msg;
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
    // Rate-limit inactivity effects to prevent rapid phrase changes
    static uint32_t lastInactivityUpdate = 0;
    uint32_t now = millis();
    
    // Only update every 5 seconds to prevent burst phrase changes
    if (now - lastInactivityUpdate < 5000) {
        return;
    }
    lastInactivityUpdate = now;
    
    if (seconds > 300) {
        // Very bored after 5 minutes
        happiness = max(happiness - 2, -100);
        if (happiness < -20) {
            int idx = random(0, sizeof(PHRASES_SLEEPY) / sizeof(PHRASES_SLEEPY[0]));
            currentPhrase = PHRASES_SLEEPY[idx];
            lastPhraseChange = now;  // Prevent immediate re-selection
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
    currentPhrase = "gps locked n loaded";
    lastPhraseChange = millis();
}

void Mood::onGPSLost() {
    happiness = max(happiness - 10, -100);
    currentPhrase = "gps lost sad piggy";
    lastPhraseChange = millis();
}

void Mood::onLowBattery() {
    currentPhrase = "piggy needs juice";
    lastPhraseChange = millis();
}

void Mood::selectPhrase() {
    const char** phrases;
    int count;
    
    if (happiness > 70) {
        // High happiness but not from handshake - use HAPPY not EXCITED
        // EXCITED phrases reserved for actual handshake captures
        phrases = PHRASES_HAPPY;
        count = sizeof(PHRASES_HAPPY) / sizeof(PHRASES_HAPPY[0]);
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
    // Calculate bubble size based on message length
    String phrase = currentPhrase;
    phrase.toUpperCase();  // UPPERCASE for visibility
    int maxCharsPerLine = 14;  // Slightly less chars for padding
    int numLines = 1;
    if (phrase.length() > maxCharsPerLine) numLines = 2;
    if (phrase.length() > maxCharsPerLine * 2) numLines = 3;
    
    int bubbleX = 125;  // Moved left to prevent overflow
    int bubbleY = 3;
    int bubbleW = DISPLAY_W - bubbleX - 4;
    int bubbleH = 14 + (numLines * 14);  // Dynamic height based on lines
    
    // Cap bubble height to fit screen
    if (bubbleH > MAIN_H - 10) bubbleH = MAIN_H - 10;
    
    // Draw filled bubble with pink background
    canvas.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 6, COLOR_FG);
    
    // Draw < arrow pointing to piglet (filled triangle would be better but text works)
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    canvas.drawString("<", bubbleX - 6, bubbleY + bubbleH / 2 - 4);
    
    // Draw phrase inside bubble with word wrapping - BLACK text on pink
    canvas.setTextDatum(top_left);
    canvas.setTextColor(COLOR_BG);  // Black text
    
    int textX = bubbleX + 6;
    int textY = bubbleY + 6;
    int lineHeight = 12;
    
    // Word wrap logic
    String remaining = phrase;
    int lineNum = 0;
    while (remaining.length() > 0 && lineNum < 4) {
        String line;
        if (remaining.length() <= maxCharsPerLine) {
            line = remaining;
            remaining = "";
        } else {
            int splitPos = remaining.lastIndexOf(' ', maxCharsPerLine);
            if (splitPos <= 0) splitPos = maxCharsPerLine;
            line = remaining.substring(0, splitPos);
            remaining = remaining.substring(splitPos + 1);
        }
        canvas.drawString(line, textX, textY + lineNum * lineHeight);
        lineNum++;
    }
}

const String& Mood::getCurrentPhrase() {
    return currentPhrase;
}

int Mood::getCurrentHappiness() {
    return happiness;
}

// Sniffing phrases
const char* PHRASES_SNIFFING[] = {
    "sniff sniff",
    "pcap n nap",
    "parsing mud",
    "channel hoppin",
    "raw sniffin",
    "mon0 piggy",
    "dump n pump",
    "truffle hunt"
};

// Deauth/digging phrases
const char* PHRASES_DEAUTH[] = {
    "rootin at %s",
    "bonkin %s",
    "snout on %s",
    "oink at %s",
    "shakin %s tree",
    "oinkin at %s",
    "poke poke %s",
    "pwning %s"
};

// Idle phrases (non-misleading)
const char* PHRASES_MENU_IDLE[] = {
    "oink oink",
    "[O] truffle hunt",
    "[W] hog out",
    "piggy ready",
    "awaiting chaos",
    "pick ur poison",
    "do somethin",
    "hack or snack"
};

void Mood::onSniffing(uint16_t networkCount, uint8_t channel) {
    lastActivityTime = millis();
    
    // Randomly pick sniffing phrase with channel info
    int idx = random(0, sizeof(PHRASES_SNIFFING) / sizeof(PHRASES_SNIFFING[0]));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s CH%d (%d APs)", PHRASES_SNIFFING[idx], channel, networkCount);
    currentPhrase = buf;
    lastPhraseChange = millis();
}

void Mood::onDeauthing(const char* apName, uint32_t deauthCount) {
    lastActivityTime = millis();
    
    // Handle null or empty SSID (hidden networks)
    String ap = (apName && strlen(apName) > 0) ? String(apName) : "ghost AP";
    if (ap.length() > 10) ap = ap.substring(0, 10) + "..";
    
    int idx = random(0, sizeof(PHRASES_DEAUTH) / sizeof(PHRASES_DEAUTH[0]));
    char buf[64];
    snprintf(buf, sizeof(buf), PHRASES_DEAUTH[idx], ap.c_str());
    
    // Append deauth count every 5th update
    if (deauthCount % 50 == 0 && deauthCount > 0) {
        String msg = String(buf) + " [" + String(deauthCount) + "]";
        currentPhrase = msg;
    } else {
        currentPhrase = buf;
    }
    lastPhraseChange = millis();
}

void Mood::onDeauthSuccess(const uint8_t* clientMac) {
    lastActivityTime = millis();
    happiness = min(happiness + 15, 100);
    
    // Format short MAC (last 2 bytes only for brevity)
    char macStr[8];
    snprintf(macStr, sizeof(macStr), "%02X%02X", clientMac[4], clientMac[5]);
    
    int idx = random(0, sizeof(PHRASES_DEAUTH_SUCCESS) / sizeof(PHRASES_DEAUTH_SUCCESS[0]));
    char buf[48];
    snprintf(buf, sizeof(buf), PHRASES_DEAUTH_SUCCESS[idx], macStr);
    currentPhrase = buf;
    lastPhraseChange = millis();
    
    // Quick beep for confirmed kick
    if (Config::personality().soundEnabled) {
        M5.Speaker.tone(800, 50);
    }
}

void Mood::onIdle() {
    int idx = random(0, sizeof(PHRASES_MENU_IDLE) / sizeof(PHRASES_MENU_IDLE[0]));
    currentPhrase = PHRASES_MENU_IDLE[idx];
    lastPhraseChange = millis();
}

void Mood::onWarhogUpdate() {
    lastActivityTime = millis();
    int idx = random(0, sizeof(PHRASES_WARHOG) / sizeof(PHRASES_WARHOG[0]));
    currentPhrase = PHRASES_WARHOG[idx];
    lastPhraseChange = millis();
}

void Mood::onWarhogFound(const char* apName, uint8_t channel) {
    (void)apName;  // Currently unused, phrases don't include AP name
    (void)channel; // Currently unused
    
    lastActivityTime = millis();
    happiness = min(100, happiness + 5);
    
    int idx = random(0, sizeof(PHRASES_WARHOG_FOUND) / sizeof(PHRASES_WARHOG_FOUND[0]));
    currentPhrase = PHRASES_WARHOG_FOUND[idx];
    lastPhraseChange = millis();
}

void Mood::onPiggyBluesUpdate() {
    lastActivityTime = millis();
    happiness = min(100, happiness + 2);
    
    int idx = random(0, sizeof(PHRASES_PIGGYBLUES) / sizeof(PHRASES_PIGGYBLUES[0]));
    currentPhrase = PHRASES_PIGGYBLUES[idx];
    lastPhraseChange = millis();
}

