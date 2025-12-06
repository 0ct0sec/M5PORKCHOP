```
 ██▓███   ▒█████   ██▀███   ██ ▄█▀ ▄████▄   ██░ ██  ▒█████   ██▓███  
▓██░  ██▒▒██▒  ██▒▓██ ▒ ██▒ ██▄█▒ ▒██▀ ▀█  ▓██░ ██▒▒██▒  ██▒▓██░  ██▒
▓██░ ██▓▒▒██░  ██▒▓██ ░▄█ ▒▓███▄░ ▒▓█    ▄ ▒██▀▀██░▒██░  ██▒▓██░ ██▓▒
▒██▄█▓▒ ▒▒██   ██░▒██▀▀█▄  ▓██ █▄ ▒▓▓▄ ▄██▒░▓█ ░██ ▒██   ██░▒██▄█▓▒ ▒
▒██▒ ░  ░░ ████▓▒░░██▓ ▒██▒▒██▒ █▄▒ ▓███▀ ░░▓█▒░██▓░ ████▓▒░▒██▒ ░  ░
▒▓▒░ ░  ░░ ▒░▒░▒░ ░ ▒▓ ░▒▓░▒ ▒▒ ▓▒░ ░▒ ▒  ░ ▒ ░░▒░▒░ ▒░▒░▒░ ▒▓▒░ ░  ░
░▒ ░       ░ ▒ ▒░   ░▒ ░ ▒░░ ░▒ ▒░  ░  ▒    ▒ ░▒░ ░  ░ ▒ ▒░ ░▒ ░     
░░       ░ ░ ░ ▒    ░░   ░ ░ ░░ ░ ░         ░  ░░ ░░ ░ ░ ▒  ░░       
             ░ ░     ░     ░  ░   ░ ░       ░  ░  ░    ░ ░           
                                  ░                                  
```

