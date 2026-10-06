// Menu system - Sirloin-style grouped modal

#include "menu.h"
#include <M5Cardputer.h>
#include "display.h"
#include "../audio/sfx.h"
#include <string.h>

// ============================================================================
// STATIC DATA DEFINITIONS
// ============================================================================

// Hint pools (flash-resident)
static const char* const H_ATTACK[] = {
    "PIGS WITH TEETH. HEAP WITH FEAR.",
    "FERAL OPTIONS. STABILITY MIA.",
    "IF IT BOOTS: SHIP IT."
};
static const char* const H_RECON[] = {
    "EYES EVERYWHERE. TX NOWHERE.",
    "WATCHING HARD. TOUCHING NOTHING.",
    "RUNS GREAT UNTIL YOU LOOK AT IT."
};
static const char* const H_LOOT[] = {
    "THE TAKE. THE BAG. THE SD CARD.",
    "CAPTURED DATA + CAPTURED DIGNITY.",
    "HEAPS DON'T LIE. YELE YELE YELE."
};
static const char* const H_RANK[] = {
    "UR STREET CRED. SAVED IN 8.3.",
    "FLEX HARD. DEBUG SOFTER.",
    "REALTIME (EMOTIONAL)."
};
static const char* const H_COMMS[] = {
    "PHONE HOME. HOPE IS A PROTOCOL.",
    "BYTES GO OUT. VIBES GO DARK.",
    "IF IT BREAKS: BLAME THE MOON PHASE."
};
static const char* const H_SYSTEM[] = {
    "UNDER THE HOOD. NOTHING BUT ASH.",
    "NOW ENTERING: SETTINGS & REGRET.",
    "HEAP STATUS: VIBING (DEROGATORY)."
};

