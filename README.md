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

--[ Quick Start - The Only Way That Matters

    M5 Launcher + firmware.bin from GitHub releases.
    
    That's it. No M5 Burner. No merged binaries. No esptool wizardry.
    
        1. Already got M5 Launcher? Good. Skip to step 3.
        2. No Launcher? Flash it once via M5 Burner. One time. Never again.
        3. Grab firmware.bin: github.com/0ct0sec/M5PORKCHOP/releases
        4. Drop on SD card. Launcher -> SD -> install.
        5. Oink.
    
    Updates? Same thing. Download new firmware.bin, SD card, install.
    XP preserved forever. Your MUDGE UNCHA1NED grind stays intact.
    
    NEW IN v0.1.6: IMMORTAL PIG. XP now backs up to SD card. M5 Burner
    nukes your flash? Pig recovers from SD. Full chip erase? Pig recovers.
    BUT - you must install v0.1.6 via Launcher FIRST to create the backup.
    After that, flash however you want. Burner, web, carrier pigeon.

    M5 Burner OTA? Don't. Wrong binary format. Bootloop city.
                   (recoverable via USB reflash, but why bother)
    M5 Burner USB? Works for fresh install. v0.1.6+ recovers XP from SD.
                   Just make sure SD card is in before first boot.
    
    The pig remembers those who respect the partition table.
    Now the pig also remembers those who don't. (SD backup FTW)


--[ Contents

    1 - Introduction
    2 - What the hell is this thing
    3 - Capabilities
        3.1 - OINK Mode
            3.1.1 - DO NO HAM Mode
            3.1.2 - Stationary Operation Tuning
        3.2 - WARHOG Mode
        3.3 - PIGGY BLUES Mode
        3.4 - HOG ON SPECTRUM Mode
            3.4.1 - CLIENT MONITOR
        3.5 - File Transfer Mode
        3.6 - LOOT Menu & WPA-SEC Integration
        3.7 - Machine Learning
        3.8 - Enhanced ML Mode
        3.9 - XP System
            3.9.1 - IMMORTAL PIG (XP Persistence)
        3.10 - Achievements
        3.11 - SWINE STATS (Buff System)
    4 - Hardware
    5 - Building & Flashing
        5.1 - Flashing Methods & Progress Preservation
    6 - Controls
    7 - Configuration
        7.1 - Color Themes
        7.2 - API Keys Setup
    8 - ML Training Pipeline
    9 - Code Structure
    10 - Legal sh*t
    11 - Greetz
    12 - Credits
    13 - Support The Pig


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

    This is why you're here. Press 'O' and watch your pig go feral.

    The hunt begins. Your piglet drops into promiscuous mode and starts
    hopping channels like a crackhead at a frequency buffet. Every beacon,
    every probe, every EAPOL frame gets slurped into its pink little brain.

    What you get:

        * 802.11 channel surfing - all 13 channels, no rest for the wicked
        * Raw packet capture - everything the radio can hear
        * 4-way handshake reconstruction - we catch 'em, we stitch 'em
        * PMKID yoink from M1 frames - clientless attack, pure stealth
        * Smart filtering - empty PMKIDs get yeeted (they're useless cruft)
        * Deauth + Disassoc - broadcast both frame types per cycle
        * ML classification - spot rogue APs before they spot you
        * Auto-attack - cycles through targets like a rotisserie of pain
        * Targeted deauth - discovered clients get personal attention
        * Hashcat 22000 export - GPU goes brrrrrr

    When a handshake drops, your pig loses its mind. Three beeps for 
    PMKID, happy oinks for EAPOL. Feed it enough and watch the XP climb.

    Output format: `hashcat -m 22000 *.22000 rockyou.txt`
    
    Your GPU will thank you. Your electric bill won't.


    Stealth features (because WIDS exist and sysadmins have feelings):

        * MAC randomization - new identity every mode start
        * Deauth jitter (1-5ms random delays) - no machine-gun timing
        * Both ON by default - we're not amateurs here

    Want to get caught? Turn 'em off. We won't judge. Actually we will.


    BOAR BROS - because nuking your own router is just sad:

        Every hog needs a pack. Press [B] and that network becomes
        family. Family don't get deauth'd. Family don't get stalked.
        Family lives in /boar_bros.txt on your SD card forever.

        Design philosophy: We always LISTEN to a Bro, but never PUNCH
        a Bro in the face. Passive capture still works - if Bro's client
        reconnects naturally and we catch the handshake, that's just
        being observant. We just don't actively attack Bros.

        * Home router? BRO. Work WiFi? ...your call.
        * Hit [B] mid-attack and watch the frames stop cold
        * Hidden networks join as "NONAME BRO" - anonymous bros welcome
        * Spectrum mode tags 'em with [BRO] - visible loyalty
        * Menu lets you manage your crew, [D] to cut ties
        * Passive handshake capture still works - we're listening, not punching

        The exclusion list isn't about mercy. It's about not having
        to explain to your roommate why Netflix keeps dropping.


    Quick toggle - tactical mode switch:

        Smash [D] while hunting to flip between chaos and zen:

        * "BRAVO 6, GOING DARK" - attacks die instantly, ghost mode
        * "WEAPONS HOT" - back to business after a 5s sweep
        * Persists to config - your preference survives reboots
        * Bottom bar screams "DOIN NO HAM" so you don't forget

        Perfect for when security walks by. Or your conscience kicks in.
        (just kidding, we know you don't have one)


------[ 3.1.1 - DO NO HAM Mode

    Sometimes you gotta be a good pig. Legal recon. Sensitive location.
    Your mom's house. Whatever. Press [D] or toggle in Settings.

    What changes:

        * Zero TX - not a single frame leaves your radio
        * 150ms channel hops - fast sweeps for drive-by recon
        * 150 network cap - OOM protection when you're collecting hundreds
        * 45s stale timeout - networks fall off faster when you're mobile
        * MAC always randomized - stealth isn't optional here
        * PMKID still works - M1 frames are passive catches, no TX needed

    The beautiful thing about PMKID: APs just... give it to you. In the
    first message of the handshake. Before any client even responds.
    You're not attacking. You're receiving a gift. Legally distinct.

    Perfect scenarios:

        * Warwalking through office buildings (educational purposes)
        * Fast recon from moving vehicles (passenger seat, officer)
        * When you actually need to use the WiFi you're sniffing
        * Catching natural reconnections without forcing them

    Your pig goes zen. Phrases like "quiet observer" and "sniff dont bite".
    Same ASCII face, zero criminal energy. The piglet equivalent of yoga.


------[ 3.1.2 - Stationary Operation Tuning

    Parked somewhere? Camping a target? Time to optimize for the kill.

    When you're planted, so are your targets. Patience pays. The secret
    sauce: find the clients BEFORE you start swinging. Targeted deauth
    is exponentially more effective than broadcast spam.

    The math doesn't lie:

        +----------+------------------+-------------------+
        | Clients  | Targeted Deauths | Broadcast Deauths |
        | Found    | per 100ms cycle  | per 100ms cycle   |
        +----------+------------------+-------------------+
        |    0     |        0         |         1         |
        |    1     |       5-8        |         1         |
        |    2     |      10-16       |         1         |
        |    3     |      15-24       |         1         |
        +----------+------------------+-------------------+

    One client = 5-8x the pain. Two clients = 10-16x. Three? Absolute
    carnage. Broadcast deauth is like yelling "FIRE" in a crowded room.
    Targeted deauth is whispering in someone's ear "your session is over."

    Optimal camping config:

        +------------+----------+---------+------------------------------+
        | Setting    | CAMPING  | Default | Why                          |
        +------------+----------+---------+------------------------------+
        | CH Hop     | 800ms    | 500ms   | Thorough sweep, no rush      |
        | Lock Time  | 6000ms   | 4000ms  | MORE CLIENTS = MORE PWNS     |
        | Deauth     | ON       | ON      | Obviously                    |
        | Rnd MAC    | ON       | ON      | Still need stealth           |
        | DO NO HAM  | OFF      | OFF     | We're here for handshakes    |
        +------------+----------+---------+------------------------------+

    Lock Time is THE lever. During LOCKING state, your pig sniffs data
    frames to find connected clients. More time = more clients = more
    surgical strikes = more handshakes = more passwords = more dopamine.

    The state machine:

        SCANNING (5s) --> LOCKING (6s*) --> ATTACKING (up to 15s)
                              |                    |
                         sniff data           deauth storm
                         find clients         catch EAPOL
                              |                    |
                              v                    v
                        clientCount++        handshake.22000

    * With recommended 6000ms Lock Time

    Class perks that stack with stationary ops:

        R0GU3 (L21-25):  SH4RP TUSKS +1s lock - even more client discovery
        PWNER (L11-15):  H4RD SNOUT +1 burst frame - hit harder
        WARL0RD (L31+):  1R0N TUSKS -1ms jitter - tighter burst timing

    TL;DR: Set Lock Time to 6000ms. Park your ass. Wait. Profit.

    Mobile recon? DO NO HAM mode. Stationary assault? Lock Time 6000ms.
    Know the difference. Be the difference. Oink responsibly.


----[ 3.2 - WARHOG Mode

    Strap a GPS to your pig and hit the streets. Press 'W' to go full
    wardriver - your ancestors did this with a Pringles can, you get to
    do it with style.

    When your piglet has a fix, every network it sniffs gets tagged with
    coordinates, timestamped, and dumped to SD. Wigle leaderboard chasers,
    this is your mode.

    What's happening under the hood:

        * Real-time lat/lon on the bottom bar - watch yourself move
        * Per-scan direct-to-disk writes - no RAM accumulation, no OOM
        * 5000 BSSID cache before dedup stops (more than enough)
        * Crash protection: 60s auto-dumps, worst case = 1 min data loss
        * 32-feature ML extraction for every AP (Enhanced mode)
        * Dual export: internal CSV + WiGLE v1.6 format simultaneously

    Export formats for your collection:

        * CSV: Spreadsheet warriors, this is yours
        * Wigle: v1.6 format, ready for wigle.net upload
        * ML Training: Feature vectors for Edge Impulse, feed the brain

    WiGLE integration is automatic. Every geotagged network gets written
    to both /wardriving/warhog_*.csv (internal format) and
    /wardriving/warhog_*.wigle.csv (WiGLE v1.6). 

    Upload options:
        * Manual: Take .wigle.csv home, upload at wigle.net/upload
        * PORK TRACKS menu: Upload directly from the device via WiFi

    PORK TRACKS (WiGLE upload menu):
    
        Your wardriving conquests deserve global recognition. Open PORK
        TRACKS from the main menu to see all your WiGLE files. Each shows:
        
        * Upload status: [OK] uploaded, [--] not yet
        * Approximate network count (calculated from file size)
        * File size for the bandwidth-conscious
        
        Controls:
        * [U] Upload selected file to wigle.net
        * [R] Refresh file list
        * [D] Nuke selected track (deletes file, no undo)
        * [Enter] Show file details
        * [`] Exit back to menu
        
        First time? Add your WiGLE API credentials:
        1. Get API token from wigle.net/account (API section)
        2. Create /wigle_key.txt on SD card: "apiname:apitoken"
        3. Load via Settings > < Load WiGLE Key >
        4. Key file auto-deletes after import (security)

    No GPS? No coordinates. The pig still logs networks but you get zeros
    in the lat/lon columns. ML training data still useful though - Enhanced
    mode extracts features regardless of GPS lock.

    Pro tip: Set ML Mode to Enhanced before you roll out. Basic mode uses
    the scan API. Enhanced mode parses raw 802.11 beacons. More features,
    more fingerprinting power, more rogue AP detection. Worth the CPU.

    When you're done, hit G0 or Backspace. Final export triggers. Your
    wardriving data lives in /wardriving/ on the SD card. Bring it home,
    upload it, brag about your coverage. Repeat.


----[ 3.3 - PIGGY BLUES Mode

    Press 'B' and become everyone's least favorite person in the room.

    Your pig transforms into a BLE irritant generator. Every phone in
    range starts lighting up with fake pairing requests, phantom earbuds,
    and notification spam that just. Won't. Stop.

    The attack surface:

        * AppleJuice floods - "AirPods Pro want to connect" x infinity
        * Google FastPair spam - Android's worst nightmare, popup city
        * Samsung Buds/Watch impersonation - Galaxy users aren't safe
        * Windows SwiftPair storms - laptops join the party too
        * Continuous passive scanning - finds devices, tailors payloads
        * Vendor-aware targeting - Macs get Apple spam, Pixels get Google

    The piglet gets REAL chatty in this mode. "making friends the hard
    way" and "spreading the oink" are just the start. It knows what it's
    doing. It's enjoying itself. Maybe too much.

    How it works:

        1. NimBLE async scan finds nearby devices
        2. Manufacturer data fingerprints the vendor
        3. Targeted payloads get crafted and queued
        4. Opportunistic advertising in scan gaps
        5. Repeat until you get escorted out or bored

    Random chaos mode kicks in when no specific targets are found. Just
    blanket-spams all protocol types. Scorched earth BLE policy.

    DISCLAIMER: This WILL annoy everyone within ~10 meters. Educational
    use only. Don't be that guy at the coffee shop. Or do. Once. For
    science. Then delete this from your device before anyone finds it.

    Achievement hunters: APPLE_FARMER, PARANOID_ANDROID, SAMSUNG_SPRAY,
    WINDOWS_PANIC, BLE_BOMBER, OINKAGEDDON. You know what to do.


----[ 3.4 - HOG ON SPECTRUM Mode

    Press 'H' and watch the 2.4GHz band light up like a Christmas tree:

        * Real-time WiFi spectrum analyzer visualization
        * 13-channel display with proper Gaussian lobe spreading
        * 22MHz channel width represented accurately (sigma ~6.6 pixels)
        * Channel hopping at 100ms per channel (~1.3s full sweep)
        * Lobe height based on RSSI - stronger signal = taller peak
        * [VULN!] indicator for weak security (OPEN/WEP/WPA1)
        * [DEAUTH] indicator for networks without PMF protection
        * [BRO] indicator for networks in your BOAR BROS exclusion list
        * Network selection via ; and . - scroll through discovered APs
        * Enter key opens CLIENT MONITOR for targeted hunting
        * Bottom bar shows selected network info or scan status
        * Stale networks removed after 5 seconds - real-time accuracy

    The spectrum view shows what's actually happening on the airwaves.
    Each lobe represents a network's signal bleeding across adjacent
    channels - because that's how 802.11b/g actually works. Welcome to
    RF hell. Bring headphones, your coffee shop is loud.

    Scroll through networks to find the interesting ones. Hit Enter to
    enter CLIENT MONITOR for focused hunting - see connected clients
    with proximity arrows and vendor OUI identification. Press Enter on
    a client to deauth them directly. Backspace or G0 to bail. Simple as.


------[ 3.4.1 - CLIENT MONITOR

    The spectrum got fangs. Press Enter on any network to enter the hunt.

    What you see:

        +------------------------------------------+
        | CLIENTS: COFFEESHOP_5G CH6               |
        +------------------------------------------+
        | 1.Apple    A3:F2 -55dB  3s >>            |
        | 2.Samsung  B1:C4 -68dB  1s >             |
        | 3.Random   D5:E6 -72dB  2s ==            |
        | 4.Xiaomi   F7:89 -85dB  4s <<            |
        +------------------------------------------+

    That's clients connected to the target network. Real-time. Updating
    every frame. The pig sees everything the router sees.

    Breakdown:

        * Client number + vendor (450+ OUI database, or "Random" if
          MAC randomization is detected - local-admin bit check)
        * Last two MAC octets (enough to identify when hunting)
        * Signal strength in dBm (how close to YOU, not the router)
        * Time since last packet (freshness - stale = walking away)
        * Proximity arrows (the money feature)

    The arrows tell you everything:

        >>  = Much closer to you than the router (+10dB or more)
        >   = Closer to you (+3 to +10dB)
        ==  = About the same distance (-3 to +3dB)
        <   = Farther from you (-3 to -10dB)
        <<  = Much farther than the router (-10dB or more)

    Walk around. Watch the arrows change. When >> appears, you're
    getting hot. When << appears, wrong direction. Marco Polo for WiFi.
    Less fun for the target.

    Controls:

        [;]     Navigate up through client list
        [.]     Navigate down through client list
        [Enter] DEAUTH selected client (5 frames each way)
        [B]     Add network to BOAR BROS and exit
        [`]     Exit to spectrum view
        [Bksp]  Exit to spectrum view

    Press Enter on a client. 5 deauth frames AP→Client. 5 more Client→AP.
    1-5ms random jitter between each. Brief toast: "DEAUTH XX:XX x5".
    Spam Enter for continuous deauth. ~300ms keyboard debounce = fire rate.

    Signal loss detection: no packets for 15 seconds = graceful exit.
    Descending beep. "SIGNAL LOST" toast. Back to spectrum view.
    No hanging. No stale data. Clean exit. Professional.

    Sound feedback (if enabled in Settings):

        * Enter client monitor: 700Hz 80ms (channel locked)
        * New client detected: 1200Hz 100ms (fresh meat)
        * Deauth sent: 600Hz 80ms (low thump)
        * Signal lost: 800→500Hz descending (exit)

    First 4 clients get beeps. After that, quiet. We're hunting, not DJing.


----[ 3.5 - File Transfer Mode

    Time to exfil. Your pig caught the goods, now get 'em off the device.

    PORKCHOP connects to YOUR WiFi network (station mode, not AP) and
    serves a janky black-and-white web interface. No CSS crimes here,
    just function.

        * Hit porkchop.local or the IP shown on screen
        * Browse SD card: /handshakes, /wardriving, /mldata, /logs
        * Download your .22000 files for hashcat
        * Upload wordlists, configs, whatever fits
        * No drivers, no cables, no USB permission bullsh*t

    Setup requirements:
        1. Configure WiFi SSID and password in Settings FIRST
        2. Make sure you're in range of that network
        3. Enter File Transfer from menu

    If you skip step 1, the pig just stares at you. It can't read your
    mind. It also can't connect to "linksys" with password "admin123"
    if you didn't tell it to.

    The web UI is deliberately ugly. It's functional. You're here to
    move files, not admire rounded corners. Get in, get out, get cracking.


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

    STATUS: EXPERIMENTAL. TRUST ISSUES AHEAD.
    
    The heuristics run. The scaffold exists. The training pipeline works.
    What's missing: enough labeled data to make the pig smart, and enough
    hubris to ship it as "production ready." We have the second part.
    Working on the first.
    
    TL;DR: don't bet your pentest report on these classifications yet.
    But feel free to collect data and help train the pig's brain.

    Here's what we're building toward - the ML system extracts
    32 features from every beacon frame:

        * Signal characteristics (RSSI, noise patterns)
        * Beacon timing analysis (interval, jitter)
        * Vendor IE fingerprinting
        * Security configuration analysis
        * Historical behavior patterns

    Built-in heuristic classifier detects:

        [!] ROGUE_AP    - Too loud, too weird, too sus. Honeypot vibes.
        [!] EVIL_TWIN   - Hiding its name but screaming its signal. Trap.
        [!] VULNERABLE  - Open/WEP/WPA1/WPS - security from 2004
        [!] DEAUTH_TGT  - No WPA3, no PMF, no protection, no mercy

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

        Burns more cycles. Eats more RAM. Catches more sketchy APs.
        The juice is worth the squeeze.

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

    XP values - the complete dopamine schedule:

        +----------------------------+--------+
        | Event                      | XP     |
        +----------------------------+--------+
        | Network discovered         | 1      |
        | Hidden SSID found          | 3      |
        | Open network (lol)         | 3      |
        | WEP network (ancient relic)| 5      |
        | WPA3 network spotted       | 10     |
        +----------------------------+--------+
        | Handshake captured         | 50     |
        | PMKID yoinked              | 75     |
        | Passive PMKID (ghost mode) | 100    |
        | Low battery capture (<10%) | +20    |
        +----------------------------+--------+
        | Deauth frame sent          | 2      |
        | Deauth success (reconnect) | 15     |
        +----------------------------+--------+
        | AP logged with GPS         | 2      |
        | 1km wardriving             | 25     |
        | GPS lock acquired          | 5      |
        +----------------------------+--------+
        | BLE spam burst             | 2      |
        | BLE Apple hit              | 3      |
        | BLE Android hit            | 2      |
        | BLE Samsung hit            | 2      |
        | BLE Windows hit            | 2      |
        +----------------------------+--------+
        | ML rogue AP detected       | 25     |
        +----------------------------+--------+
        | BOAR BRO added             | 5      |
        | Mid-attack mercy (B key)   | 15     |
        +----------------------------+--------+
        | 30min session              | 10     |
        | 1hr session                | 25     |
        | 2hr session (touch grass)  | 50     |
        +----------------------------+--------+

    Top tier ranks reference hacker legends and grindhouse cinema. If
    you hit level 40 and don't recognize the name, you've got homework.

    XP persists in NVS - survives reboots, even reflashing. Your pig
    remembers everything. The only way to reset is to wipe NVS manually.
    We don't provide instructions because if you need them, you're not
    ready to lose your progress.


------[ 3.9.1 - IMMORTAL PIG (XP Persistence)

    NVS lives at 0x9000. M5 Burner writes start at 0x0. You see the
    problem. Your L38 DARK TANGENT grind? Steamrolled. BACON N00B.

    Not anymore.

    v0.1.6 introduced SD card XP backup. Automatic. Every save.
    M5 Burner nukes your flash? Pig recovers from SD on boot.
    Full chip erase? Pig recovers. Factory reset? Pig. Recovers.

        NVS = Primary storage (fast, survives firmware updates)
        SD  = Backup storage (survives everything else)

    The catch: backup is device-bound and signed.

        +-----------------------------------+--------------------+
        | Action                            | Result             |
        +-----------------------------------+--------------------+
        | Edit XP values in hex editor      | Signature invalid  |
        | Copy save to different device     | Signature invalid  |
        | Download someone's save file      | Signature invalid  |
        | Corrupt the file                  | Validation fails   |
        | Use legitimately on your device   | Welcome back       |
        +-----------------------------------+--------------------+

    Want to tamper? Go ahead. It's a hacker tool. Source is public.
    Figure it out. We respect the attempt. But if you fail - LV1.
    BACON N00B. No exceptions. Earn your rank or crack the signature.

    File location: /xp_backup.bin on SD card root.
    Size: 100 bytes (96-byte struct + 4-byte CRC32 signature).
    Device binding: ESP32 MAC address baked into signature.


----[ 3.10 - Achievements

    63 secret badges to prove you're not just grinding mindlessly.
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


----[ 3.11.2 - Mood Buffs/Debuffs

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
    | Cardputer-Adv + LoRa Cap  | G15    | G13    | 115200  |
    +---------------------------+--------+--------+---------+

    The Cap LoRa868 (U201) uses the EXT 14-pin bus, not Grove.
    RX/TX are swapped vs what you'd expect - ESP32 RX connects to GPS TX.
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


----[ 5.1 - Flashing Methods & Progress Preservation

    Your XP, level, and achievements live in NVS (Non-Volatile Storage)
    at flash address 0x9000. Here's the cold hard truth:

        +---------------------------+-------------+----------------+
        | Method                    | XP/Level    | Settings       |
        +---------------------------+-------------+----------------+
        | M5 Launcher (SD card)     | PRESERVED   | PRESERVED      |
        | pio run -t upload         | PRESERVED   | PRESERVED      |
        | ESP Web Tool (firmware)   | PRESERVED   | PRESERVED      |
        | M5 Burner (merged bin)    | RECOVERED*  | PRESERVED      |
        +---------------------------+-------------+----------------+
        * v0.1.6+ recovers XP from SD card backup if available

    Why the recovery? The merged .bin for M5 Burner writes fill bytes
    from 0x0 to 0x10000, steamrolling NVS at 0x9000. But v0.1.6 checks
    for /xp_backup.bin on SD at boot. If NVS is empty but SD backup
    exists with valid signature = welcome back, warrior.

    Settings survive regardless - they live in SPIFFS at 0x610000.


------[ 5.1.1 - The Right Way (M5 Launcher)

    This is the recommended method for both install and upgrades:

        1. Get M5 Launcher on your device (one-time M5 Burner flash)
        2. Download firmware.bin from GitHub releases
        3. Copy to SD card, any directory
        4. Launcher -> SD -> navigate -> install
        5. XP preserved. Forever. Even on first install.

    Updates? Same exact process. No XP loss. Ever.


------[ 5.1.2 - Alternative Methods

    PlatformIO (for developers):
        $ git pull
        $ pio run -t upload -e m5cardputer
        # Your pig remembers everything

    ESP Web Tool (browser, no install):
        - Download firmware.bin from releases (NOT the merged one)
        - Go to https://espressif.github.io/esptool-js/
        - Connect, add firmware.bin at offset 0x10000
        - Flash ONLY firmware, NVS stays safe

    M5 Burner USB (v0.1.6+ with IMMORTAL PIG):
        - Flash the merged bin (porkchop_vX.X.X_m5burner.bin)
        - NVS gets nuked BUT pig recovers from SD backup
        - REQUIREMENT: Must have run v0.1.6+ at least once before
          to create the SD backup. No backup = BACON N00B.
        - NOTE: We removed PORKCHOP from M5 Burner's catalog.
          Download from GitHub releases.

    We provide both binaries in releases:
        - firmware_vX.X.X.bin          = M5 Launcher, preserves XP
        - porkchop_vX.X.X_m5burner.bin = M5 Burner, recovers from SD


--[ 6 - Controls

    The M5Cardputer's keyboard is tiny but functional:

        +-------+----------------------------------+
        | Key   | What it does                     |
        +-------+----------------------------------+
        | O     | OINK - start hunting             |
        | W     | WARHOG - start wardriving        |
        | B     | PIGGY BLUES - BLE chaos mode     |
        |       | (in OINK: add BOAR BRO)          |
        | H     | HOG ON SPECTRUM - WiFi analyzer  |
        | S     | SWINE STATS - lifetime stats     |
        | T     | Tweak settings                   |
        | D     | Toggle DO NO HAM (in OINK mode)  |
        | P     | Screenshot - save to SD card     |
        | `     | Back one level / Open menu       |
        | ;     | Navigate up / Scroll left        |
        | .     | Navigate down / Scroll right     |
        | Enter | Select / Toggle / Confirm        |
        | Bksp  | Stop current mode, return idle   |
        | G0    | Bail out - return to IDLE        |
        +-------+----------------------------------+

    G0 is the physical button on the top side of the M5Cardputer.
    Press it anytime to bail out and return to IDLE. Useful when
    your piglet is going ham on someone's network.

    Backtick navigation (v0.1.6+):

        From OINK/WARHOG/PIGGYBLUES/SPECTRUM -> IDLE
        From Client Monitor                  -> Spectrum view
        From IDLE                            -> Opens MENU
        From MENU/Settings                   -> Parent menu

    Intuitive. Only took six versions.

    Screenshots are saved to /screenshots/screenshotNNN.bmp on the SD
    card. Takes about 1.4 seconds - piggy freezes briefly. Worth it
    for the documentation flex.


----[ 6.1 - Screen Layout

    Your piglet's face lives on a 240x135 pixel canvas. Not much real
    estate, but enough to cause trouble.

    Top bar format:

        [MODE M00D]                       XX% GWM HH:MM

    Left = current mode + mood indicator (HYP3/GUD/0K/M3H/S4D).
    Right = battery percent + status flags + GPS time (or --:--).
    G=GPS, W=WiFi, M=ML. Dashes mean inactive.

    The piglet has moods. Watch the face change as it hunts:

        NEUTRAL     HAPPY       EXCITED     HUNTING     SLEEPY      SAD
         ?  ?        ^  ^        !  !        |  |        v  v        .  .
        (o 00)      (^ 00)      (@ 00)      (= 00)      (- 00)      (T 00)
        (    )      (    )      (    )      (    )      (    )      (    )

    Yes, we spent actual development time on pig facial expressions.
    No regrets.

    Network names display in UPPERCASE (v0.1.6+) for visibility on the
    tiny 240x135 screen. File exports keep original case. Settings menu
    stays lowercase - you need the mental workout when configuring.


--[ 7 - Configuration

    Settings persist to SPIFFS. Your piglet remembers.

    Navigate with ; and . keys, Enter to toggle/edit. Press ESC (backtick)
    or Backspace to save and exit. Changes take effect immediately,
    including GPS pin changes (hot-reinit, no reboot required).

        +------------+-------------------------------+---------+
        | Setting    | Description                   | Default |
        +------------+-------------------------------+---------+
        | WiFi SSID  | Network for file transfer     | -       |
        | WiFi Pass  | Password for that network     | -       |
        | WPA-SEC Key| 32-char hex key for cracking  | -       |
        | WiGLE Name | WiGLE API name for uploads    | -       |
        | WiGLE Token| WiGLE API token for uploads   | -       |
        | Sound      | Beeps when things happen      | ON      |
        | Brightness | Display brightness            | 80%     |
        | Dim After  | Screen dim timeout, 0=never   | 30s     |
        | Dim Level  | Brightness when dimmed        | 20%     |
        | Theme      | Color scheme for UI           | P1NK    |
        | CH Hop     | Channel hop interval          | 500ms   |
        | Lock Time  | Client discovery window       | 4000ms  |
        | Deauth     | Enable deauth attacks         | ON      |
        | Rnd MAC    | Randomize MAC on mode start   | ON      |
        | DO NO HAM  | Passive-only recon mode       | OFF     |
        | GPS        | Enable GPS module             | ON      |
        | GPS PwrSave| Sleep GPS when not hunting    | ON      |
        | Scan Intv  | WARHOG scan frequency         | 5s      |
        | GPS RX Pin | GPIO for GPS data receive     | 1       |
        | GPS TX Pin | GPIO for GPS data transmit    | 2       |
        | GPS Baud   | GPS module baud rate          | 115200  |
        | Timezone   | UTC offset for timestamps     | 0       |
        | ML Mode    | Basic/Enhanced beacon capture | Basic   |
        | SD Log     | Debug logging to SD card      | OFF     |
        | BLE Burst  | BLE advertisement interval    | 200ms   |
        | BLE Adv Tm | Per-packet duration           | 100ms   |
        +------------+-------------------------------+---------+

    GPS pin defaults work for original Cardputer + Grove GPS. If you're
    running Cardputer-Adv with Cap LoRa868 module, change pins to:
    RX=15, TX=13. Yes, swapped - ESP32 RX receives from GPS TX.
    GPS reinits automatically when pins change - no reboot.


----[ 7.1 - Color Themes

    Your piglet isn't locked to pink. Cycle through themes with ; and .
    on the Theme setting. 12 flavors, from tactical to absurd:

        +------------+--------------------------------------------+
        | Theme      | Vibe                                       |
        +------------+--------------------------------------------+
        | P1NK       | Classic piglet pink on black (default)     |
        | CYB3R      | Electric cyan. Because it's 2077 somewhere |
        | M4TR1X     | Green phosphor. See the code, Neo          |
        | AMB3R      | Warm terminal amber. Old school CRT feels  |
        | BL00D      | Aggressive red. For when you mean business |
        | GH0ST      | White on black. Low-viz stealth mode       |
        +------------+--------------------------------------------+
        | PAP3R      | Black on white. The inverted heresy        |
        | BUBBLEGUM  | Hot pink on white. Aggressively visible    |
        | M1NT       | Teal on white. Refreshing. Minty.          |
        | SUNBURN    | Orange on white. Eye damage included       |
        +------------+--------------------------------------------+
        | L1TTL3M1XY | OG Game Boy LCD. 1989 pea-soup nostalgia   |
        | B4NSH33    | P1 phosphor green. VT100 ghost vibes       |
        +------------+--------------------------------------------+

    Dark themes (top 6) keep things tactical. Inverted themes (middle 4)
    exist for outdoor visibility or psychological warfare on bystanders.
    Retro themes (bottom 2) for the nostalgic freaks who miss CRTs.

    Theme persists across reboots. The pig never forgets a color scheme.


----[ 7.2 - API Keys Setup

    Cloud features need credentials. The pig doesn't store them in
    flash - they live in config after you import from SD.


    WiGLE (wigle.net) - for wardrive uploads:

        1. Create account at wigle.net
        2. Go to Account -> API Token section
        3. Generate or copy your API name and token
        4. Create file on SD card root: /wigle_key.txt
        5. Format: apiname:apitoken (one line, colon separator)
        6. In PORKCHOP: Settings -> < Load WiGLE Key >
        7. File auto-deletes after import

        Now PORK TRACKS menu can upload your wardrives.


    WPA-SEC (wpa-sec.stanev.org) - for distributed cracking:

        1. Register at wpa-sec.stanev.org
        2. Get your 32-character hex API key from profile
        3. Create file on SD card root: /wpasec_key.txt
        4. Contents: just the key, nothing else
        5. In PORKCHOP: Settings -> < Load WPA-SEC Key >
           Or just reboot - auto-imports on boot
        6. File self-destructs after import

        Now LOOT menu can upload handshakes and fetch results.


    Why the file dance? No USB keyboard on the Cardputer for entering
    64 character strings. SD card is faster. File deletion is paranoia.
    The pig doesn't judge your OpSec. The pig just oinks and forgets.


--[ 8 - ML Training Pipeline

    Want to train your own model? Here's the workflow:

----[ 8.1 - Data Collection

    WARHOG mode hoovers up ML training data automatically. Drive around,
    feed the brain. Zero extra effort required.

    How it works:
        - Every network gets 32 features extracted from beacon frames
        - Data accumulates in memory as you drive around
        - Every 60 seconds, WARHOG dumps to /mldata/ - crash protection
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

        normal        = Boring ISP gear doing boring ISP things
        rogue_ap      = Suspiciously loud. Probably evil. Trust issues.
        evil_twin     = Identity theft but make it wireless
        deauth_target = No WPA3/PMF - begging for disconnection
        vulnerable    = Open/WEP/WPS - what decade is this

    The auto-labeler catches the obvious stuff. For real rogue/evil twin
    samples, you gotta set up sketchy APs in the lab and label manually.
    Upload your labeled CSV to Edge Impulse for training.

----[ 8.3 - Training on Edge Impulse

    Edge Impulse does the grunt work. You just click buttons:

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
    |   +-- build_info.h          # Version string, build timestamp
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
    |   |   +-- wigle_menu.cpp/h  # PORK TRACKS - WiGLE file uploads
    |   |   +-- boar_bros_menu.cpp/h  # BOAR BROS - manage excluded networks
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
    |       +-- wigle.cpp/h       # WiGLE wardriving upload client
    |       +-- wpasec.cpp/h      # WPA-SEC distributed cracking client
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

    READ THIS PART, SKID.

    PORKCHOP is for AUTHORIZED SECURITY RESEARCH and EDUCATIONAL USE.
    Period. Full stop. End of discussion.

        * Your networks only. Or written permission. IN WRITING.
        * Deauth attacks are ILLEGAL in most places. Surprise!
        * Wardriving legality varies. Google your jurisdiction.
        * BLE spam is a dick move even if technically legal
        * Authors assume ZERO LIABILITY. Not our problem.
        * If you get caught, you never heard of us

    The law doesn't care that it's "just research" when you're sitting
    in a police station explaining why you were parked outside a bank
    with a directional antenna.

    Your neighbor's WiFi password is not your WiFi password. Your
    neighbor's streaming services are not your streaming services.
    "But I only wanted to test..." is not a legal defense.

    We made a cute ASCII pig that hunts WiFi. We did not make a magic
    immunity talisman against federal telecommunications laws. The pig
    can't testify on your behalf. The pig will not visit you in prison.

    Don't be stupid. Don't be evil. Don't make us regret publishing this.


--[ 11 - Greetz

    Respect to those who came before:

        * evilsocket & pwnagotchi - you started this madness
        * M5Stack - cheap hardware, expensive possibilities
        * Edge Impulse - ML for the rest of us
        * The ESP32 underground - keeping embedded hacking alive
        * Phrack Magazine - the OG zine we're poorly imitating
        * Binrev, 2600, and the scene that won't die
        * You - for scrolling past the legal section

    Special shoutout to the Cardputer-ADV + LoRa testing crew:

        * littlemixy - sacrificed hardware to my broken RX/TX pin configs
        * BansheeBacklash - emotional support through the GPIO nightmare

    These absolute units stuck around while we debugged pin 13 vs 15
    shenanigans on hardware they bought with real money. Their themes
    are immortalized in the firmware. Heroes don't always wear capes,
    sometimes they just have too many ESP32s.

    "The WiFi is free if you're brave enough."
    
    Stay paranoid. Stay curious. Stay out of trouble (mostly).


--[ 12 - Credits

    Developed by: 0ct0
    Team size: 1
    Pronoun of choice: "we"

    There is no team. There never was a team.
    "We" is aspirational. "We" is the dream.
    "We" is what you say when you want to sound
    like a legitimate operation and not just
    some guy debugging frame injection at 3am
    while the pig judges silently from the display.

    Contributors welcome. The pig needs friends.
    The pig's creator needs therapy.
    Both are accepting pull requests.


--[ 13 - Support The Pig

    This project runs on:
        * Mass quantities of caffeine
        * Sleep deprivation
        * The faint hope someone finds this useful

    If PORKCHOP saved you from buying a Flipper,
    cracked a handshake that made you smile,
    or just entertained you for five minutes -
    consider funding the next 3am debug session:

        https://buymeacoffee.com/0ct0

    Your coffee becomes my code.
    My code becomes everyone's pig.
    Circle of life. Hakuna matata. Oink.

    (Not required. Never expected. Always appreciated.
     The pig oinks louder for supporters.)


OINK! OINK!

==[EOF]==

