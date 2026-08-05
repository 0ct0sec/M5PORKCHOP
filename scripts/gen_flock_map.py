#!/usr/bin/env python3
"""Generate src/core/flock_map_data.h from a mapdata/*.csv of ALPR locations.

The CSV comes from fetch_flockmap.py (OpenStreetMap / Overpass, ODbL).
Columns: lat,lon,operator,direction  (header row required).

Usage:
    python scripts/gen_flock_map.py [csv_path] [header_path]

Coordinates are stored as int32 degrees * 1e7 (~1.1 cm resolution), which keeps
the table at 8 bytes per point instead of 16 for doubles -- the Cardputer has no
PSRAM, so this lives in flash .rodata and is scanned in place.
"""
import csv
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV_PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    REPO, 'mapdata', 'flock_map_huntsville.csv')
OUT_PATH = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    REPO, 'src', 'core', 'flock_map_data.h')

points = []
with open(CSV_PATH, newline='', encoding='utf-8') as f:
    for row in csv.DictReader(f):
        try:
            lat, lon = float(row['lat']), float(row['lon'])
        except (KeyError, TypeError, ValueError):
            continue
        if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
            continue
        points.append((round(lat * 1e7), round(lon * 1e7)))

# Deterministic order + de-dup: identical nodes appear when OSM has stacked
# cameras on one pole, and they cost flash without adding coverage.
points = sorted(set(points))

lines = [
    '// flock_map_data.h -- GENERATED, DO NOT EDIT BY HAND',
    '// ---------------------------------------------------------------------------',
    '// Known ALPR / Flock camera locations, used by flock_proximity for the GPS',
    '// "you are approaching a camera" ramp. This is map data, NOT a detection',
    '// signature -- it says where cameras have been reported, not what is on air.',
    '//',
    '// Source: OpenStreetMap via the Overpass API (nodes tagged',
    '// surveillance:type=ALPR), fetched by fetch_flockmap.py and converted by',
    '// scripts/gen_flock_map.py.',
    '//',
    '// (c) OpenStreetMap contributors, licensed under the Open Database License',
    '// (ODbL) v1.0 -- https://www.openstreetmap.org/copyright. Redistribution of',
    '// this derived table must keep this attribution and stay ODbL.',
    '//',
    '// Camera reporting/crowdsourcing: deflock.me',
    '// ---------------------------------------------------------------------------',
    '#pragma once',
    '#include <stdint.h>',
    '',
    'namespace flockprox {',
    '',
    '// Degrees * 1e7, so the whole table is 8 bytes/point in flash.',
    'struct MapPoint {',
    '    int32_t lat_e7;',
    '    int32_t lon_e7;',
    '};',
    '',
    'static const uint32_t kFlockMapCount = %d;' % len(points),
    '',
    'static const MapPoint kFlockMap[kFlockMapCount] = {',
]

# Four points per line keeps the file readable without exploding line count.
for i in range(0, len(points), 4):
    chunk = points[i:i + 4]
    lines.append('    ' + ' '.join('{%d,%d},' % p for p in chunk))

lines += [
    '};',
    '',
    '} // namespace flockprox',
    '',
]

with open(OUT_PATH, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines))

print('%s: %d points (%d bytes of flash)' % (OUT_PATH, len(points), len(points) * 8))