static const char* const H_OINK[] = {
    "FERAL DEAUTH ON TAP. OOM ON DECK.",
    "RESTRAINT IS WEAK. CRASHES ARE STRONG.",
    "STACK OVERFLOW. THE REAL BOSS FIGHT."
};
static const char* const H_BLUES[] = {
    "BLEZZ YOUR NEIGHBORS. CURSE YOUR HEAP.",
    "BECAUSE SILENCE IS FOR WELL-ADJUSTED.",
    "I OPTIMIZED IT. NOW IT FAILS FASTER."
};
static const char* const H_DNOHAM[] = {
    "DO NO HAM. ZERO TX. PURE POVERTY.",
    "PASSIVE MODE: MY WILL TO DEBUG.",
    "NO PSRAM. ONLY VIBES."
};
static const char* const H_WARHOG[] = {
    "GPS PATROL. LOST BUT CONFIDENT.",
    "PORK TRACKS. NO THOUGHTS.",
    "WPE IS A MYTH."
};
static const char* const H_SPCTRM[] = {
    "WATCH AIR MELT. RF THERAPY SESSION.",
    "SPECTRUM BUSY. LIKE MY ANXIETY.",
    "LOGS DON'T HELP. WAVES DON'T CARE."
};
static const char* const H_HASHES[] = {
    "FEED YO HASHCAT.",
    "COLLECTED PAIN. COMPRESSED.",
    "MALLOC SAID NAH."
};
static const char* const H_TRACKS[] = {
    "PORK TRAILS TO WIGLE.",
    "MAP IT OUT. PRETEND IT'S SCIENCE.",
    "IT'S NOT A BUG. IT'S A JOURNEY."
};
static const char* const H_BOUNTY[] = {
    "COLLECT BACON. AVOID CONSEQUENCES",
    "TARGETS LISTED. MORALS OPTIONAL.",
    "WORKING AS INTENDED (I INTENDED PAIN)."
};
static const char* const H_SYNC[] = {
    "PG PHONE HOME. PRAY IT CONNECTS.",
    "IF IT FAILS - DNS DID IT.",
    "SERIAL OUTPUT. CRY FOR HELP @115200."
};
static const char* const H_BACONTX[] = {
    "BEACON THE BLOCK. BLAME 'RF NOISE'.",
    "SOME CHAOS REQUIRED.",
    "I DIDN'T CRASH. NOT ME."
};
static const char* const H_XFIL[] = {
    "LOOT OUT. LIGHTS OUT.",
    "BYTES LEAVING. TROUBLE STAYING.",
    "HEAP DIED. PRAISE THE SUN."
};
static const char* const H_FLEX[] = {
    "SHOW YOUR GRIND. HIDE THE PAIN.",
    "LOOK MA, NO STABILITY.",
    "DEBUG LEVEL: REGRET."
};
static const char* const H_BADGES[] = {
    "MISCHIEF MERIT. LEGALLY DISTINCT.",
    "ACHIEVEMENTS UNLOCKED.",
    "PERFORMANCE MODE: DENIAL."
};
static const char* const H_SNOUTS[] = {
    "MOUNT YOUR TROPHIES. NO SNITCHES.",
    "COLLECTIBLES FOR THE HEAPLESS.",
    "HEAP FRAGGED. SOUL INTACT."
};
static const char* const H_SETTINGS[] = {
    "NOW SCREAMS IN UPPERCASE!!!",
    "TUNE IT. BREAK IT. TUNE IT AGAIN.",
    "FIX - SIMPLE. CAUSE - SPIRITUAL."
};
static const char* const H_BRBRS[] = {
    "RESPECT THE BRO. DON'T HAM THE HOMIES.",
    "FRIENDS? OF THE. HOG?",
    "SAFE MODE? NEVER HEARD OF HER."
};
static const char* const H_CRASHES[] = {
    "CORE DUMPS. CORE FEELS. SAME FILE.",
    "POST-MORTEM. ALWAYS ON.",
    "RESET BUTTON. THE REAL UI."
};
static const char* const H_DIAG[] = {
    "DUDE WHERE'S MY HEAP?",
    "STABLE. YEAH. PREDICTABLE? LOL.",
    "HEALTHCHECK PASSED. YOU DIED."
};
static const char* const H_SDFMT[] = {
    "FAT32 OR BUST.",
    "WIPE THE PAST. FORMAT THE FUTURE.",
    "SD CARD REBORN."
};
static const char* const H_ABOUT[] = {
    "IT WAS not A MISTAKE. ",
    "CREDIT ROLLS. HEAP FALLS.",
    "DOCUMENT NOTHING."
};
static const char* const H_CHARGING[] = {
    "PLUG IN. ZONE OUT. SAVE POWER.",
    "BATTERY REST. SERVICES CEASED.",
    "CHARGING VIBES. MAX CHILL."
};
static const char* const H_JANUS[] = {
    "TWO FACES. ALL BANDS. JANUS HOG.",
    "C5 COPROCESSOR LINK STATUS.",
    "5GHZ RECON VIA MONSTERC5."
};

// Root menu items
const RootItem Menu::ROOT_ITEMS[] = {
    {"/>",  "ATTACK",  H_ATTACK,  (uint8_t)(sizeof(H_ATTACK)/sizeof(H_ATTACK[0])),  RootType::GROUP,  {.groupId = GroupId::ATTACK}},
    {"o~",  "RECON",   H_RECON,   (uint8_t)(sizeof(H_RECON)/sizeof(H_RECON[0])),    RootType::GROUP,  {.groupId = GroupId::RECON}},
    {"[$",  "LOOT",    H_LOOT,    (uint8_t)(sizeof(H_LOOT)/sizeof(H_LOOT[0])),      RootType::GROUP,  {.groupId = GroupId::LOOT}},
    {"^#",  "RANK",    H_RANK,    (uint8_t)(sizeof(H_RANK)/sizeof(H_RANK[0])),      RootType::GROUP,  {.groupId = GroupId::RANK}},
    {"))",  "COMMS",   H_COMMS,   (uint8_t)(sizeof(H_COMMS)/sizeof(H_COMMS[0])),    RootType::GROUP,  {.groupId = GroupId::COMMS}},
    {"::",  "SYSTEM",  H_SYSTEM,  (uint8_t)(sizeof(H_SYSTEM)/sizeof(H_SYSTEM[0])),  RootType::GROUP,  {.groupId = GroupId::SYSTEM}}
};
const uint8_t Menu::ROOT_COUNT = sizeof(ROOT_ITEMS) / sizeof(ROOT_ITEMS[0]);

