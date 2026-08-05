// GUARD HOG - DEFENSIVE SUITE watch face. See guardhog.h.
// Original code for M5PORKCHOP (MIT). Tracker signatures: tracker_detect.h.
#include "guardhog.h"

#include "../core/config.h"
#include "../core/network_recon.h"
#include "../core/sd_layout.h"
#include "../gps/gps.h"
#include "../audio/sfx.h"
#include "../ui/display.h"
#include "../piglet/avatar.h"
#include "../piglet/mood.h"
#include "../core/xp.h"

#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <SD.h>
#include <math.h>
#include <string.h>

// ============================================================================
// Static members
// ============================================================================
bool GuardHogMode::running = false;
bool GuardHogMode::bleStarted = false;

GuardHogMode::TrackerEntry GuardHogMode::trackers[GuardHogMode::kMaxTrackers];
uint8_t GuardHogMode::trackerCount = 0;

GuardHogMode::FollowEntry GuardHogMode::followers[GuardHogMode::kMaxFollowers];
uint8_t GuardHogMode::followerCount = 0;
uint8_t GuardHogMode::followingFlagged = 0;

uint8_t GuardHogMode::droneSeen[GuardHogMode::kMaxSeen][6];
uint8_t GuardHogMode::droneCount = 0;
uint8_t GuardHogMode::flipperSeen[GuardHogMode::kMaxSeen][6];
uint8_t GuardHogMode::flipperCount = 0;

volatile GuardHogMode::Sighting GuardHogMode::sightRing[GuardHogMode::kSightSlots];
volatile uint8_t GuardHogMode::sightWrite = 0;
volatile uint8_t GuardHogMode::sightRead = 0;

GuardHogMode::RadioPhase GuardHogMode::radioPhase = GuardHogMode::RadioPhase::BleSlice;
uint32_t GuardHogMode::phaseStartMs = 0;
WatchState GuardHogMode::watch = WatchState::Calm;

static const uint32_t TRACKER_STALE_MS = 30000;   // drop from list after 30s unseen
static char ghLogFile[128] = {0};

// GUARD HOG "life" economy (named tuning constants; see mood.cpp for the decay
// side). +1 happiness per completed BLE<->WiFi rotation (~17s) offsets the
// reduced on-duty decay so idle nets ~break-even (a hair below zero).
static const int GH_VIGILANCE_HAPPINESS = 1;

// Award one dedup'd defensive catch: XP (visible bar bump) + a mood fatten-up.
static inline void ghAwardCatch() {
    XP::addXP(XP_DEFENSE_HIT);
    Mood::onDefensiveCatch();
}

// ============================================================================
// BLE scan callback (runs in NimBLE task context — keep it cheap)
// ============================================================================
class GuardHogScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        if (!device) return;
        const std::vector<uint8_t>& pl = device->getPayload();
        if (pl.empty()) return;

        // getAddress() returns a NimBLEAddress BY VALUE; getVal()/getBase() point
        // INTO it, so it must stay alive while we read. Bind it to a named local
        // (a bare `getAddress().getVal()` would dangle after the full expression).
        // NimBLE stores the address little-endian (val[0] = LSB); un-reverse to
        // human display order so prefix matching + display tails are correct.
        NimBLEAddress bleAddr = device->getAddress();
        const uint8_t* le = bleAddr.getVal();
        uint8_t addr[6];
        if (le) { for (int i = 0; i < 6; ++i) addr[i] = le[5 - i]; }
        else    { memset(addr, 0, 6); }
        int8_t rssi = (int8_t)device->getRSSI();

        uint8_t len = (pl.size() > 255) ? 255 : (uint8_t)pl.size();
        trackerdet::TrackerHit t = trackerdet::trackerInspectBleAdv(addr, pl.data(), len, rssi);
        dronedet::DroneHit     d = dronedet::droneInspectBleAdv(addr, pl.data(), len, rssi);
        flipperdet::FlipperHit f = flipperdet::flipperInspect(addr, pl.data(), len, rssi);

        // Every sighting feeds TAIL WAGGER; typed hits light their detectors.
        GuardHogMode::enqueueSighting(addr, rssi, (uint8_t)t.type, t.lost, d.hit(), f.hit());
    }
};
static GuardHogScanCallbacks g_scanCallbacks;

