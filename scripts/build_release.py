#!/usr/bin/env python3
"""
==============================================================================
                         PORKCHOP RELEASE BUILD SCRIPT
==============================================================================

    --[ 0x00 - What This Does

        Builds both release binaries for distribution:
        
        1. firmware_vX.X.X.bin       - Flash at 0x10000 via esptool-js
                                       (preserves XP, for upgrades)
        
        2. porkchop_vX.X.X_m5burner.bin - Flash at 0x0 via M5 Burner
                                          (full image, nukes NVS)

        Output lands in m5porkchop_builds/ directory.


    --[ 0x01 - Usage

        python scripts/build_release.py

        That's it. No args. Version pulled from platformio.ini.
        Go make coffee, come back to fresh binaries.


    --[ 0x02 - Requirements

        * PlatformIO CLI (pio) in PATH
        * That's literally it


==[EOF]==
"""

import os
import sys
import subprocess
import shutil
import re
from pathlib import Path

# Phrack-style banner
BANNER = r"""
 ██▓███   ▒█████   ██▀███   ██ ▄█▀ ▄████▄   ██░ ██  ▒█████   ██▓███  
▓██░  ██▒▒██▒  ██▒▓██ ▒ ██▒ ██▄█▒ ▒██▀ ▀█  ▓██░ ██▒▒██▒  ██▒▓██░  ██▒
▓██░ ██▓▒▒██░  ██▒▓██ ░▄█ ▒▓███▄░ ▒▓█    ▄ ▒██▀▀██░▒██░  ██▒▓██░ ██▓▒
▒██▄█▓▒ ▒▒██   ██░▒██▀▀█▄  ▓██ █▄ ▒▓▓▄ ▄██▒░▓█ ░██ ▒██   ██░▒██▄█▓▒ ▒
▒██▒ ░  ░░ ████▓▒░░██▓ ▒██▒▒██▒ █▄▒ ▓███▀ ░░▓█▒░██▓░ ████▓▒░▒██▒ ░  ░
▒▓▒░ ░  ░░ ▒░▒░▒░ ░ ▒▓ ░▒▓░▒ ▒▒ ▓▒░ ░▒ ▒  ░ ▒ ░░▒░▒░ ▒░▒░▒░ ▒▓▒░ ░  ░
                    RELEASE BUILD SCRIPT - OINK!
"""


def log(msg, prefix="[*]"):
    """Print with prefix"""
    print(f"{prefix} {msg}")


def log_ok(msg):
    log(msg, "[+]")


def log_err(msg):
    log(msg, "[!]")


def log_info(msg):
    log(msg, "[>]")


def get_version():
    """Extract version from platformio.ini"""
    ini_path = Path(__file__).parent.parent / "platformio.ini"
    
    if not ini_path.exists():
        log_err("platformio.ini not found - are you in the right directory?")
        sys.exit(1)
    
    with open(ini_path, "r") as f:
        content = f.read()
    
    # Look for custom_version = X.X.X
    match = re.search(r'custom_version\s*=\s*(\d+\.\d+\.\d+)', content)
    if match:
        return match.group(1)
    
    log_err("Could not find custom_version in platformio.ini")
    sys.exit(1)


def run_cmd(cmd, description):
    """Run a command and handle errors"""
    log_info(f"{description}...")
    
    try:
        result = subprocess.run(
            cmd,
            shell=True,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            log_err(f"Command failed: {cmd}")
            log_err(result.stderr)
            sys.exit(1)
        
        return result.stdout
    except Exception as e:
        log_err(f"Exception running command: {e}")
        sys.exit(1)


def main():
    print(BANNER)
    
    # Get project root (parent of scripts/)
    project_root = Path(__file__).parent.parent
    os.chdir(project_root)
    
    log_info(f"Working directory: {project_root}")
    
    # Get version
    version = get_version()
    log_ok(f"Building version: {version}")
    
    # Create output directory
    builds_dir = project_root / "m5porkchop_builds"
    builds_dir.mkdir(exist_ok=True)
    log_ok(f"Output directory: {builds_dir}")
    
    # Build paths
    build_dir = project_root / ".pio" / "build" / "m5cardputer"
    firmware_src = build_dir / "firmware.bin"
    bootloader_src = build_dir / "bootloader.bin"
    partitions_src = build_dir / "partitions.bin"
    
    # Output filenames
    firmware_dst = builds_dir / f"firmware_v{version}.bin"
    m5burner_dst = builds_dir / f"porkchop_v{version}_m5burner.bin"
    
    # Step 1: Build with PlatformIO
    log("")
    log("=" * 60)
    log("STEP 1: Building firmware with PlatformIO")
    log("=" * 60)
    
    run_cmd("pio run -e m5cardputer", "Compiling")
    log_ok("Firmware compiled successfully")
    
    # Verify build artifacts exist
    if not firmware_src.exists():
        log_err(f"firmware.bin not found at {firmware_src}")
        sys.exit(1)
    
    if not bootloader_src.exists():
        log_err(f"bootloader.bin not found at {bootloader_src}")
        sys.exit(1)
    
    # Step 2: Copy firmware.bin for esptool-js upgrades
    log("")
    log("=" * 60)
    log("STEP 2: Copying firmware for esptool-js upgrades")
    log("=" * 60)
    
    shutil.copy2(firmware_src, firmware_dst)
    size_kb = firmware_dst.stat().st_size / 1024
    log_ok(f"Created: {firmware_dst.name} ({size_kb:.1f} KB)")
    
    # Step 3: Create merged binary for M5 Burner
    log("")
    log("=" * 60)
    log("STEP 3: Creating merged binary for M5 Burner")
    log("=" * 60)
    
    # Build the esptool merge command
    merge_cmd = (
        f'pio pkg exec -p tool-esptoolpy -- esptool.py '
        f'--chip esp32s3 merge_bin '
        f'-o "{m5burner_dst}" '
        f'--flash_mode dio --flash_size 8MB '
        f'0x0 "{bootloader_src}" '
        f'0x8000 "{partitions_src}" '
        f'0x10000 "{firmware_src}"'
    )
    
    run_cmd(merge_cmd, "Merging binaries")
    
    size_kb = m5burner_dst.stat().st_size / 1024
    log_ok(f"Created: {m5burner_dst.name} ({size_kb:.1f} KB)")
    
    # Summary
    log("")
    log("=" * 60)
    log("BUILD COMPLETE - OINK!")
    log("=" * 60)
    log("")
    log_ok(f"Release binaries for v{version}:")
    log("")
    log(f"    {firmware_dst.name}")
    log(f"        -> Flash at 0x10000 via esptool-js (preserves XP)")
    log("")
    log(f"    {m5burner_dst.name}")
    log(f"        -> Flash at 0x0 via M5 Burner (full install)")
    log("")
    log_info(f"Files are in: {builds_dir}")
    log("")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