// Group: ATTACK - offensive TX operations
const MenuItem Menu::GROUP_ATTACK[] = {
    {"/>", "OINKS",  1,  H_OINK,   (uint8_t)(sizeof(H_OINK)/sizeof(H_OINK[0]))},
    {"!!", "BLUES", 8,  H_BLUES,  (uint8_t)(sizeof(H_BLUES)/sizeof(H_BLUES[0]))}
};
const uint8_t Menu::GROUP_ATTACK_SIZE = sizeof(GROUP_ATTACK) / sizeof(GROUP_ATTACK[0]);

// Group: RECON - passive RX intelligence
const MenuItem Menu::GROUP_RECON[] = {
    {"o~", "DNOHAM",  14, H_DNOHAM, (uint8_t)(sizeof(H_DNOHAM)/sizeof(H_DNOHAM[0]))},
    {"<>", "WARHOG",  2,  H_WARHOG, (uint8_t)(sizeof(H_WARHOG)/sizeof(H_WARHOG[0]))},
    {"~~", "SPCTRM", 10,  H_SPCTRM, (uint8_t)(sizeof(H_SPCTRM)/sizeof(H_SPCTRM[0]))}
};
const uint8_t Menu::GROUP_RECON_SIZE = sizeof(GROUP_RECON) / sizeof(GROUP_RECON[0]);

// Group: LOOT - captured data and targets
const MenuItem Menu::GROUP_LOOT[] = {
    {"C#", "HASHES",  4,  H_HASHES, (uint8_t)(sizeof(H_HASHES)/sizeof(H_HASHES[0]))},
    {"~>", "TRACKS",  13, H_TRACKS, (uint8_t)(sizeof(H_TRACKS)/sizeof(H_TRACKS[0]))},
    {"B$", "BOUNTY",  17, H_BOUNTY, (uint8_t)(sizeof(H_BOUNTY)/sizeof(H_BOUNTY[0]))}
};
const uint8_t Menu::GROUP_LOOT_SIZE = sizeof(GROUP_LOOT) / sizeof(GROUP_LOOT[0]);

// Group: COMMS - external communication
const MenuItem Menu::GROUP_COMMS[] = {
    {"@)", "PIGSYNC",    16, H_SYNC,    (uint8_t)(sizeof(H_SYNC)/sizeof(H_SYNC[0]))},
    {"))", "BACONTX", 18, H_BACONTX, (uint8_t)(sizeof(H_BACONTX)/sizeof(H_BACONTX[0]))},
    {"FX", "TRANSFR",    3,  H_XFIL,    (uint8_t)(sizeof(H_XFIL)/sizeof(H_XFIL[0]))},
    {"5G", "JANUSHG",   22, H_JANUS,   (uint8_t)(sizeof(H_JANUS)/sizeof(H_JANUS[0]))}
};
const uint8_t Menu::GROUP_COMMS_SIZE = sizeof(GROUP_COMMS) / sizeof(GROUP_COMMS[0]);