// ============================================================================
// Lifecycle
// ============================================================================
void GuardHogMode::start() {
    if (running) return;

    trackerCount = 0;
    followerCount = 0;
    followingFlagged = 0;
    droneCount = 0;
    flipperCount = 0;
    sightWrite = sightRead = 0;
    ghLogFile[0] = '\0';

    watch = WatchState::Calm;

    // GUARD HOG owns the radio and time-shares it. Start clean: stop the WiFi
    // engine, then begin the BLE slice (which brings NimBLE up with WiFi down).
    NetworkRecon::stop();
    running = true;
    enterBleSlice();

    Avatar::setState(AvatarState::HUNTING);
    Display::notify(NoticeKind::STATUS, "GUARD HOG - WATCHING", 4000, NoticeChannel::TOP_BAR);
}

void GuardHogMode::stop() {
    if (!running) return;
    running = false;

    stopBleScan();
    // Only ONE controller may be up when WiFi restarts. If BLE is still inited,
    // NetworkRecon::start() will deinit it as part of its coex-safe bring-up.
    NetworkRecon::start();   // restore background WiFi recon for other modes
    Avatar::setState(AvatarState::NEUTRAL);
}

// ---- Eye-Spy radio rotation ------------------------------------------------
// Only ONE radio's controller is ever enabled at a time. Bringing WiFi up while
// the BT controller is still enabled aborts in coex_enable() (coredump-proven),
// so each handoff fully tears the other radio down before enabling the next.
void GuardHogMode::enterBleSlice() {
    // Vigilance trickle: reaching a BLE slice FROM a WiFi slice means one full
    // BLE<->WiFi rotation of guarding just completed. (The first entry from
    // start() has radioPhase already BleSlice, so it doesn't award.)
    if (radioPhase == RadioPhase::WifiSlice) {
        Mood::adjustHappiness(GH_VIGILANCE_HAPPINESS);
    }

    // Coming from the WiFi slice: stop WiFi promiscuous + free the WiFi driver so
    // the BT controller can own the radio.
    NetworkRecon::stop();
    WiFi.mode(WIFI_OFF);
    delay(30);              // let WiFi/coex fully tear down before BLE comes up
    startBleScan();         // (re)inits NimBLE + starts the passive scan
    radioPhase = RadioPhase::BleSlice;
    phaseStartMs = millis();
    Serial.printf("[GH] -> BLE slice @ %lums\n", (unsigned long)phaseStartMs);
}

void GuardHogMode::enterWifiSlice() {
    // Stop + FULLY tear down BLE (deinit the controller) before WiFi starts, or
    // coex_enable() aborts. NetworkRecon::start() then does the proven coex-safe
    // WiFi promiscuous bring-up (it also deinits NimBLE if still inited); its
    // callback feeds flock/attack/evil-twin and the global serviceFlockAlerts
    // drains + alerts. No GUARD-HOG-owned promiscuous callback needed.
    stopBleScan();
    if (NimBLEDevice::isInitialized()) {
        NimBLEDevice::deinit(true);
    }
    bleStarted = false;
    delay(30);              // let the BT controller fully release the radio
    NetworkRecon::start();
    radioPhase = RadioPhase::WifiSlice;
    phaseStartMs = millis();
    Serial.printf("[GH] -> WiFi slice @ %lums\n", (unsigned long)phaseStartMs);
}

void GuardHogMode::startBleScan() {
    if (bleStarted) return;
    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("");
    }
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) return;
    pScan->setScanCallbacks(&g_scanCallbacks, false);
    pScan->setActiveScan(false);   // PASSIVE — never transmit scan requests
    pScan->setInterval(160);       // 100ms
    pScan->setWindow(150);         // window < interval (leave the radio breathing room)
    pScan->setDuplicateFilter(false);
    // CRITICAL: callback-only. Without this, duplicateFilter(false) makes NimBLE
    // stash every advertisement in its results vector, which grows unbounded
    // through the 9s slice and exhausts heap (no PSRAM) -> crash a few seconds in.
    // 0 = don't store results; we process entirely in onResult().
    pScan->setMaxResults(0);
    pScan->start(0, false, true);  // duration=0 forever, non-blocking, continuous cb
    bleStarted = true;
}

