// GUARD HOG - the DEFENSIVE SUITE watch face ("am I being watched?")
// ---------------------------------------------------------------------------
// Always-passive counter-surveillance mode. Built up across the phased
// DEFENSIVE SUITE rollout:
//   P1  BLE tracker scan (TICK CHECK) + GPS persistence (TAIL WAGGER)
//   P4  drone Remote ID (SKY HOGS) + Flipper (FLIPPER FINDER) share the BLE scan
//   P5  full CALM/SNIFFY/SPOOKED fusion + WiFi/BLE radio rotation
//
// RX-only. Owns the radio while active: it stops NetworkRecon, runs a NimBLE
// passive scan, and (from P5) time-slices in WiFi like the WARHOG rotation.
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include "../defense/tracker_detect.h"
#include "../defense/drone_detect.h"
#include "../defense/flipper_detect.h"

// Fused "am I being watched?" score. SPOOKED only when two independent signals
// (ideally across both radios) agree — no crying wolf.
enum class WatchState : uint8_t { Calm = 0, Sniffy, Spooked };

class GuardHogMode {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isRunning() { return running; }
    static WatchState watchState() { return watch; }

    // Deferred BLE sighting handed over from the scan callback (callback-safe).
    // `addr` is in human display order (addr[0] = first printed octet).
    static void enqueueSighting(const uint8_t* addr, int8_t rssi,
                                uint8_t trackerType, bool lost,
                                bool drone, bool flipper);

private:
    static bool running;

    // ---- TICK CHECK: currently-visible trackers -----------------------------
    static const uint8_t kMaxTrackers = 12;
    struct TrackerEntry {
        uint8_t  addr[6];
        uint8_t  type;        // trackerdet::TrackerType
        int8_t   rssi;
        bool     lost;
        uint32_t lastSeen;
    };
    static TrackerEntry trackers[kMaxTrackers];
    static uint8_t trackerCount;

    // ---- TAIL WAGGER: cross-waypoint persistence engine ---------------------
    // A BLE MAC that keeps showing up as we move between GPS waypoints is a
    // tail. Bounded fixed table with LRU eviction (no PSRAM on this board).
    static const uint8_t  kMaxFollowers = 32;
    static const uint16_t kWaypointMeters = 120;    // min move to count a new waypoint
    static const uint8_t  kFollowWaypoints = 3;     // distinct waypoints to flag
    static const uint8_t  kFollowHits = 3;          // min total sightings
    static const uint32_t kFollowMinMs = 5UL * 60UL * 1000UL;  // over >=5 min
    struct FollowEntry {
        uint8_t  addr[6];
        uint16_t hits;
        uint8_t  waypoints;
        bool     flagged;
        bool     isTracker;
        double   lastWpLat, lastWpLon;
        uint32_t firstMs, lastMs;
    };
    static FollowEntry followers[kMaxFollowers];
    static uint8_t followerCount;
    static uint8_t followingFlagged;   // count of currently-flagged followers

    // ---- SKY HOGS / FLIPPER FINDER: distinct-MAC tallies --------------------
    static const uint8_t kMaxSeen = 8;
    static uint8_t droneSeen[kMaxSeen][6];
    static uint8_t droneCount;
    static uint8_t flipperSeen[kMaxSeen][6];
    static uint8_t flipperCount;

    // ---- deferred sighting ring (scan callback -> update) -------------------
    static const uint8_t kSightSlots = 24;
    struct Sighting {
        uint8_t addr[6]; int8_t rssi; uint8_t type; bool lost; bool drone; bool flipper;
    };
    static volatile Sighting sightRing[kSightSlots];
    static volatile uint8_t sightWrite, sightRead;

    static bool seenContains(const uint8_t seen[][6], uint8_t n, const uint8_t* mac);

    // ---- Eye-Spy radio rotation (P5) ---------------------------------------
    // One 2.4GHz radio, time-shared. Only ONE stack's controller is ever up at a
    // time -- this is mandatory: bringing WiFi up while the BT controller is
    // still enabled aborts in coex_enable() (confirmed by coredump). So each
    // handoff fully tears the other radio down:
    //   BLE slice  : NimBLE inited + passive scan, WiFi/NetworkRecon stopped.
    //   WiFi slice : NimBLE DEINITED, NetworkRecon::start() owns WiFi promiscuous
    //                (its callback feeds flock/attack/evil-twin; the global
    //                serviceFlockAlerts drains + alerts). NetworkRecon::start()
    //                is the proven coex-safe bring-up (it deinits BLE itself).
    // No TX in either slice (passive scan / passive promiscuous).
    enum class RadioPhase : uint8_t { BleSlice = 0, WifiSlice };
    static const uint32_t BLE_SLICE_MS  = 9000;
    static const uint32_t WIFI_SLICE_MS = 8000;
    static RadioPhase radioPhase;
    static uint32_t   phaseStartMs;
    static void enterBleSlice();
    static void enterWifiSlice();

    // ---- fused score --------------------------------------------------------
    static WatchState watch;
    static void computeWatch();

    // BLE lifecycle
    static bool bleStarted;
    static void startBleScan();
    static void stopBleScan();

    // engines
    static void processSightings();
    static void upsertTracker(const Sighting& s);
    static void ageTrackers();
    static void updateFollower(const Sighting& s);
    static void logTrackerHit(const Sighting& s);
};