// Group: RANK - progression and street cred
const MenuItem Menu::GROUP_RANK[] = {
    {"^#", "FLEXES",    11, H_FLEX,   (uint8_t)(sizeof(H_FLEX)/sizeof(H_FLEX[0]))},
    {"*#", "BADGES",   9, H_BADGES, (uint8_t)(sizeof(H_BADGES)/sizeof(H_BADGES[0]))},
    {"?*", "UNLOCK",  15, H_SNOUTS, (uint8_t)(sizeof(H_SNOUTS)/sizeof(H_SNOUTS[0]))}
};
const uint8_t Menu::GROUP_RANK_SIZE = sizeof(GROUP_RANK) / sizeof(GROUP_RANK[0]);

// Group: SYSTEM - utilities and config
const MenuItem Menu::GROUP_SYSTEM[] = {
    {"==", "SETTINGS",   5,  H_SETTINGS, (uint8_t)(sizeof(H_SETTINGS)/sizeof(H_SETTINGS[0]))},
    {"[]", "BOARBROS",  12, H_BRBRS,    (uint8_t)(sizeof(H_BRBRS)/sizeof(H_BRBRS[0]))},
    {"!!", "COREDUMP",  7,  H_CRASHES,  (uint8_t)(sizeof(H_CRASHES)/sizeof(H_CRASHES[0]))},
    {"::", "DIAGDATA",   19, H_DIAG,     (uint8_t)(sizeof(H_DIAG)/sizeof(H_DIAG[0]))},
    {"SD", "FORMATSD",  20, H_SDFMT,    (uint8_t)(sizeof(H_SDFMT)/sizeof(H_SDFMT[0]))},
    {"~~", "CHARGING",  21, H_CHARGING, (uint8_t)(sizeof(H_CHARGING)/sizeof(H_CHARGING[0]))},
    {":?", "ABOUTPIG",   6,  H_ABOUT,    (uint8_t)(sizeof(H_ABOUT)/sizeof(H_ABOUT[0]))}
};
const uint8_t Menu::GROUP_SYSTEM_SIZE = sizeof(GROUP_SYSTEM) / sizeof(GROUP_SYSTEM[0]);

// ============================================================================
// STATIC MEMBER INITIALIZATION
// ============================================================================

uint8_t Menu::rootIdx = 0;
uint8_t Menu::rootScroll = 0;
GroupId Menu::activeGroup = GroupId::NONE;
uint8_t Menu::modalIdx = 0;
uint8_t Menu::modalScroll = 0;
bool Menu::active = false;
MenuCallback Menu::callback = nullptr;
bool Menu::keyWasPressed = false;
uint8_t Menu::rootHintIndex[Menu::ROOT_COUNT] = {0};
uint8_t Menu::attackHintIndex[Menu::GROUP_ATTACK_SIZE] = {0};
uint8_t Menu::reconHintIndex[Menu::GROUP_RECON_SIZE] = {0};
uint8_t Menu::lootHintIndex[Menu::GROUP_LOOT_SIZE] = {0};
uint8_t Menu::commsHintIndex[Menu::GROUP_COMMS_SIZE] = {0};
uint8_t Menu::rankHintIndex[Menu::GROUP_RANK_SIZE] = {0};
uint8_t Menu::systemHintIndex[Menu::GROUP_SYSTEM_SIZE] = {0};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

bool Menu::isRootSelectable(uint8_t idx) {
    if (idx >= ROOT_COUNT) return false;
    return ROOT_ITEMS[idx].type != RootType::SEPARATOR;
}

const MenuItem* Menu::getGroupItems(GroupId group) {
    switch (group) {
        case GroupId::ATTACK:  return GROUP_ATTACK;
        case GroupId::RECON:   return GROUP_RECON;
        case GroupId::LOOT:    return GROUP_LOOT;
        case GroupId::COMMS:   return GROUP_COMMS;
        case GroupId::RANK:    return GROUP_RANK;
        case GroupId::SYSTEM:  return GROUP_SYSTEM;
        default: return nullptr;
    }
}

