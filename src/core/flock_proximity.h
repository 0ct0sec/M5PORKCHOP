// flock_proximity.h
// ---------------------------------------------------------------------------
// GPS proximity warning against the known-ALPR map in flock_map_data.h.
//
// This is the *map* half of the Flock feature and is completely independent of
// the radio: it never transmits, never scans, and keeps working while the WiFi
// hardware is busy doing something else. It answers "how far am I from the
// nearest camera anyone has reported?" and ramps an audible geiger-counter tick
// as that distance closes.
//
// Map data (c) OpenStreetMap contributors, ODbL -- see flock_map_data.h.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

namespace flockprox {

// Reset distance + alert state. Call when entering a mode that drives update().
void init();

// Feed the current fix. Safe (and cheap) to call every loop; the map scan is
// internally rate-limited and the beep ramp is millis()-gated.
// A false `valid` silences everything and re-arms the "ALPR AHEAD" toast.
void update(double lat, double lon, bool valid);

// Metres to the nearest known camera, or -1 with no fix / nothing in range.
float nearestMeters();

} // namespace flockprox