void GuardHogMode::stopBleScan() {
    if (!bleStarted) return;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan) {
        if (pScan->isScanning()) pScan->stop();
        pScan->clearResults();
    }
    // Do NOT deinit NimBLE — ESP32-S3 is unhappy re-initing after deinit.
    bleStarted = false;
}

// ============================================================================
// Sighting ring (callback-safe enqueue)
// ============================================================================
void GuardHogMode::enqueueSighting(const uint8_t* addr, int8_t rssi,
                                   uint8_t trackerType, bool lost,
                                   bool drone, bool flipper) {
    uint8_t w = sightWrite;
    uint8_t nxt = (uint8_t)((w + 1) % kSightSlots);
    if (nxt == sightRead) return;   // full: drop
    for (int i = 0; i < 6; ++i) sightRing[w].addr[i] = addr[i];
    sightRing[w].rssi = rssi;
    sightRing[w].type = trackerType;
    sightRing[w].lost = lost;
    sightRing[w].drone = drone;
    sightRing[w].flipper = flipper;
    sightWrite = nxt;
}

bool GuardHogMode::seenContains(const uint8_t seen[][6], uint8_t n, const uint8_t* mac) {
    for (uint8_t i = 0; i < n; ++i)
        if (memcmp(seen[i], mac, 6) == 0) return true;
    return false;
}

// ============================================================================
// Engines (main-loop context)
// ============================================================================
void GuardHogMode::update() {
    if (!running) return;
    uint32_t now = millis();

    // BLE sightings are drained in every slice (the ring keeps filling during
    // the BLE slice and is emptied here). WiFi-radio detections drain in the
    // global serviceFlockAlerts() on the main loop.
    processSightings();
    ageTrackers();

    // Eye-Spy rotation. During the WiFi slice NetworkRecon owns the radio and
    // hops channels itself, so there is nothing to service here.
    if (radioPhase == RadioPhase::BleSlice) {
        if (now - phaseStartMs >= BLE_SLICE_MS) enterWifiSlice();
    } else {
        if (now - phaseStartMs >= WIFI_SLICE_MS) enterBleSlice();
    }

    computeWatch();
}

// Fuse every detector into CALM / SNIFFY / SPOOKED. SPOOKED requires two
// independent signals to agree — ideally one per radio (a WiFi Flock/attack/
// evil-twin AND a BLE follower/drone), or two distinct WiFi attack signals.
void GuardHogMode::computeWatch() {
    // BLE-radio signals
    bool bleStrong = (followingFlagged > 0) || (droneCount > 0);
    bool bleWeak   = (trackerCount > 0) || (flipperCount > 0);

    // WiFi-radio signals (accumulated by the shared detectors)
    bool flock  = NetworkRecon::getFlockAlertCount() > 0;
    bool attack = NetworkRecon::getLastAttack() != attackdet::AttackType::None;
    bool twin   = NetworkRecon::getEvilTwinCount() > 0;
    uint8_t wifiStrongCount = (uint8_t)flock + (uint8_t)attack + (uint8_t)twin;
    bool wifiStrong = wifiStrongCount > 0;

    WatchState s;
    if ((bleStrong && wifiStrong) || wifiStrongCount >= 2) {
        s = WatchState::Spooked;            // two independent signals agree
    } else if (bleStrong || wifiStrong || bleWeak) {
        s = WatchState::Sniffy;             // one signal — something's there
    } else {
        s = WatchState::Calm;
    }

    if (s != watch) {
        WatchState prev = watch;
        watch = s;
        if (s == WatchState::Spooked && prev != WatchState::Spooked) {
            Display::showToast("SPOOKED\nTWO RADIOS AGREE");
            SFX::play(SFX::PIG_ALARM);
            Avatar::setState(AvatarState::ANGRY);
            // A corroborated real threat is the best meal — bigger XP + fatten.
            XP::addXP(XP_DEFENSE_SPOOKED);
            Mood::onDefensiveCatch();
        } else if (s == WatchState::Sniffy) {
            Avatar::setState(AvatarState::EXCITED);
        } else {
            Avatar::setState(AvatarState::HUNTING);
        }
    }
}