uint8_t Menu::getGroupSize(GroupId group) {
    switch (group) {
        case GroupId::ATTACK:  return GROUP_ATTACK_SIZE;
        case GroupId::RECON:   return GROUP_RECON_SIZE;
        case GroupId::LOOT:    return GROUP_LOOT_SIZE;
        case GroupId::COMMS:   return GROUP_COMMS_SIZE;
        case GroupId::RANK:    return GROUP_RANK_SIZE;
        case GroupId::SYSTEM:  return GROUP_SYSTEM_SIZE;
        default: return 0;
    }
}

const char* Menu::getGroupName(GroupId group) {
    switch (group) {
        case GroupId::ATTACK:  return "ATTACK";
        case GroupId::RECON:   return "RECON";
        case GroupId::LOOT:    return "LOOT";
        case GroupId::COMMS:   return "COMMS";
        case GroupId::RANK:    return "RANK";
        case GroupId::SYSTEM:  return "SYSTEM";
        default: return "";
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void Menu::setCallback(MenuCallback cb) {
    callback = cb;
}

void Menu::init() {
    rootIdx = 0;
    rootScroll = 0;
    activeGroup = GroupId::NONE;
    modalIdx = 0;
    modalScroll = 0;
    // Seed root hint selection
    for (uint8_t i = 0; i < ROOT_COUNT && i < sizeof(rootHintIndex)/sizeof(rootHintIndex[0]); i++) {
        if (ROOT_ITEMS[i].hintCount > 0) {
            rootHintIndex[i] = esp_random() % ROOT_ITEMS[i].hintCount;
        } else {
            rootHintIndex[i] = 0;
        }
    }
}

void Menu::show() {
    active = true;
    rootIdx = 0;
    rootScroll = 0;
    activeGroup = GroupId::NONE;
    modalIdx = 0;
    modalScroll = 0;
    // Rotate root hints on each show
    for (uint8_t i = 0; i < ROOT_COUNT && i < sizeof(rootHintIndex)/sizeof(rootHintIndex[0]); i++) {
        if (ROOT_ITEMS[i].hintCount > 0) {
            rootHintIndex[i] = esp_random() % ROOT_ITEMS[i].hintCount;
        } else {
            rootHintIndex[i] = 0;
        }
    }
}

void Menu::hide() {
    active = false;
    activeGroup = GroupId::NONE;
}

bool Menu::closeModal() {
    if (activeGroup == GroupId::NONE) return false;
    activeGroup = GroupId::NONE;
    modalIdx = 0;
    modalScroll = 0;
    return true;
}

const char* Menu::getSelectedDescription() {
    if (activeGroup != GroupId::NONE) {
        const MenuItem* items = getGroupItems(activeGroup);
        uint8_t* indices = nullptr;
        switch (activeGroup) {
            case GroupId::ATTACK: indices = attackHintIndex; break;
            case GroupId::RECON:  indices = reconHintIndex;  break;
            case GroupId::LOOT:   indices = lootHintIndex;   break;
            case GroupId::COMMS:  indices = commsHintIndex;  break;
            case GroupId::RANK:   indices = rankHintIndex;   break;
            case GroupId::SYSTEM: indices = systemHintIndex; break;
            default: break;
        }
        uint8_t groupSize = getGroupSize(activeGroup);
        if (items && indices && modalIdx < groupSize && items[modalIdx].hintCount > 0 && indices[modalIdx] < items[modalIdx].hintCount) {
            return items[modalIdx].hintPool[indices[modalIdx]];
        }
        return "";
    }
    // At root - return selected root item's hint
    if (rootIdx >= ROOT_COUNT || ROOT_ITEMS[rootIdx].hintCount == 0 || rootHintIndex[rootIdx] >= ROOT_ITEMS[rootIdx].hintCount) return "";
    return ROOT_ITEMS[rootIdx].hintPool[rootHintIndex[rootIdx]];
}

void Menu::update() {
    if (!active) return;
    handleInput();
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

void Menu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    if (activeGroup != GroupId::NONE) {
        // === MODAL INPUT ===
        uint8_t groupSize = getGroupSize(activeGroup);
        
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (modalIdx > 0) {
                modalIdx--;
                SFX::play(SFX::MENU_CLICK);
                if (modalIdx < modalScroll) {
                    modalScroll = modalIdx;
                }
            }
        }
        
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if (modalIdx < groupSize - 1) {
                modalIdx++;
                SFX::play(SFX::MENU_CLICK);
                if (modalIdx >= modalScroll + MODAL_VISIBLE) {
                    modalScroll = modalIdx - MODAL_VISIBLE + 1;
                }
            }
        }
        
        if (keys.enter) {
            SFX::play(SFX::MENU_CLICK);
            const MenuItem* items = getGroupItems(activeGroup);
            if (items && callback) {
                callback(items[modalIdx].actionId);
            }
            closeModal();
        }
        
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            closeModal();
        }
        
    } else {
        // === ROOT INPUT ===
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            // Move up, skip non-selectable
            int newIdx = rootIdx;
            do {
                if (newIdx > 0) newIdx--;
                else break;
            } while (!isRootSelectable(newIdx) && newIdx > 0);
            
            if (isRootSelectable(newIdx) && newIdx != rootIdx) {
                rootIdx = newIdx;
                SFX::play(SFX::MENU_CLICK);
                if (rootIdx < rootScroll) {
                    rootScroll = rootIdx;
                }
            }
        }
        
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            // Move down, skip non-selectable
            int newIdx = rootIdx;
            do {
                if (newIdx < ROOT_COUNT - 1) newIdx++;
                else break;
            } while (!isRootSelectable(newIdx) && newIdx < ROOT_COUNT - 1);
            
            if (isRootSelectable(newIdx) && newIdx != rootIdx) {
                rootIdx = newIdx;
                SFX::play(SFX::MENU_CLICK);
                if (rootIdx >= rootScroll + VISIBLE_ITEMS) {
                    rootScroll = rootIdx - VISIBLE_ITEMS + 1;
                }
            }
        }
        
        if (keys.enter) {
            SFX::play(SFX::MENU_CLICK);
            const RootItem& item = ROOT_ITEMS[rootIdx];
            if (item.type == RootType::GROUP) {
                // Open modal
                activeGroup = item.groupId;
                modalIdx = 0;
                modalScroll = 0;
                // Pick modal hints for the group
                const MenuItem* items = getGroupItems(activeGroup);
                uint8_t groupSize = getGroupSize(activeGroup);
                uint8_t* indices = nullptr;
                switch (activeGroup) {
                    case GroupId::ATTACK: indices = attackHintIndex; break;
                    case GroupId::RECON:  indices = reconHintIndex; break;
                    case GroupId::LOOT:   indices = lootHintIndex;   break;
                    case GroupId::COMMS:  indices = commsHintIndex;  break;
                    case GroupId::RANK:   indices = rankHintIndex;   break;
                    case GroupId::SYSTEM: indices = systemHintIndex; break;
                    default: break;
                }
                if (items && indices) {
                    uint8_t maxIndex = 0;
                    switch (activeGroup) {
                        case GroupId::ATTACK: maxIndex = sizeof(attackHintIndex)/sizeof(attackHintIndex[0]); break;
                        case GroupId::RECON:  maxIndex = sizeof(reconHintIndex)/sizeof(reconHintIndex[0]); break;
                        case GroupId::LOOT:   maxIndex = sizeof(lootHintIndex)/sizeof(lootHintIndex[0]); break;
                        case GroupId::COMMS:  maxIndex = sizeof(commsHintIndex)/sizeof(commsHintIndex[0]); break;
                        case GroupId::RANK:   maxIndex = sizeof(rankHintIndex)/sizeof(rankHintIndex[0]); break;
                        case GroupId::SYSTEM: maxIndex = sizeof(systemHintIndex)/sizeof(systemHintIndex[0]); break;
                        default: maxIndex = 0; break;
                    }
                    for (uint8_t i = 0; i < groupSize && i < maxIndex; i++) {
                        if (items[i].hintCount > 0) {
                            indices[i] = esp_random() % items[i].hintCount;
                        } else {
                            indices[i] = 0;
                        }
                    }
                }
            } else if (item.type == RootType::DIRECT) {
                // Direct action
                if (callback) {
                    callback(item.actionId);
                }
            }
        }
    }
}

