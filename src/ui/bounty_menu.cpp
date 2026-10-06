// Bounty Menu - View bounties to send to kid (Sirloin)
// Porkchop sends wardriven networks TO Sirloin for hunting
// Refactored to match captures_menu/boar_bros_menu patterns

#include "bounty_menu.h"
#include <M5Cardputer.h>
#include "display.h"
#include "../modes/pigsync_mode.h"
#include "../modes/warhog.h"

// Static member initialization
uint16_t BountyMenu::selectedIndex = 0;
uint16_t BountyMenu::scrollOffset = 0;
bool BountyMenu::active = false;
bool BountyMenu::keyWasPressed = false;

// Cached bounty list (refreshed each draw cycle to avoid 5x redundant calls)
static std::vector<uint64_t> cachedBounties;
static const uint32_t BOUNTY_CACHE_REFRESH_MS = 1000;
static uint32_t lastCacheRefreshMs = 0;
static bool cacheDirty = true;

static void refreshBountyCache(bool force) {
    uint32_t now = millis();
    if (!force && !cacheDirty && (now - lastCacheRefreshMs) < BOUNTY_CACHE_REFRESH_MS) {
        return;
    }
    cachedBounties = WarhogMode::getUnclaimedBSSIDs();
    lastCacheRefreshMs = now;
    cacheDirty = false;
}

// Format uint64_t BSSID to string
static void formatBSSID(uint64_t bssid, char* out, size_t len) {
    if (!out || len == 0) return;
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)(bssid >> 40),
             (uint8_t)(bssid >> 32),
             (uint8_t)(bssid >> 24),
             (uint8_t)(bssid >> 16),
             (uint8_t)(bssid >> 8),
             (uint8_t)(bssid));
}

void BountyMenu::init() {
    selectedIndex = 0;
    scrollOffset = 0;
}

void BountyMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    keyWasPressed = true;  // Ignore the Enter that opened us
    cacheDirty = true;
    lastCacheRefreshMs = 0;
    refreshBountyCache(true);
}

void BountyMenu::hide() {
    active = false;
    cachedBounties.clear();
    cachedBounties.shrink_to_fit();  // FIX: Return capacity to heap, avoid fragmentation
    cacheDirty = true;
    lastCacheRefreshMs = 0;
}

void BountyMenu::getSelectedInfo(char* out, size_t len) {
    if (!out || len == 0) return;
    refreshBountyCache(false);
    size_t readyCount = cachedBounties.size();
    
    // Get sync stats for bottom bar
    uint16_t totalSynced = PigSyncMode::getTotalSynced();
    uint8_t claimedCount = PigSyncMode::getLastBountyMatches();
    
    // Format: RDY:XX SYNC:XX CLMD:XX
    snprintf(out, len, "RDY:%u SYNC:%u CLMD:%u",
             (unsigned)readyCount, (unsigned)totalSynced, (unsigned)claimedCount);
}

void BountyMenu::update() {
    if (!active) return;
    handleInput();
}

void BountyMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    // Use cached bounties (refreshed by draw() each frame)
    size_t count = cachedBounties.size();
    
    // Navigation (; = up, . = down)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (count > 0 && selectedIndex < count - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // Backspace - go back
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
    }
}

void BountyMenu::draw(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    
    // Refresh cache on a timer to avoid per-frame allocations
    refreshBountyCache(false);
    
    if (cachedBounties.empty()) {
        drawEmpty(canvas);
    } else {
        drawList(canvas);
    }
}

void BountyMenu::drawList(M5Canvas& canvas) {
    // Uses cached bounties from draw()
    size_t count = cachedBounties.size();
    
    if (count == 0) return;
    
    // Bounds check (handle case where list shrank since last frame)
    if (selectedIndex >= count) {
        selectedIndex = (uint16_t)(count - 1);
    }
    if (scrollOffset > selectedIndex) {
        scrollOffset = selectedIndex;
    }
    
    // Start at top of canvas (no header)
    int y = 2;
    
    // Loop with uint16_t to support >255 items
    uint16_t endIdx = (uint16_t)min((size_t)(scrollOffset + VISIBLE_ITEMS), count);
    for (uint16_t i = scrollOffset; i < endIdx; i++) {
        uint64_t bssid = cachedBounties[i];
        char bssidStr[20];
        formatBSSID(bssid, bssidStr, sizeof(bssidStr));
        
        // Highlight selected (standard porkchop pattern - inverted colors)
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), LINE_H, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // BSSID (clean, left-aligned)
        canvas.setCursor(COL_LEFT, y);
        canvas.print(bssidStr);
        
        y += LINE_H;
    }
    
    // Scroll indicators (standard pattern)
    canvas.setTextColor(COLOR_FG);
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 2);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < count) {
        canvas.setCursor(canvas.width() - 10, 2 + (VISIBLE_ITEMS - 1) * LINE_H);
        canvas.print("v");
    }
}

void BountyMenu::drawEmpty(M5Canvas& canvas) {
    // Toast-style empty state (centered rounded box, inverted colors)
    const int boxW = 180;
    const int boxH = 50;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill (inverted colors)
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(MC_DATUM);
    
    canvas.drawString("N0 B0UNT13S Y3T!", canvas.width() / 2, boxY + 15);
    canvas.drawString("RUN W4RH0G [W] T0 HUNT", canvas.width() / 2, boxY + 35);
    
    // Reset text state
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(COLOR_FG);
}