void GuardHogMode::processSightings() {
    while (sightRead != sightWrite) {
        Sighting s;
        uint8_t r = sightRead;
        for (int i = 0; i < 6; ++i) s.addr[i] = sightRing[r].addr[i];
        s.rssi = sightRing[r].rssi;
        s.type = sightRing[r].type;
        s.lost = sightRing[r].lost;
        s.drone = sightRing[r].drone;
        s.flipper = sightRing[r].flipper;
        sightRead = (uint8_t)((r + 1) % kSightSlots);

        bool isTracker = (s.type != (uint8_t)trackerdet::TrackerType::None);
        if (isTracker) upsertTracker(s);
        updateFollower(s);

        // SKY HOGS: a drone announcing Remote ID nearby.
        if (s.drone && !seenContains(droneSeen, droneCount, s.addr) && droneCount < kMaxSeen) {
            memcpy(droneSeen[droneCount++], s.addr, 6);
            Display::showToast("SKY HOG!\nDRONE REMOTE ID");
            SFX::play(SFX::PIG_ALARM);
            ghAwardCatch();
        }
        // FLIPPER FINDER: another critter at the con.
        if (s.flipper && !seenContains(flipperSeen, flipperCount, s.addr) && flipperCount < kMaxSeen) {
            memcpy(flipperSeen[flipperCount++], s.addr, 6);
            char toast[40];
            snprintf(toast, sizeof(toast), "FLIPPER NEAR #%u", flipperCount);
            Display::showToast(toast);
            SFX::play(SFX::PIG_GRUNT);
            ghAwardCatch();
        }
    }
}

void GuardHogMode::upsertTracker(const Sighting& s) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < trackerCount; ++i) {
        if (memcmp(trackers[i].addr, s.addr, 6) == 0) {
            trackers[i].rssi = s.rssi;
            trackers[i].lost = s.lost;
            trackers[i].type = s.type;
            trackers[i].lastSeen = now;
            return;
        }
    }
    // New tracker.
    uint8_t slot;
    if (trackerCount < kMaxTrackers) {
        slot = trackerCount++;
    } else {
        // Replace the stalest.
        slot = 0;
        for (uint8_t i = 1; i < trackerCount; ++i)
            if (trackers[i].lastSeen < trackers[slot].lastSeen) slot = i;
    }
    memcpy(trackers[slot].addr, s.addr, 6);
    trackers[slot].rssi = s.rssi;
    trackers[slot].lost = s.lost;
    trackers[slot].type = s.type;
    trackers[slot].lastSeen = now;

    // Announce a freshly-seen tracker (soft grunt + toast); the loud alarm is
    // reserved for a confirmed !FOLLOWING (TAIL WAGGER).
    char toast[40];
    snprintf(toast, sizeof(toast), "%s SEEN %02X%02X",
             trackerdet::typeLabel((trackerdet::TrackerType)s.type), s.addr[4], s.addr[5]);
    Display::showToast(toast);
    SFX::play(SFX::PIG_GRUNT);
    logTrackerHit(s);
    ghAwardCatch();   // TICK CHECK: a freshly-seen tracker (deduped per MAC above)
}

void GuardHogMode::ageTrackers() {
    uint32_t now = millis();
    uint8_t w = 0;
    for (uint8_t i = 0; i < trackerCount; ++i) {
        if (now - trackers[i].lastSeen <= TRACKER_STALE_MS) {
            if (w != i) trackers[w] = trackers[i];
            ++w;
        }
    }
    trackerCount = w;
}