// ============================================================================
// DRAWING
// ============================================================================

void Menu::draw(M5Canvas& canvas) {
    if (!active) return;
    
    // Always draw root
    drawRoot(canvas);
    
    // Overlay modal if active
    if (activeGroup != GroupId::NONE) {
        drawModal(canvas);
    }
}

void Menu::drawRoot(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    uint16_t accent = fg;  // Same as COLOR_ACCENT
    
    canvas.fillSprite(bg);
    canvas.setTextColor(fg);
    
    // Title with root icon
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    char titleBuf[32];
    const RootItem& sel = ROOT_ITEMS[rootIdx];
    if (sel.icon && sel.icon[0] && strlen(sel.icon) < sizeof(titleBuf) - 10) {  // Ensure enough space for " PORKCHOP OS"
        snprintf(titleBuf, sizeof(titleBuf), "%s PORKCHOP OS", sel.icon);
    } else {
        strncpy(titleBuf, "PORKCHOP OS", sizeof(titleBuf) - 1);
        titleBuf[sizeof(titleBuf) - 1] = '\0';
    }
    canvas.drawString(titleBuf, DISPLAY_W / 2, 2);
    canvas.drawLine(10, 20, DISPLAY_W - 10, 20, accent);
    
    // Root items
    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);
    int yOffset = 25;
    int lineHeight = 18;
    
    for (uint8_t i = 0; i < VISIBLE_ITEMS && (rootScroll + i) < ROOT_COUNT; i++) {
        uint8_t idx = rootScroll + i;
        int y = yOffset + i * lineHeight;
        const RootItem& item = ROOT_ITEMS[idx];
        
        // Separator
        if (item.type == RootType::SEPARATOR) {
            canvas.drawLine(20, y + lineHeight/2, DISPLAY_W - 20, y + lineHeight/2, accent);
            continue;
        }
        
        bool isSelected = (idx == rootIdx) && (activeGroup == GroupId::NONE);
        
        if (isSelected) {
            canvas.fillRect(5, y - 2, DISPLAY_W - 10, lineHeight, accent);
            canvas.setTextColor(bg);
        } else {
            canvas.setTextColor(fg);
        }
        
        // Label with icon and arrow for groups
        char labelBuf[40];
        const char* icon = (item.icon && item.icon[0]) ? item.icon : ">";
        if (item.type == RootType::GROUP) {
            // Check that the combined length won't exceed buffer size
            size_t totalLen = strlen(icon) + strlen(item.label) + 3; // +3 for " >" and null terminator
            if (totalLen <= sizeof(labelBuf)) {
                snprintf(labelBuf, sizeof(labelBuf), "%s %s >", icon, item.label);
            } else {
                // Truncate the label to fit in the buffer
                size_t maxLabelLen = sizeof(labelBuf) - strlen(icon) - 3; // -3 for " >" and null terminator
                if (maxLabelLen > 0) {
                    snprintf(labelBuf, sizeof(labelBuf), "%s %.*s >", icon, (int)maxLabelLen, item.label);
                } else {
                    strncpy(labelBuf, icon, sizeof(labelBuf) - 1);
                    labelBuf[sizeof(labelBuf) - 1] = '\0';
                }
            }
        } else {
            // Check that the combined length won't exceed buffer size
            size_t totalLen = strlen(icon) + strlen(item.label) + 2; // +2 for space and null terminator
            if (totalLen <= sizeof(labelBuf)) {
                snprintf(labelBuf, sizeof(labelBuf), "%s %s", icon, item.label);
            } else {
                // Truncate the label to fit in the buffer
                size_t maxLabelLen = sizeof(labelBuf) - strlen(icon) - 2; // -2 for space and null terminator
                if (maxLabelLen > 0) {
                    snprintf(labelBuf, sizeof(labelBuf), "%s %.*s", icon, (int)maxLabelLen, item.label);
                } else {
                    strncpy(labelBuf, icon, sizeof(labelBuf) - 1);
                    labelBuf[sizeof(labelBuf) - 1] = '\0';
                }
            }
        }
        canvas.drawString(labelBuf, 10, y);
    }
    
    // Scroll indicators
    canvas.setTextColor(fg);
    canvas.setTextSize(1);
    if (rootScroll > 0) {
        canvas.drawString("^", DISPLAY_W - 12, 22);
    }
    if (rootScroll + VISIBLE_ITEMS < ROOT_COUNT) {
        canvas.drawString("v", DISPLAY_W - 12, yOffset + (VISIBLE_ITEMS - 1) * lineHeight);
    }
}

