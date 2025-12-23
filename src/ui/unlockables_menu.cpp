// Unlockables Menu - Secret challenges for the worthy

#include "unlockables_menu.h"
#include <M5Cardputer.h>
#include <mbedtls/sha256.h>
#include "display.h"
#include "../core/xp.h"
#include "../piglet/mood.h"

// Static member initialization
uint8_t UnlockablesMenu::selectedIndex = 0;
uint8_t UnlockablesMenu::scrollOffset = 0;
bool UnlockablesMenu::active = false;
bool UnlockablesMenu::keyWasPressed = false;
bool UnlockablesMenu::exitRequested = false;
bool UnlockablesMenu::textEditing = false;
String UnlockablesMenu::textBuffer = "";

// The unlockables - add more here as lore expands
// Hash generated: SHA256(phrase) in lowercase hex
static const UnlockableItem UNLOCKABLES[] = {
    // Bit 0: PROPHECY - phrase is "dtr rts zero" (from riddle's final line)
    { 
        "PROPHECY", 
        "THE PROPHECY SPEAKS THE KEY",
        "13ca9c448763034b2d1b1ec33b449ae90433634c16b50a0a9fba6f4b5a67a72a",
        0 
    },
    // Future unlockables can be added here
    // { "NEXT_SECRET", "HINT TEXT", "sha256hash...", 1 },
};

static const uint8_t TOTAL_UNLOCKABLES = sizeof(UNLOCKABLES) / sizeof(UNLOCKABLES[0]);

void UnlockablesMenu::init() {
    selectedIndex = 0;
    scrollOffset = 0;
    textEditing = false;
    textBuffer = "";
}

void UnlockablesMenu::show() {
    active = true;
    exitRequested = false;
    selectedIndex = 0;
    scrollOffset = 0;
    textEditing = false;
    textBuffer = "";
    keyWasPressed = true;  // Ignore the Enter that selected us from menu
    updateBottomOverlay();
}

void UnlockablesMenu::hide() {
    active = false;
    textEditing = false;
    textBuffer = "";
    Display::clearBottomOverlay();
}

void UnlockablesMenu::update() {
    if (!active) return;
    handleInput();
}

bool UnlockablesMenu::validatePhrase(const char* phrase, const char* expectedHash) {
    // Compute SHA256 of input phrase
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA256, 1 = SHA224
    mbedtls_sha256_update(&ctx, (const uint8_t*)phrase, strlen(phrase));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    // Convert hash to hex string
    char hexHash[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&hexHash[i * 2], "%02x", hash[i]);
    }
    hexHash[64] = '\0';
    
    // Compare with expected
    return strcmp(hexHash, expectedHash) == 0;
}

void UnlockablesMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    // Handle text input mode separately
    if (textEditing) {
        handleTextInput();
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    // Navigation with ; (up) and . (down)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selectedIndex > 0 && TOTAL_UNLOCKABLES > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
            updateBottomOverlay();
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (TOTAL_UNLOCKABLES > 0 && selectedIndex < TOTAL_UNLOCKABLES - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
            updateBottomOverlay();
        }
    }
    
    // Enter to attempt unlock
    if (keys.enter && TOTAL_UNLOCKABLES > 0 && selectedIndex < TOTAL_UNLOCKABLES) {
        // Check if already unlocked
        if (XP::hasUnlockable(UNLOCKABLES[selectedIndex].bitIndex)) {
            Display::showToast("ALREADY YOURS");
            delay(500);
        } else {
            // Enter text input mode
            textEditing = true;
            textBuffer = "";
            keyWasPressed = true;
        }
    }
    
    // Exit with backtick
    if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        exitRequested = true;
        hide();
    }
}

