// flock_signatures.h
// ---------------------------------------------------------------------------
// Signature data tables for passive surveillance-device detection.
//
// This is a clean-room, original implementation written for M5PORKCHOP (MIT).
// The *signature data* below (OUIs, BLE manufacturer IDs, service UUIDs) is
// crowdsourced / research-derived fact, not copied source. Attribution:
//   - OUI list & addr1 receiver technique .... @NitekryDPaul, DeFlockJoplin
//   - 31st OUI (82:6B:F2) ...................... DeFlockJoplin
//   - BLE manufacturer ID 0x09C8 (XUNTONG) .... @wgreenberg's research
//   - Raven BLE service UUIDs .................. deflock.me / community datasets
//   - Crowdsourced camera locations ........... deflock.me (FoggedLens/deflock)
//
// The OUI table below is the canonical field-tested 31-prefix set (30 from
// @NitekryDPaul's research + 82:6B:F2 from DeFlockJoplin), as published in
// colonelpanichacks/flock-you (oui.txt / datasets/NitekryDPaul_wifi_ouis.md).
// Field results for that list: 11/12 cameras detected, 2 false positives.
// Generic Espressif vendor blocks are deliberately NOT included -- they match
// any ESP32 (including the Cardputer itself) and are false-positive machines.
//
// NOTE: the Raven service-UUID values still need to come from the authoritative
// deflock.me dataset once its license permits redistribution.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

namespace flockdet {

// How trustworthy a given OUI is on its own.
//   GENERIC_ESP  -> matches ANY Espressif device (incl. the Cardputer itself!).
//                   Weak signal. Must be corroborated before alerting.
//   FLOCK_LINKED -> OUI observed specifically in Flock deployments and not a
//                   generic vendor block. Stronger signal.
enum class OuiClass : uint8_t {
    GENERIC_ESP  = 0,
    FLOCK_LINKED = 1,
};

struct OuiEntry {
    uint8_t  oui[3];      // first three bytes of the MAC (big-endian, as on air)
    OuiClass cls;
    const char* note;
};

// The canonical 31. Every prefix here is Flock-specific enough to stand on its
// own, so all are FLOCK_LINKED: an OUI match alone fires at Medium, and a
// correlated wildcard probe request lifts it to High -- see the scorer.
static const OuiEntry kFlockOuis[] = {
    { {0x70, 0xC9, 0x4E}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x3C, 0x91, 0x80}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xD8, 0xF3, 0xBC}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x80, 0x30, 0x49}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xB8, 0x35, 0x32}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x14, 0x5A, 0xFC}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x74, 0x4C, 0xA1}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x08, 0x3A, 0x88}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x9C, 0x2F, 0x9D}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xC0, 0x35, 0x32}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x94, 0x08, 0x53}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xE4, 0xAA, 0xEA}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xF4, 0x6A, 0xDD}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xF8, 0xA2, 0xD6}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x24, 0xB2, 0xB9}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x00, 0xF4, 0x8D}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xD0, 0x39, 0x57}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xE8, 0xD0, 0xFC}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xE0, 0x4F, 0x43}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xB8, 0x1E, 0xA4}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x70, 0x08, 0x94}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x58, 0x8E, 0x81}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xEC, 0x1B, 0xBD}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x3C, 0x71, 0xBF}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x58, 0x00, 0xE3}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x90, 0x35, 0xEA}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x5C, 0x93, 0xA2}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x64, 0x6E, 0x69}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x48, 0x27, 0xEA}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0xA4, 0xCF, 0x12}, OuiClass::FLOCK_LINKED, "NitekryDPaul field list" },
    { {0x82, 0x6B, 0xF2}, OuiClass::FLOCK_LINKED, "DeFlockJoplin 31st OUI"  },
};
static const uint8_t kFlockOuiCount = sizeof(kFlockOuis) / sizeof(kFlockOuis[0]);

// ---- BLE signatures -------------------------------------------------------

// Manufacturer-specific data company identifier (little-endian on air).
// 0x09C8 == XUNTONG, observed in Flock/Raven BLE advertisements.
static const uint16_t kFlockBleCompanyId = 0x09C8;

// BLE advertised-name substrings worth flagging (case-insensitive match).
static const char* const kFlockBleNameHints[] = {
    "flock",
    "raven",
    "falcon",   // common Flock camera model family
};
static const uint8_t kFlockBleNameHintCount =
    sizeof(kFlockBleNameHints) / sizeof(kFlockBleNameHints[0]);

// Raven (SoundThinking/ShotSpotter) BLE service UUIDs.
// Populate the 128-bit values from the community raven_configurations.json
// (fw 1.1.7 / 1.2.0 / 1.3.1). Left empty here rather than fabricated.
struct BleServiceUuid128 { uint8_t bytes[16]; const char* note; };
static const BleServiceUuid128 kRavenServiceUuids[] = {
    // { { /* 16 bytes, MSB..LSB */ }, "Raven fw1.3.1 svc" },
};
static const uint8_t kRavenServiceUuidCount =
    sizeof(kRavenServiceUuids) / sizeof(kRavenServiceUuids[0]);

} // namespace flockdet