void Menu::drawModal(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    
    // Modal dimensions - Sirloin-style
    int boxW = 220;
    int boxH = 90;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = 20;
    
    // Background with border
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 6, fg);
    canvas.drawRoundRect(boxX, boxY, boxW, boxH, 6, bg);
    
    // Title
    canvas.setTextColor(bg);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.drawString(getGroupName(activeGroup), boxX + boxW/2, boxY + 4);
    canvas.drawLine(boxX + 10, boxY + 20, boxX + boxW - 10, boxY + 20, bg);
    canvas.setTextDatum(top_left);
    
    // Items
    const MenuItem* items = getGroupItems(activeGroup);
    uint8_t groupSize = getGroupSize(activeGroup);
    int itemStartY = boxY + 24;
    int itemHeight = 16;
    int itemPadX = 6;
    int textIndent = 10;
    int valueMargin = 14;
    
    canvas.setTextSize(2);
    
    for (int i = 0; i < MODAL_VISIBLE && (modalScroll + i) < groupSize; i++) {
        int idx = modalScroll + i;
        int y = itemStartY + i * itemHeight;
        const MenuItem& item = items[idx];
        
        bool isSelected = (idx == modalIdx);
        
        if (isSelected) {
            canvas.fillRect(boxX + itemPadX, y, boxW - (itemPadX * 2), itemHeight - 1, bg);
            canvas.setTextColor(fg);
            canvas.setCursor(boxX + textIndent, y);
            canvas.print("> ");
        } else {
            canvas.setTextColor(bg);
            canvas.setCursor(boxX + textIndent, y);
            canvas.print("  ");
        }
        
        // Icon + truncated label
        if (item.icon && item.icon[0]) {
            canvas.print(item.icon);
            canvas.print(" ");
        } else {
            canvas.print("  ");
        }

        constexpr int kMaxLabelChars = 10;  // keep width with icon
        char shortLabel[kMaxLabelChars + 1];
        // Ensure we don't copy more characters than the buffer can hold
        if (strlen(item.label) <= kMaxLabelChars) {
            strcpy(shortLabel, item.label);
        } else {
            strncpy(shortLabel, item.label, kMaxLabelChars);
            shortLabel[kMaxLabelChars] = '\0';
        }
        canvas.print(shortLabel);
    }
    
    // Scroll indicators
    canvas.setTextSize(1);
    canvas.setTextColor(bg);
    if (modalScroll > 0) {
        canvas.setCursor(boxX + boxW - 12, itemStartY + 4);
        canvas.print("^");
    }
    if (modalScroll + MODAL_VISIBLE < groupSize) {
        canvas.setCursor(boxX + boxW - 12, itemStartY + (MODAL_VISIBLE - 1) * itemHeight + 4);
        canvas.print("v");
    }
}