--[ Contents

    1 - Introduction
    2 - What the hell is this thing
    3 - Capabilities
        3.1 - OINK Mode
        3.2 - WARHOG Mode
        3.3 - Machine Learning
    4 - Hardware
    5 - Building & Flashing
    6 - Controls
    7 - Configuration
    8 - ML Training Pipeline
    9 - Code Structure
    10 - Legal sh*t
    11 - Greetz


--[ 1 - Introduction

    Listen up. You're looking at PORKCHOP - a pocket-sized WiFi hunting
    companion that lives in your M5Cardputer. Think pwnagotchi had a baby
    with a tamagotchi, except this one oinks and has zero chill when it
    catches a handshake.
    
    The piglet personality isn't just for show. It reacts to what you're
    doing - gets hyped when you pop a 4-way, goes full WARHOG when you're
    driving around mapping networks, and gets sleepy when nothing's
    happening. Feed it handshakes and it'll love you forever.


--[ 2 - What the hell is this thing

    PORKCHOP is built on the ESP32-S3 platform running on M5Cardputer
    hardware. It's designed for:

        - Passive WiFi reconnaissance
        - WPA/WPA2 handshake capture
        - GPS-enabled wardriving
        - ML-powered rogue AP detection
        - Looking cute while doing questionable things

    Your digital companion reacts to discoveries like any good attack pet
    should. Captures make it happy. Boredom makes it sad. It's basically
    you, but as an ASCII pig.


--[ 3 - Capabilities


----[ 3.1 - OINK Mode

    The bread and butter. Press 'O' and let the piglet loose:

        * Channel hopping across all 802.11 channels
        * Promiscuous mode packet capture  
        * EAPOL frame detection and 4-way handshake reconstruction
        * Deauth capability for... "authorized testing purposes"
        * Real-time ML classification of suspicious APs
        * Auto-attack mode cycles through targets automatically
        * Targeted deauth prioritizes discovered clients
        * PCAP export to SD for post-processing


----[ 3.2 - WARHOG Mode

    GPS + WiFi = tactical mapping. Hook up an AT6668 and go mobile:

        * Real-time GPS coordinate display on bottom bar
        * Automatic network discovery and logging
        * Memory-safe design (auto-saves at 2000 entries)
        * Feature extraction for ML training
        * Multiple export formats:
            - CSV: Simple, spreadsheet-ready
            - Wigle: Upload your wardriving data to wigle.net
            - Kismet NetXML: For your Kismet workflow
            - ML Training: 32-feature vectors for model training


----[ 3.3 - Machine Learning

    PORKCHOP doesn't just capture - it thinks. The ML system extracts
    32 features from every beacon frame:

        * Signal characteristics (RSSI, noise patterns)
        * Beacon timing analysis (interval, jitter)
        * Vendor IE fingerprinting
        * Security configuration analysis
        * Historical behavior patterns

    Built-in heuristic classifier detects:

        [!] ROGUE_AP    - Strong signal + abnormal timing + missing IEs
        [!] EVIL_TWIN   - Hidden SSID + suspiciously strong signal
        [!] VULNERABLE  - Open/WEP/WPA1-only/WPS enabled
        [!] DEAUTH_TGT  - No WPA3 or PMF = free real estate

    Want real ML? Train your own model on Edge Impulse and drop it in.
    The scaffold is ready.


--[ 4 - Hardware

    Required:
        * M5Cardputer (ESP32-S3 based)
        * MicroSD card for data storage

    Optional:
        * AT6668 GPS Module (WARHOG mode)
        * Questionable ethics


--[ 5 - Building & Flashing

    We use PlatformIO because we're not savages.

        # Install if you haven't
        $ pip install platformio

        # Build it
        $ pio run -e m5cardputer

        # Flash it
        $ pio run -t upload -e m5cardputer

        # Watch it work
        $ pio device monitor

    If it doesn't compile, skill issue. Check your dependencies.


--[ 6 - Controls

    The M5Cardputer's keyboard is tiny but functional:

        +-------+----------------------------------+
        | Key   | What it does                     |
        +-------+----------------------------------+
        | O     | Enter OINK mode (hunting)        |
        | W     | Enter WARHOG mode (wardriving)   |
        | S     | Settings menu                    |
        | `     | Toggle menu / Go back            |
        | ;     | Navigate up / Decrease value     |
        | .     | Navigate down / Increase value   |
        | Enter | Select / Toggle / Confirm        |
        +-------+----------------------------------+


--[ 7 - Configuration

    Settings persist to SPIFFS. Your piglet remembers.

        +------------+-------------------------------+---------+
        | Setting    | Description                   | Default |
        +------------+-------------------------------+---------+
        | Sound      | Beeps when things happen      | ON      |
        | Brightness | Display brightness            | 80%     |
        | CH Hop     | Channel hop interval (ms)     | 500     |
        | Scan Time  | Per-channel scan duration     | 2000    |
        | Deauth     | Enable deauth attacks         | ON      |
        | GPS        | Enable GPS module             | ON      |
        | GPS PwrSave| Power saving for GPS          | ON      |
        +------------+-------------------------------+---------+


--[ 8 - ML Training Pipeline

    Want to train your own model? Here's the workflow:

    Step 1: Collect data
        - Run WARHOG mode in various environments
        - Let it build up a nice dataset
        - Export ML training data to SD card

    Step 2: Label your data
        - 0 = unknown (unlabeled)
        - 1 = normal (legitimate APs)
        - 2 = rogue_ap (suspicious)
        - 3 = evil_twin (impersonating)
        - 4 = vulnerable (weak security)

    Step 3: Train on Edge Impulse
        - Create project at studio.edgeimpulse.com
        - Upload your labeled CSV
        - Design impulse: Raw data -> Neural Network
        - Train, test, iterate

    Step 4: Deploy
        - Export as "C++ Library" for ESP32
        - Drop edge-impulse-sdk/ into lib/
        - Uncomment EDGE_IMPULSE_ENABLED
        - Rebuild and flash

    Now your piglet has a real brain.


--[ 9 - Code Structure

    porkchop/
    |
    +-- src/
    |   +-- main.cpp              # Entry point, main loop
    |   +-- core/
    |   |   +-- porkchop.cpp/h    # State machine, mode management
    |   |   +-- config.cpp/h      # Configuration (SPIFFS persistence)
    |   |
    |   +-- ui/
    |   |   +-- display.cpp/h     # Triple-canvas display system
    |   |   +-- menu.cpp/h        # Main menu with callbacks
    |   |   +-- settings_menu.cpp/h   # Interactive settings
    |   |   +-- captures_menu.cpp/h   # Browse captured handshakes
    |   |
    |   +-- piglet/
    |   |   +-- avatar.cpp/h      # Derpy ASCII pig (flips L/R)
    |   |   +-- mood.cpp/h        # Context-aware phrase system
    |   |
    |   +-- gps/
    |   |   +-- gps.cpp/h         # TinyGPS++ wrapper, power mgmt
    |   |
    |   +-- ml/
    |   |   +-- features.cpp/h    # 32-feature WiFi extraction
    |   |   +-- inference.cpp/h   # Heuristic + Edge Impulse classifier
    |   |   +-- edge_impulse.h    # SDK scaffold
    |   |
    |   +-- modes/
    |       +-- oink.cpp/h        # WiFi scanning, deauth, capture
    |       +-- warhog.cpp/h      # GPS wardriving, exports
    |
    +-- .github/
    |   +-- copilot-instructions.md   # AI assistant context
    |
    +-- platformio.ini            # Build config


--[ 10 - Legal sh*t

    LISTEN CAREFULLY.

    This tool is for AUTHORIZED SECURITY RESEARCH and EDUCATIONAL
    PURPOSES ONLY.

        * Only use on networks YOU OWN or have EXPLICIT WRITTEN PERMISSION
        * Deauth attacks are ILLEGAL in most jurisdictions without consent
        * Wardriving laws vary by location - know your local regulations
        * The authors assume ZERO LIABILITY for misuse
        * Don't be an a**hole

    If you use this to pwn your neighbor's WiFi, you deserve whatever
    happens to you. We made a cute pig, not a get-out-of-jail-free card.


--[ 11 - Greetz

    Shouts to the legends:

        * evilsocket & the pwnagotchi project - the original inspiration
        * M5Stack - for making affordable hacking hardware
        * Edge Impulse - democratizing ML on embedded
        * The ESP32 community - keeping the hacking spirit alive
        * You - for reading this far

    "The WiFi is free if you're brave enough."

OINK! OINK!

==[EOF]==