// TAIL WAGGER: has this MAC followed us across enough distinct waypoints?
void GuardHogMode::updateFollower(const Sighting& s) {
    uint32_t now = millis();
    GPSData g = GPS::getData();
    bool haveFix = GPS::hasFix();

    FollowEntry* e = nullptr;
    for (uint8_t i = 0; i < followerCount; ++i) {
        if (memcmp(followers[i].addr, s.addr, 6) == 0) { e = &followers[i]; break; }
    }
    if (!e) {
        uint8_t slot;
        if (followerCount < kMaxFollowers) {
            slot = followerCount++;
        } else {
            // Evict the least-recently-seen unflagged entry.
            slot = 0;
            for (uint8_t i = 1; i < followerCount; ++i)
                if (!followers[i].flagged && followers[i].lastMs < followers[slot].lastMs) slot = i;
        }
        e = &followers[slot];
        memcpy(e->addr, s.addr, 6);
        e->hits = 0;
        e->waypoints = 0;
        e->flagged = false;
        e->isTracker = false;
        e->lastWpLat = e->lastWpLon = 0.0;
        e->firstMs = now;
    }

    e->hits++;
    e->lastMs = now;
    if (s.type != (uint8_t)trackerdet::TrackerType::None) e->isTracker = true;

    // Count a distinct waypoint each time we've moved far enough since we last
    // logged one for this MAC. Needs GPS; without a fix TAIL WAGGER just lists.
    if (haveFix) {
        if (e->waypoints == 0) {
            e->waypoints = 1;
            e->lastWpLat = g.latitude;
            e->lastWpLon = g.longitude;
        } else {
            // equirectangular metres (same approx as flock_proximity)
            const double R = 6371000.0, D2R = 0.017453292519943295;
            double midLat = (e->lastWpLat + g.latitude) * 0.5 * D2R;
            double x = (g.longitude - e->lastWpLon) * D2R * cos(midLat);
            double y = (g.latitude - e->lastWpLat) * D2R;
            double dist = sqrt(x * x + y * y) * R;
            if (dist >= kWaypointMeters) {
                if (e->waypoints < 255) e->waypoints++;
                e->lastWpLat = g.latitude;
                e->lastWpLon = g.longitude;
            }
        }
    }

    // Flag once: persistent across waypoints, enough hits, over enough time.
    if (!e->flagged &&
        e->waypoints >= kFollowWaypoints &&
        e->hits >= kFollowHits &&
        (now - e->firstMs) >= kFollowMinMs) {
        e->flagged = true;
        if (followingFlagged < 255) followingFlagged++;
        Display::showToast("!FOLLOWING\nYOU'VE GOT A TAIL");
        SFX::play(SFX::PIG_ALARM);
        Avatar::setState(AvatarState::ANGRY);
        ghAwardCatch();   // TAIL WAGGER: confirmed follower (flagged once per MAC)
    }
}

void GuardHogMode::logTrackerHit(const Sighting& s) {
    if (!Config::isSDAvailable()) return;
    if (ghLogFile[0] == '\0') {
        const char* dir = SDLayout::logsDir();
        if (!SD.exists(dir)) { if (!SD.mkdir(dir)) return; }
        GPSData g = GPS::getData();
        if (g.date > 0 && g.time > 0) {
            uint8_t day=g.date/10000, mon=(g.date/100)%100, yr=g.date%100;
            uint8_t hh=g.time/1000000, mm=(g.time/10000)%100, ss=(g.time/100)%100;
            snprintf(ghLogFile, sizeof(ghLogFile), "%s/guardhog_20%02d%02d%02d_%02d%02d%02d.csv",
                     dir, yr,mon,day,hh,mm,ss);
        } else {
            snprintf(ghLogFile, sizeof(ghLogFile), "%s/guardhog_%lu.csv", dir, (unsigned long)millis());
        }
        File f = SD.open(ghLogFile, FILE_WRITE);
        if (!f) { ghLogFile[0] = '\0'; return; }
        f.print("ms,category,label,mac,rssi,lat,lon\n");
        f.close();
    }
    GPSData g = GPS::getData();
    char line[128];
    snprintf(line, sizeof(line), "%lu,tracker,%s,%02X:%02X:%02X:%02X:%02X:%02X,%d,%.6f,%.6f\n",
             (unsigned long)millis(), trackerdet::typeLabel((trackerdet::TrackerType)s.type),
             s.addr[0],s.addr[1],s.addr[2],s.addr[3],s.addr[4],s.addr[5],
             (int)s.rssi, g.latitude, g.longitude);
    File f = SD.open(ghLogFile, FILE_APPEND);
    if (f) { f.print(line); f.close(); }
}