void UnlockablesMenu::handleTextInput() {
    auto keys = M5Cardputer.Keyboard.keysState();
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    bool hasPrintableChar = !keys.word.empty();
    bool hasActionKey = keys.enter || keys.del;
    
    if (!hasPrintableChar && !hasActionKey) {
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    // Enter to submit
    if (keys.enter) {
        // Safety check
        if (TOTAL_UNLOCKABLES == 0 || selectedIndex >= TOTAL_UNLOCKABLES) {
            textEditing = false;
            textBuffer = "";
            return;
        }
        
        // Convert to lowercase for comparison
        String phrase = textBuffer;
        phrase.toLowerCase();
        
        // Validate against hash
        if (validatePhrase(phrase.c_str(), UNLOCKABLES[selectedIndex].hashHex)) {
            // SUCCESS!
            XP::setUnlockable(UNLOCKABLES[selectedIndex].bitIndex);
            Display::showToast("UNLOCKED");
            Display::flashSiren(3);
            Mood::adjustHappiness(30);  // Happy pig
        } else {
            // WRONG
            Display::showToast("WRONG");
            Mood::adjustHappiness(-20);  // Sad pig
        }
        delay(500);
        
        textEditing = false;
        textBuffer = "";
        return;
    }
    
    // Backspace to delete
    if (keys.del) {
        if (textBuffer.length() > 0) {
            textBuffer.remove(textBuffer.length() - 1);
        }
        return;
    }
    
    // Backtick to cancel
    for (char c : keys.word) {
        if (c == '`') {
            textEditing = false;
            textBuffer = "";
            return;
        }
    }
    
    // Add typed characters (max 32)
    if (textBuffer.length() < 32) {
        for (char c : keys.word) {
            if (c >= 32 && c <= 126 && c != '`' && textBuffer.length() < 32) {
                textBuffer += c;
            }
        }
    }
}

void UnlockablesMenu::draw(M5Canvas& canvas) {
    if (!active) return;
    
    // If text editing, show input overlay
    if (textEditing) {
        drawTextInput(canvas);
        return;
    }
    
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    
    // Get unlocked status
    uint32_t unlocked = XP::getUnlockables();
    
    // Draw unlockables list
    int y = 2;
    int lineHeight = 18;
    
    for (uint8_t i = scrollOffset; i < TOTAL_UNLOCKABLES && i < scrollOffset + VISIBLE_ITEMS; i++) {
        bool hasIt = (unlocked & (1UL << UNLOCKABLES[i].bitIndex)) != 0;
        
        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // Lock/unlock indicator
        canvas.setCursor(4, y);
        canvas.print(hasIt ? "[X]" : "[ ]");
        
        // Name
        canvas.setCursor(28, y);
        canvas.print(UNLOCKABLES[i].name);
        
        y += lineHeight;
    }
    
    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 16);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < TOTAL_UNLOCKABLES) {
        canvas.setCursor(canvas.width() - 10, 16 + (VISIBLE_ITEMS - 1) * lineHeight);
        canvas.setTextColor(COLOR_FG);
        canvas.print("v");
    }
}

void UnlockablesMenu::drawTextInput(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    
    // Toast-style input box
    int boxW = 200;
    int boxH = 50;
    int boxX = (canvas.width() - boxW) / 2;
    int boxY = (canvas.height() - boxH) / 2;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    
    // Title
    canvas.drawString("ENTER THE KEY", canvas.width() / 2, boxY + 6);
    
    // Input field (show what they're typing)
    String displayText = textBuffer;
    if (displayText.length() > 20) {
        displayText = "..." + displayText.substring(displayText.length() - 17);
    }
    displayText += "_";  // Cursor
    canvas.drawString(displayText, canvas.width() / 2, boxY + 26);
    
    canvas.setTextDatum(top_left);
}

void UnlockablesMenu::updateBottomOverlay() {
    if (TOTAL_UNLOCKABLES > 0 && selectedIndex < TOTAL_UNLOCKABLES) {
        Display::setBottomOverlay(UNLOCKABLES[selectedIndex].hint);
    } else {
        Display::setBottomOverlay("NO SECRETS YET");
    }
}
