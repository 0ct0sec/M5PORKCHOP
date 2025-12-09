#!/usr/bin/env python3
"""
Prepare ML training data for Edge Impulse.

This script:
1. Reads raw Porkchop ml_training.csv data
2. Labels samples based on security characteristics
3. Converts to Edge Impulse compatible format

Usage:
    python scripts/prepare_ml_data.py <input.csv> [output.csv]
    
    If output not specified, creates <input>_ei.csv

Labels assigned:
    - normal: Standard ISP routers, secure configs
    - vulnerable: Open networks, WEP, WPS enabled
    - deauth_target: No WPA3/PMF (can be deauthed)
    - rogue_ap: Strong signal + suspicious characteristics
    - evil_twin: (requires manual labeling)
"""

import csv
import sys
from pathlib import Path

# ISP/Known router SSID patterns (likely legitimate)
ISP_PATTERNS = [
    "PLAY", "Orange", "T-Mobile", "Plus", "UPC", "Vectra", "Netia",
    "NETIASPOT", "Livebox", "FunBox", "Connect Box", "pxs.pl",
    "TP-Link", "ASUS", "Linksys", "NETGEAR", "Xiaomi", "Huawei",
    "Fritz", "Vodafone", "Comcast", "Xfinity", "Spectrum", "ATT",
    "Verizon", "CenturyLink", "BT-", "Sky-", "Virgin", "EE-",
]

# Labels (must match MLLabel enum in inference.h)
LABELS = {
    0: "normal",
    1: "rogue_ap",
    2: "evil_twin",
    3: "deauth_target",
    4: "vulnerable"
}

# Feature columns to export (numeric only)
FEATURE_COLUMNS = [
    "rssi", "noise", "snr", "channel", "secondary_ch",
    "beacon_interval", "capability_lo", "capability_hi",
    "has_wps", "has_wpa", "has_wpa2", "has_wpa3", "is_hidden",
    "response_time", "beacon_count", "beacon_jitter",
    "responds_probe", "probe_response_time", "vendor_ie_count",
    "supported_rates", "ht_cap", "vht_cap", "anomaly_score",
    "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"
]


def is_known_isp(ssid: str) -> bool:
    """Check if SSID matches known ISP/router patterns."""
    if not ssid:
        return False
    ssid_upper = ssid.upper()
    return any(pattern.upper() in ssid_upper for pattern in ISP_PATTERNS)


def label_sample(row: dict) -> str:
    """Assign label based on network characteristics."""
    ssid = row.get('ssid', '')
    
    # Parse numeric fields with defaults
    def get_float(key, default=0.0):
        try:
            return float(row.get(key, default))
        except (ValueError, TypeError):
            return default
    
    rssi = get_float('rssi', -100)
    has_wps = get_float('has_wps') > 0
    has_wpa = get_float('has_wpa') > 0
    has_wpa2 = get_float('has_wpa2') > 0
    has_wpa3 = get_float('has_wpa3') > 0
    is_hidden = get_float('is_hidden') > 0
    beacon_interval = get_float('beacon_interval', 100)
    
    # Check if open network (no WPA/WPA2/WPA3)
    is_open = not (has_wpa or has_wpa2 or has_wpa3)
    
    # VULNERABLE: Open networks, WEP, or WPS enabled
    if is_open:
        return "vulnerable"
    if has_wps:
        return "vulnerable"
    
    # ROGUE_AP: Suspiciously strong signal + anomalies
    if rssi > -30:
        if is_hidden or beacon_interval < 50 or beacon_interval > 200:
            return "rogue_ap"
    
    # DEAUTH_TARGET: No WPA3 (vulnerable to deauth attacks)
    if not has_wpa3:
        return "deauth_target"
    
    # NORMAL: Known ISP or standard secure config
    return "normal"


def prepare_data(input_path: str, output_path: str):
    """Read, deduplicate, label, and convert data for Edge Impulse."""
    
    with open(input_path, 'r', newline='', encoding='utf-8') as infile:
        reader = csv.DictReader(infile)
        
        # Check which feature columns exist
        available = [c for c in FEATURE_COLUMNS if c in reader.fieldnames]
        missing = [c for c in FEATURE_COLUMNS if c not in reader.fieldnames]
        
        if missing:
            print(f"Note: {len(missing)} expected columns not in CSV (using 0.0)")
        
        rows = []
        seen_bssids = set()  # Track unique BSSIDs for deduplication
        duplicates_removed = 0
        label_counts = {name: 0 for name in LABELS.values()}
        
        for row in reader:
            # Deduplicate by BSSID (keep first occurrence with best RSSI)
            bssid = row.get('bssid', '').upper().strip()
            if bssid:
                if bssid in seen_bssids:
                    duplicates_removed += 1
                    continue
                seen_bssids.add(bssid)
            
            # Assign label
            label = label_sample(row)
            label_counts[label] += 1
            
            # Build output row with features
            out_row = {}
            for col in FEATURE_COLUMNS:
                try:
                    out_row[col] = float(row.get(col, 0))
                except (ValueError, TypeError):
                    out_row[col] = 0.0
            
            out_row['label'] = label
            rows.append(out_row)
    
    # Write Edge Impulse format
    with open(output_path, 'w', newline='', encoding='utf-8') as outfile:
        fieldnames = FEATURE_COLUMNS + ['label']
        writer = csv.DictWriter(outfile, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    
    # Summary
    print(f"Input:  {input_path}")
    print(f"Output: {output_path}")
    print(f"Total rows: {len(rows) + duplicates_removed}")
    print(f"Duplicates removed: {duplicates_removed}")
    print(f"Unique samples: {len(rows)}")
    print(f"Features: {len(FEATURE_COLUMNS)}")
    print(f"\nLabel distribution:")
    for label, count in sorted(label_counts.items(), key=lambda x: -x[1]):
        if count > 0:
            pct = count / len(rows) * 100
            print(f"  {label}: {count} ({pct:.1f}%)")


def main():
    # Parse arguments
    if len(sys.argv) < 2:
        # Default: look for ml_training.csv in project root
        script_dir = Path(__file__).parent
        project_dir = script_dir.parent
        input_path = project_dir / "ml_training.csv"
        if not input_path.exists():
            print("Usage: python prepare_ml_data.py <input.csv> [output.csv]")
            print("\nNo input file specified and ml_training.csv not found.")
            sys.exit(1)
    else:
        input_path = Path(sys.argv[1])
    
    if not input_path.exists():
        print(f"Error: Input file not found: {input_path}")
        sys.exit(1)
    
    # Output path
    if len(sys.argv) >= 3:
        output_path = Path(sys.argv[2])
    else:
        output_path = input_path.with_stem(input_path.stem + "_ei")
    
    prepare_data(str(input_path), str(output_path))
    print(f"\nReady for Edge Impulse upload!")


if __name__ == "__main__":
    main()