// ============================================================================
// Watch face
// ============================================================================
void GuardHogMode::draw(M5Canvas& canvas) {
    const int W = canvas.width();
    canvas.setTextFont(1);
    canvas.setTextSize(1);

    // Title + which radio slice is live right now.
    canvas.setTextColor(TFT_WHITE);
    canvas.setCursor(4, 2);
    canvas.print("GUARD HOG");
    canvas.setTextColor(TFT_DARKGREY);
    canvas.setCursor(W - 40, 2);
    canvas.print(radioPhase == RadioPhase::BleSlice ? "[BLE]" : "[WiFi]");

    // Fused mood banner — the headline.
    uint16_t col; const char* word;
    switch (watch) {
        case WatchState::Spooked: col = TFT_RED;       word = "SPOOKED"; break;
        case WatchState::Sniffy:  col = TFT_ORANGE;    word = "SNIFFY";  break;
        default:                  col = TFT_DARKGREEN; word = "CALM";    break;
    }
    canvas.fillRect(0, 13, W, 20, col);
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextSize(2);
    canvas.setCursor(6, 15);
    canvas.print(word);
    canvas.setTextSize(1);
    int y = 36;

    if (followingFlagged > 0) {
        canvas.setTextColor(TFT_RED);
        canvas.setCursor(4, y);
        canvas.printf("!FOLLOWING x%u - YOU'VE GOT A TAIL", followingFlagged);
        y += 12;
    }

    // Signal tallies across both radios.
    canvas.setTextColor(TFT_CYAN);
    canvas.setCursor(4, y);
    canvas.printf("BLE  tick:%u sky:%u flip:%u", trackerCount, droneCount, flipperCount);
    y += 11;
    canvas.setTextColor(TFT_YELLOW);
    canvas.setCursor(4, y);
    canvas.printf("WiFi cam:%lu atk:%s twin:%u",
                  (unsigned long)NetworkRecon::getFlockAlertCount(),
                  NetworkRecon::getLastAttack() != attackdet::AttackType::None ? "Y" : "-",
                  NetworkRecon::getEvilTwinCount());
    y += 13;

    // A couple of the nearest trackers for context.
    uint8_t shown = 0;
    for (uint8_t i = 0; i < trackerCount && shown < 3; ++i, ++shown) {
        const TrackerEntry& t = trackers[i];
        canvas.setTextColor(t.lost ? TFT_ORANGE : TFT_GREENYELLOW);
        canvas.setCursor(6, y);
        canvas.printf("%-13s %4d ..%02X%02X%s",
                      trackerdet::typeLabel((trackerdet::TrackerType)t.type),
                      (int)t.rssi, t.addr[4], t.addr[5], t.lost ? " LOST" : "");
        y += 10;
    }

    if (watch == WatchState::Calm) {
        canvas.setTextColor(TFT_DARKGREY);
        canvas.setCursor(6, y + 2);
        canvas.print("no ticks on you. good.");
    }

    // LIFE + XP readout, pinned to the bottom so the economy is visible: idle
    // guarding holds LIFE ~flat, a catch bumps LIFE + the XP bar.
    int life = Mood::getEffectiveHappiness();
    canvas.setTextColor(life >= 0 ? TFT_GREEN : TFT_RED);
    canvas.setCursor(4, canvas.height() - 10);
    canvas.printf("LIFE %+d", life);
    canvas.setTextColor(TFT_WHITE);
    canvas.setCursor(canvas.width() - 92, canvas.height() - 10);
    canvas.printf("XP L%u %u%%", XP::getLevel(), XP::getProgress());
}
