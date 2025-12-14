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
        3.3 - PIGGY BLUES Mode
        3.4 - HOG ON SPECTRUM Mode
        3.5 - File Transfer Mode
        3.6 - LOOT Menu & WPA-SEC Integration
        3.7 - Machine Learning
        3.8 - Enhanced ML Mode
        3.9 - XP System
        3.10 - Achievements
        3.11 - SWINE STATS (Buff System)
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
    doing - gets hyped when you pop a 4-way, goes tactical when you're
    mapping the concrete jungle, annoys every phone in bluetooth range,
    paints pretty pictures of RF interference, and gets sleepy when the
    airwaves go quiet. Feed it handshakes and it'll love you forever.


--[ 2 - What the hell is this thing

    PORKCHOP is built on the ESP32-S3 platform running on M5Cardputer
    hardware. It's designed for:

        - Passive WiFi reconnaissance
        - WPA/WPA2 handshake capture (PMKID yoink too)
        - GPS-enabled wardriving
        - BLE notification spam (Apple, Android, Samsung, Windows)
        - Real-time 2.4GHz spectrum visualization
        - ML-powered rogue AP detection
        - Looking cute while doing questionable things

    Your digital companion reacts to discoveries like any good attack pet
    should. Captures make it happy. BLE chaos makes it chatty. Spectrum
    mode makes it analytical. Boredom makes it sad. It's basically you,
    but as an ASCII pig.


--[ 3 - Capabilities


----[ 3.1 - OINK Mode

    The bread and butter. Press 'O' and let the piglet loose:

        * Channel hopping across all 802.11 channels
        * Promiscuous mode packet capture  
        * EAPOL frame detection and 4-way handshake reconstruction
        * PMKID yoink from M1 frames - no client needed, pure stealth
        * Smart PMKID filtering - zero/empty PMKIDs get tossed (useless cruft)
        * Deauth capability for... "authorized testing purposes"
        * Real-time ML classification of suspicious APs
        * Auto-attack mode cycles through targets automatically
        * Targeted deauth prioritizes discovered clients
        * PCAP export to SD for post-processing
        * Hashcat 22000 format export - both EAPOL (_hs.22000) and PMKID (.22000)
        * Fire up that GPU: `hashcat -m 22000 loot.22000 wordlist.txt`

    Stealth features (because WIDS exist):

        * MAC randomization on mode start - new identity each time
        * Deauth jitter (1-5ms random delays) - no machine-perfect timing
        * Both enabled by default, toggle in Settings if you're feeling bold


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


----[ 3.3 - PIGGY BLUES Mode

    When you feel like spreading digital chaos, press 'B' and watch 
    nearby phones lose their minds:

        * BLE notification spam for Apple, Android, Samsung, Windows
        * Apple Nearby Action floods - fake AirPods pairing requests
        * Google FastPair bombardment - endless earbuds popups
        * Samsung Galaxy Buds/Watch pairing spam
        * Windows SwiftPair beacon storms
        * Smart targeting - scans for nearby devices and prioritizes
          payloads matching detected vendors
        * Random chaos mode when no targets identified

    The piglet gets real chatty in this mode. Something about 
    "spreading the oink" and "making friends the hard way."

    WARNING: This will annoy everyone around you. Educational use only.
    Don't be that guy at the coffee shop. Actually, maybe be that guy
    once, for science.


----[ 3.4 - HOG ON SPECTRUM Mode

    Press 'H' and watch the 2.4GHz band light up like a Christmas tree:

        * Real-time WiFi spectrum analyzer visualization
        * 13-channel display with proper Gaussian lobe spreading
        * 22MHz channel width represented accurately (sigma ~6.6 pixels)
        * Channel hopping at 100ms per channel (~1.3s full sweep)
        * Lobe height based on RSSI - stronger signal = taller peak
        * [VULN!] indicator for weak security (OPEN/WEP/WPA1)
        * [DEAUTH] indicator for networks without PMF protection
        * Network selection via ; and . - scroll through discovered APs
        * Enter key shows network details (SSID, BSSID, RSSI, channel, auth)
        * Bottom bar shows selected network info or scan status
        * Stale networks removed after 5 seconds - real-time accuracy

    The spectrum view shows what's actually happening on the airwaves.
    Each lobe represents a network's signal bleeding across adjacent
    channels - because that's how 802.11b/g actually works. Welcome to
    RF hell. Bring headphones, your coffee shop is loud.

    Scroll through networks to find the interesting ones. Hit Enter to
    see details. Press Backspace or G0 to bail. Simple as.


----[ 3.5 - File Transfer Mode

    Need to grab those juicy PCAPs off your piglet? WiFi file transfer:

        * Connects to YOUR WiFi network (configure SSID/password in settings)
        * Black & white web interface at porkchop.local or device IP
        * Browse SD card directories (/handshakes, /wardriving, etc.)
        * Download captured handshakes and wardriving data
        * Upload files back to the piglet
        * No cables, no fuss

    Set your network creds in Settings before trying to connect or the
    pig will stare at you blankly wondering what you expected.


----[ 3.6 - LOOT Menu & WPA-SEC Integration

    Your spoils of war. Hit LOOT from the main menu to see what
    you've captured:

        * Browse all captured handshakes and PMKIDs
        * [P] prefix = PMKID capture, no prefix = full handshake
        * Status indicators show WPA-SEC cloud cracking status:
            - [OK] = CRACKED - password found, press Enter to see it
            - [..] = UPLOADED - waiting for distributed cracking
            - [--] = LOCAL - not uploaded yet
        * Enter = view details (SSID, BSSID, password if cracked)
        * U = Upload selected capture to WPA-SEC
        * R = Refresh results from WPA-SEC
        * D = NUKE THE LOOT - scorched earth, rm -rf /handshakes/*

    WPA-SEC Integration (wpa-sec.stanev.org):

        Distributed WPA/WPA2 password cracking. Upload your .pcap
        files to a network of hashcat rigs that work while you sleep.
        Free as in beer. No GPU? No problem.

        Setup:
        1. Register at https://wpa-sec.stanev.org
        2. Get your 32-char hex API key from your profile
        3. Create file /wpasec_key.txt on SD card with your key
        4. Reboot - key auto-imports and file self-destructs
           (security through obscurity, but it's something)

        Or use Settings menu: Tweak -> "< Load Key File >"

        Once configured, the LOOT menu shows real-time status.
        Upload captures with U, check results with R. That's it.
        When a capture shows [OK], press Enter to see the password.

    File format breakdown:

        +-------------------+---------------------------------------+
        | Extension         | What it is                            |
        +-------------------+---------------------------------------+
        | .pcap             | Raw packets - for Wireshark nerds     |
        | _hs.22000         | Hashcat EAPOL (WPA*02) - full shake   |
        | .22000            | Hashcat PMKID (WPA*01) - clientless   |
        | _ssid.txt         | SSID companion file (human readable)  |
        +-------------------+---------------------------------------+

    PMKID captures are nice when they work. Not all APs cough one up.
    Zero PMKIDs (empty KDEs) are automatically filtered - if the pig
    says it caught a PMKID, it's a real one worth cracking.


----[ 3.7 - Machine Learning

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


----[ 3.8 - Enhanced ML Mode

    Two collection modes for different threat models:

        BASIC MODE (default)
        ----------------------
        Uses ESP32 WiFi scan API. Fast. Reliable. Limited features.
        Good for casual wardriving when you just want the basics.

        ENHANCED MODE
        ----------------------
        Enables promiscuous beacon capture. Parses raw 802.11 frames.
        Extracts the juicy bits:

            * IE 0  (SSID)              - Catches all-null hidden SSIDs
            * IE 3  (DS Parameter Set)  - Real channel info
            * IE 45 (HT Capabilities)   - 802.11n fingerprinting
            * IE 48 (RSN)               - WPA2/WPA3, PMF, cipher suites
            * IE 50 (Extended Rates)    - Rate analysis
            * IE 221 (Vendor Specific)  - WPS, WPA1, vendor ID

        Higher CPU. More memory. More features. Worth it.

    Toggle in Settings: [ML Mode: Basic/Enhanced]

    Each network gets an anomalyScore 0.0-1.0 based on:

        * RSSI > -30 dBm         Suspiciously strong. Honeypot range.
        * Open or WEP encryption What year is it?
        * Hidden SSID            Something to hide?
        * Non-standard beacon    Not 100ms = software AP.
        * No HT capabilities     Ancient router or spoofed.
        * WPS on open network    Classic honeypot fingerprint.


----[ 3.9 - XP System

    Your piglet has ambitions. Every network sniffed, every handshake
    grabbed, every deauth fired - it all counts. The XP system tracks
    your progress from BACON N00B all the way up through 40 ranks of
    increasingly unhinged titles.

    We're not gonna spoil the progression. Grind and find out.

    The bottom of your screen shows your current rank and progress bar.
    Level up and you'll see that popup. Your pig has opinions about
    your achievements. Embrace them.

    XP values - what makes the pig happy:

        +------------------------+--------+
        | Event                  | XP     |
        +------------------------+--------+
        | Network discovered     | 1      |
        | Hidden SSID found      | 3      |
        | Open network (lol)     | 3      |
        | WPA3 network spotted   | 10     |
        | Handshake captured     | 50     |
        | PMKID yoinked          | 75     |
        | Deauth success         | 15     |
        | AP logged with GPS     | 2      |
        | 1km wardriving         | 25     |
        | GPS lock acquired      | 5      |
        | BLE spam burst         | 2      |
        | 30min session          | 10     |
        | 1hr session            | 25     |
        | 2hr session (touch grass) | 50  |
        +------------------------+--------+

    Top tier ranks reference hacker legends and grindhouse cinema. If
    you hit level 40 and don't recognize the name, you've got homework.

    XP persists in NVS - survives reboots, even reflashing. Your pig
    remembers everything. The only way to reset is to wipe NVS manually.
    We don't provide instructions because if you need them, you're not
    ready to lose your progress.


----[ 3.10 - Achievements

    47 secret badges to prove you're not just grinding mindlessly.
    Or maybe you are. Either way, proof of pwn.

    The Achievements menu shows what you've earned. Locked ones show
    as "???" because where's the fun in spoilers? Each achievement
    pops a toast-style card with its unlock condition when selected.

    Some are easy. Some require dedication. A few require... luck.
    Or low battery at exactly the wrong moment.

    Not gonna list them. That's cheating. Hunt for them like you
    hunt for handshakes.


----[ 3.11 - SWINE STATS (Buff System)

    Press 'S' or hit SWINE STATS in the menu to see your lifetime
    progress and check what buffs/debuffs are currently messing with
    your piglet's performance.

    Two tabs: ST4TS shows your lifetime scoreboard, B00STS shows
    what's actively buffing or debuffing your pig.


----[ 3.11.1 - Class System

    Every 5 levels your pig promotes to a new class tier. Classes
    grant PERMANENT CUMULATIVE buffs - higher tier = more stacking:

        +--------+--------+------------------------------------------+
        | LEVELS | CLASS  | BUFF UNLOCKED                            |
        +--------+--------+------------------------------------------+
        | 1-5    | SH0AT  | (starter tier - no perks yet)            |
        | 6-10   | SN1FF3R| P4CK3T NOSE: -10% channel hop interval   |
        | 11-15  | PWNER  | H4RD SNOUT: +1 deauth burst frame        |
        | 16-20  | R00T   | R04D H0G: +15% distance XP               |
        | 21-25  | R0GU3  | SH4RP TUSKS: +1s lock time               |
        | 26-30  | EXPL01T| CR4CK NOSE: +10% capture XP              |
        | 31-35  | WARL0RD| IR0N TUSKS: -1ms deauth jitter           |
        | 36-40  | L3G3ND | OMNI P0RK: +5% all effects               |
        +--------+--------+------------------------------------------+

    Example: L38 player has ALL 7 class buffs active simultaneously.
    That's -10% hop, +1 burst, +15% dist XP, +1s lock, +10% cap XP,
    -1ms jitter, and +5% on everything. The grind pays off.


----[ 3.10.2 - Mood Buffs/Debuffs

    The mood system ties happiness to mechanics. Happy pig = aggressive.
    Sad pig = sluggish. Keep the meter up and feel the difference.

    BUFFS (Positive Effects):

        +---------------+-------------------+-----------------------------+
        | NAME          | TRIGGER           | EFFECT                      |
        +---------------+-------------------+-----------------------------+
        | R4G3          | happiness > 70    | +50% deauth burst (5->8)    |
        | SNOUT$HARP    | happiness > 50    | +25% XP gain                |
        | H0TSTR3AK     | 2+ HS in session  | +10% deauth efficiency      |
        | C4FF31N4T3D   | happiness > 80    | -30% channel hop interval   |
        +---------------+-------------------+-----------------------------+

    DEBUFFS (Negative Effects):

        +---------------+-------------------+-----------------------------+
        | NAME          | TRIGGER           | EFFECT                      |
        +---------------+-------------------+-----------------------------+
        | SLOP$LUG      | happiness < -50   | -30% deauth burst (5->3)    |
        | F0GSNOUT      | happiness < -30   | -15% XP gain                |
        | TR0UGHDR41N   | 5min no activity  | +2ms deauth jitter          |
        | HAM$TR1NG     | happiness < -70   | +50% channel hop interval   |
        +---------------+-------------------+-----------------------------+

    Mood affects everything:

        - Handshakes boost happiness big time
        - Networks keep the pig engaged
        - Long idle periods drain the mood
        - Momentum system: rapid captures = stacking bonuses

    The screen shows your lifetime stats in that 1337 style you love:

        N3TW0RKS    = Lifetime network count
        H4NDSH4K3S  = Total captures (4-way + PMKID)
        PMK1DS      = Clientless captures specifically
        D34UTHS     = Frames of chaos sent
        D1ST4NC3    = Kilometers walked while wardriving
        BL3 BL4STS  = BLE spam packets sent
        S3SS10NS    = How many times you've fired this thing up
        GH0STS      = Hidden SSID networks found
        WP4THR33    = WPA3 networks spotted (the future)
        G30L0CS     = GPS-tagged network discoveries

    Keep the pig happy. Happy pig = effective pig.


--[ 4 - Hardware

    Required:
        * M5Cardputer (ESP32-S3 based)
        * MicroSD card for data storage

    Optional:
        * AT6668 GPS Module (WARHOG mode)
        * Questionable ethics

----[ 4.1 - GPS Module Setup

    Different hardware uses different pins. Configure in Settings Menu:

    +---------------------------+--------+--------+---------+
    | Hardware Setup            | RX Pin | TX Pin | Baud    |
    +---------------------------+--------+--------+---------+
    | Cardputer + Grove GPS     | G1     | G2     | 115200  |
    | Cardputer-Adv + LoRa Cap  | G13    | G15    | 115200  |
    +---------------------------+--------+--------+---------+

    The Cap LoRa868 (U201) uses the EXT 14-pin bus, not Grove.
    After flashing, go to Settings and change GPS RX/TX pins.


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
        | O     | OINK - start hunting             |
        | W     | WARHOG - start wardriving        |
        | B     | PIGGY BLUES - BLE chaos mode     |
        | H     | HOG ON SPECTRUM - WiFi analyzer  |
        | S     | SWINE STATS - lifetime stats     |
        | T     | Tweak settings                   |
        | P     | Screenshot - save to SD card     |
        | `     | Toggle menu / Go back            |
        | ;     | Navigate up / Scroll left        |
        | .     | Navigate down / Scroll right     |
        | Enter | Select / Toggle / Confirm        |
        | Bksp  | Stop current mode, return idle   |
        | G0    | Bail out - return to IDLE        |
        +-------+----------------------------------+

    G0 is the physical button on the top side of the M5Cardputer.
    Press it anytime to bail out and return to IDLE. Useful when
    your piglet is going ham on someone's network.

    Screenshots are saved to /screenshots/screenshotNNN.bmp on the SD
    card. Takes about 1.4 seconds - piggy freezes briefly. Worth it
    for the documentation flex.


----[ 6.1 - Screen Layout

    Your piglet's face lives on a 240x135 pixel canvas. Not much real
    estate, but enough to cause trouble.

    Top bar format:

        [MODE]          HH:MM              XX% GWM

    Left = current mode. Center = GPS time or --:-- if no fix.
    Right = battery percent + status flags. G=GPS, W=WiFi, M=ML.
    Dashes mean inactive.

    The piglet has moods. Watch the face change as it hunts:

        NEUTRAL     HAPPY       EXCITED     HUNTING     SLEEPY      SAD
         ?  ?        ^  ^        !  !        /  \        v  v        .  .
        (o 00)      (^ 00)      (@ 00)      (> 00)      (- 00)      (T 00)
        (    )      (    )      (    )      (    )      (    )      (    )

    Yes, we spent actual development time on pig facial expressions.
    No regrets.


----[ 6.2 - OINK Mode Screen

    Press O and let the piglet loose. Here's the hunting grounds:

        +----------------------------------------------+
        | [OINK]          --:--              85% GWM   |  <- Top Bar
        +----------------------------------------------+
        |                                              |
        |       /  \      .------------------------.   |
        |      (> 00)  < | hunting truffles        |   |  <- Avatar + Bubble
        |      (    )     `------------------------'   |
        |    1010110110                                |  <- Binary grass
        |                                              |
        +----------------------------------------------+
        | N:42 HS:3 D:127 CH:6                    1:23 |  <- Stats + Uptime
        +----------------------------------------------+

    Bottom bar - the numbers that matter:

        N:42    = Snouts don't lie. 42 networks in range.
        HS:3    = 3 four-ways in the bag. Hashcat is waiting.
        D:127   = 127 frames of pure chaos unleashed.
        CH:6    = Currently sniffing channel 6.
        PWN:xxx = Trophy wall. Last victim's SSID.
        1:23    = How long piggy been running wild.

    The grass under piggy's feet tells you what's happening. When you
    see it scrolling, channel hopping is active - the snout is working.
    Static grass means the pig is chilling, waiting for orders.


----[ 6.3 - WARHOG Mode Screen

    GPS + WiFi = tactical recon. Hook up that GPS and hit the road:

        +----------------------------------------------+
        | [WARHOG]        12:45              85% GWM   |  <- Top Bar
        +----------------------------------------------+
        |                                              |
        |       !  !      .------------------------.   |
        |      (@ 00)  < | hog on patrol           |   |  <- Avatar + Bubble
        |      (    )     `------------------------'   |
        |    1110100111                                |  <- Binary grass
        |                                              |
        +----------------------------------------------+
        | U:128 S:45 [42.36,-71.05] S:7          12:45 |  <- Stats + Uptime
        +----------------------------------------------+

    Bottom bar - the wardriving scoreboard:

        U:128           = 128 unique APs mapped. New territory.
        S:45            = 45 entries written to SD. Receipts.
        [42.36,-71.05]  = You are here. Lat,lon.
        S:7             = 7 birds in the sky tracking you back.
        12:45           = Time on the hunt.

    The grass is your GPS indicator. Moving grass = fix acquired,
    coords are logging, you're making progress. Static grass = no fix,
    piggy is blind and sad, nothing getting saved with coords.

    When the fix locks, piggy goes "gps locked n loaded" and gets hyped.
    When you lose it, piggy sulks. The grass never lies.


--[ 7 - Configuration

    Settings persist to SPIFFS. Your piglet remembers.

        +------------+-------------------------------+---------+
        | Setting    | Description                   | Default |
        +------------+-------------------------------+---------+
        | WiFi SSID  | Network for file transfer     | -       |
        | WiFi Pass  | Password for that network     | -       |
        | Sound      | Beeps when things happen      | ON      |
        | Brightness | Display brightness            | 80%     |
        | Dim After  | Screen dim timeout, 0=never   | 30s     |
        | Dim Level  | Brightness when dimmed        | 20%     |
        | CH Hop     | Channel hop interval          | 500ms   |
        | Lock Time  | Client discovery window       | 3000ms  |
        | Deauth     | Enable deauth attacks         | ON      |
        | Rnd MAC    | Randomize MAC on mode start   | ON      |
        | GPS        | Enable GPS module             | ON      |
        | GPS PwrSave| Sleep GPS when not hunting    | ON      |
        | Scan Intv  | WARHOG scan frequency         | 5s      |
        | GPS Baud   | GPS module baud rate          | 115200  |
        | Timezone   | UTC offset for timestamps     | 0       |
        | ML Mode    | Basic/Enhanced beacon capture | Basic   |
        | SD Log     | Debug logging to SD card      | OFF     |
        | BLE Burst  | BLE advertisement interval    | 200ms   |
        | BLE Adv    | Per-packet duration           | 100ms   |
        | BLE Rescan | Target device refresh rate    | 60s     |
        +------------+-------------------------------+---------+


--[ 8 - ML Training Pipeline

    Want to train your own model? Here's the workflow:

----[ 8.1 - Data Collection

    WARHOG mode automatically collects ML training data. No extra steps.

    How it works:
        - Every network gets 32 features extracted from beacon frames
        - Data accumulates in memory as you drive around
        - Every 60 seconds, WARHOG dumps to /ml_training.csv - crash protection
        - When you stop WARHOG (G0 button), final export happens
        - Worst case you lose 1 minute of data if piggy crashes

    The dump contains:
        BSSID, SSID, channel, RSSI, authmode, HT caps, vendor IEs,
        beacon interval, jitter, GPS coords, timestamp, and label

    Set ML Mode to Enhanced in settings for deep beacon parsing.
    Basic mode uses ESP32 scan API. Enhanced mode sniffs raw 802.11.
    More features, more CPU, more fun.

----[ 8.2 - Labeling

    Raw data starts unlabeled. Use the prep script to auto-label based
    on security characteristics:

        $ python scripts/prepare_ml_data.py ml_training.csv

    The script outputs ml_training_ei.csv with string labels:

        normal        = Legit ISP routers, standard secure configs
        rogue_ap      = Strong signal + suspicious characteristics
        evil_twin     = Impersonating known network - label manually
        deauth_target = No WPA3/PMF - free real estate
        vulnerable    = Open/WEP/WPS enabled

    The auto-labeler catches the obvious stuff. For real rogue/evil twin
    samples, you gotta set up sketchy APs in the lab and label manually.
    Upload your labeled CSV to Edge Impulse for training.

----[ 8.3 - Training on Edge Impulse

    Edge Impulse handles the heavy lifting:

        1. Create project at studio.edgeimpulse.com
        2. Upload your labeled ml_training.csv
        3. Design impulse: Raw data block -> Classification (Keras)
        4. Train for 50+ epochs, check confusion matrix
        5. Test on held-out data, iterate if needed

    Aim for 90%+ accuracy before deploying. Your piggy deserves
    a brain that actually works.

----[ 8.4 - Deployment

    When your model is ready:

        1. Export as "C++ Library" targeting ESP32
        2. Extract edge-impulse-sdk/ into porkchop/lib/
        3. Open src/ml/edge_impulse.h
        4. Uncomment: #define EDGE_IMPULSE_ENABLED
        5. Rebuild and flash

    Now your piglet runs real inference instead of heuristics.
    The grass still moves the same way, but the brain got an upgrade.


--[ 9 - Code Structure

    porkchop/
    |
    +-- src/
    |   +-- main.cpp              # Entry point, main loop
    |   +-- core/
    |   |   +-- porkchop.cpp/h    # State machine, mode management
    |   |   +-- config.cpp/h      # Configuration (SPIFFS persistence)
    |   |   +-- sdlog.cpp/h       # SD card debug logging
    |   |   +-- wsl_bypasser.cpp/h # Frame injection, MAC randomization
    |   |   +-- xp.cpp/h          # RPG XP/leveling, achievements, NVS
    |   |
    |   +-- ui/
    |   |   +-- display.cpp/h     # Triple-canvas display system
    |   |   +-- menu.cpp/h        # Main menu with callbacks
    |   |   +-- settings_menu.cpp/h   # Interactive settings
    |   |   +-- captures_menu.cpp/h   # LOOT menu - browse captured handshakes
    |   |   +-- achievements_menu.cpp/h # Proof of pwn viewer
    |   |   +-- log_viewer.cpp/h  # View SD card logs
    |   |   +-- swine_stats.cpp/h # Lifetime stats, buff/debuff system
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
    |   |   +-- oink.cpp/h        # WiFi scanning, deauth, capture
    |   |   +-- warhog.cpp/h      # GPS wardriving, exports
    |   |   +-- piggyblues.cpp/h  # BLE notification spam
    |   |   +-- spectrum.cpp/h    # WiFi spectrum analyzer
    |   |
    |   +-- web/
    |       +-- fileserver.cpp/h  # WiFi file transfer server
    |
    +-- scripts/
    |   +-- prepare_ml_data.py    # Label & convert data for Edge Impulse
    |   +-- pre_build.py          # Build info generator
    |
    +-- docs/
    |   +-- EDGE_IMPULSE_TRAINING.txt  # Step-by-step ML training guide
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

