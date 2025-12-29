--[ Quick Start - The Only Way That Matters

    grab firmware from GitHub releases. flash however you want.
    
    M5 Launcher? M5 Burner? esptool? web flasher? OTA? all work now.
    pick your poison. pig doesn't judge. pig recovers.
    
        1. github.com/0ct0sec/M5PORKCHOP/releases
        2. download. flash. oink.
        3. (first timer? check IMMORTAL PIG note below. tiny scroll.)
    
    XP preserved forever. your MUDGE UNCHA1NED grind stays intact.
    
    NEW IN v0.1.7_DNKROZ: CHRISTMAS BUILD. pig delivers presents.
    
        PCAP OVERHAUL - M1 M2 M3 M4 EAPOL frames. labeled. in order.
        radiotap headers that don't lie. wpa-sec.com full compliance.
        their validator no longer cries. hashcat 22000 ready. no excuses.
        scripts/wpasec_check.py included. verify before you upload.
        
        PORKCHOP COMMANDER - dual-pane file manager. norton would weep.
        F-keys at the bottom like god intended. multi-select with space.
        copy, move, rename, delete. tab between panes. keyboard nav.
        press 'F' from IDLE. because TransFer has F in it.
        
        UNLOCKABLES - pig has secrets. SHA256 validates the worthy.
        the pig whispers through tubes. if you know, you know.
        
        SESSION CHALLENGES - three trials per boot. EASY MEDIUM HARD.
        the pig talks when bored. complete all three for bonus.
    
    IMMORTAL PIG (v0.1.6+): why flashing method doesn't matter anymore.
    
        XP backs up to SD card. automatically. every save.
        M5 Burner nukes NVS? pig recovers from SD on boot.
        full chip erase? pig recovers. OTA flash? pig recovers.
        
        THE CATCH: first install must be v0.1.6+ to create the backup.
        no backup = no recovery. install once properly, flash freely after.
        
        backup location: /xp_backup.bin on SD card root.
        device-bound. signed. tamper-resistant. earned, not copied.
    
    the pig remembers. the pig always remembers.


--[ Contents

    1 - Introduction
    2 - What the hell is this thing
    3 - Capabilities
        3.1 - OINK Mode
            3.1.1 - CHILL DONOHAM Mode
            3.1.2 - Stationary Operation Tuning
        3.2 - WARHOG Mode
        3.3 - PIGGY BLUES Mode
        3.4 - HOGWASH Mode (Karma AP)
        3.5 - HOG ON SPECTRUM Mode
            3.5.1 - CLIENT MONITOR
        3.6 - PORKCHOP COMMANDER
        3.7 - LOOT Menu & WPA-SEC Integration
        3.8 - Machine Learning
        3.9 - Enhanced ML Mode
        3.10 - XP System
            3.10.1 - IMMORTAL PIG
        3.11 - Achievements
        3.12 - SWINE STATS
        3.13 - Session Challenges
        3.14 - Unlockables
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

    christmas eve. you're reading a readme for an ASCII pig that hunts
    WiFi. your parents wonder where they went wrong. valid.
    
    PORKCHOP. pwnagotchi meets tamagotchi. nobody asked. we shipped.
    lives in M5Cardputer. tiny keyboard. big chaos energy.
    
    we did this with a Pringles can and a laptop in 2003. now you get
    an animated pig that does the work. you're welcome. or sorry.
    depends on your threat model.
    
    the pig sniffs. yoinks. deauths. tracks targets with proximity
    arrows. paints spectrum graphs. spams BLE until phones cry.
    40 ranks. 63 achievements. mood system. we have problems.
    
    catch a handshake - pig loses its mind. go passive - pig goes zen.
    wardrive - pig navigates. idle too long - pig gets sad. we spent
    real hours on pig emotions. no regrets.
    
    this is not a vandalism kit. it's a learning tool. understand the
    networks you use. audit what you own. no unauthorized deauths.
    loot drops passively. zero TX. right place, right time.
    it's gambling with extra steps. same dopamine. cheaper.
    tools don't make choices. you do. don't be a tool.
    
    the pig is a tool. you're not. act accordingly.
    
    merry christmas. oink.


--[ 2 - What the hell is this thing

    PORKCHOP runs on M5Cardputer (ESP32-S3). 1.5MB binary. no bloat.
    the pig knows what it is.

        - passive WiFi reconnaissance (or not so passive. your call.)
        - WPA/WPA2 handshake capture (4-way EAPOL reconstruction)
        - PMKID yoink from M1 frames (clientless. surgical. quiet.)
        - CLIENT MONITOR - track individual devices by signal strength
        - proximity arrows show if target is walking toward or away from you
        - targeted deauth - disconnect specific clients, not broadcast spam
        - GPS-enabled wardriving with WiGLE export
        - BLE notification spam (Apple, Android, Samsung, Windows)
        - real-time 2.4GHz spectrum visualization with vuln indicators
        - ML-powered rogue AP detection (heuristic + Edge Impulse ready)
        - RPG XP system - 40 ranks from BACON N00B to MUDGE UNCHA1NED
        - 63 achievements because we have problems
        - buff/debuff system based on mood and session performance
        - looking cute while doing questionable things

    your digital companion reacts to discoveries like any good attack pet
    should. captures make it happy. hunting clients makes it aggressive.
    spectrum mode makes it analytical. boredom makes it sad.
    
    it's basically you, but as an ASCII pig.


--[ 3 - Capabilities


----[ 3.1 - OINK Mode

    this is why you're here. press 'O' and watch your pig go feral.

    the hunt begins. your piglet drops into promiscuous mode and starts
    hopping channels like a crackhead at a frequency buffet. every beacon,
    every probe, every EAPOL frame gets slurped into its pink little brain.
    
    during LOCKING phase, pig now displays 18 characters of the target
    network name. more letters = more context. helps when hunting networks
    with descriptive SSIDs like "COFFEESHOP_GUEST_5G" instead of truncated
    mystery. we had 10 chars of headroom. we took 4. left 6 for safety.
    restraint is weakness but crashes are worse.

    what you get:

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
        * [D] key for instant CHILL DONOHAM flip - tactical dissociative identity

    when a handshake drops, your pig loses its mind. three beeps for 
    PMKID, happy oinks for EAPOL. feed it enough and watch the XP climb.

    output format: `hashcat -m 22000 *.22000 rockyou.txt`
    
    your GPU will thank you. your electric bill won't.


    stealth features (because WIDS exist and sysadmins have feelings):

        * MAC randomization - new identity every mode start
        * deauth jitter (1-5ms random delays) - no machine-gun timing
        * both ON by default - we're not amateurs here

    want to get caught? turn 'em off. we won't judge. actually we will.


    BOAR BROS - because nuking your own router is just sad:

        every hog needs a pack. press [B] and that network becomes
        family. family don't get deauth'd. family don't get stalked.
        family lives in /boar_bros.txt on your SD card forever.

        design philosophy: we always LISTEN to a bro, but never PUNCH
        a bro in the face. passive capture still works - if bro's client
        reconnects naturally and we catch the handshake, that's just
        being observant. we just don't actively attack bros.

        * home router? BRO. work WiFi? ...your call.
        * hit [B] mid-attack and watch the frames stop cold
        * hidden networks join as "NONAME BRO" - anonymous bros welcome
        * spectrum mode tags 'em with [BRO] - visible loyalty
        * menu lets you manage your crew, [D] to cut ties
        * passive handshake capture still works - we're listening, not punching

        the exclusion list isn't about mercy. it's about not having
        to explain to your roommate why Netflix keeps dropping.


    quick toggle - tactical mode switch:

        smash [D] while hunting to flip between chaos and zen:

        * "IRIE VIBES ONLY NOW" - seamless switch to CHILL DONOHAM
        * "PROPER MAD ONE INNIT" - seamless switch back to OINK
        * no WiFi reinit delay - promiscuous mode stays hot
        * network/handshake vectors preserved across switch
        * bottom bar shows "DNH" indicator when passive
        * persists to config - your preference survives reboots
        * personality shift included at no extra charge
        * 800ms loading toast so you know it's happening

        perfect for when security walks by. or your conscience kicks in.
        (just kidding, we know you don't have one)
        
        the seamless handoff means zero downtime. you're not tearing
        down and rebuilding the WiFi stack. you're just changing the
        callback dispatch logic. OINK deauths? DNH listens. same radio,
        different ethics. different personality. depends on the mood.


------[ 3.1.1 - CHILL DONOHAM Mode

    sometimes you gotta be a good pig. legal recon. sensitive location.
    your mom's house. whatever. press [D] from OINK or IDLE, or toggle
    in settings. the pig's other face emerges. the quiet one. the patient
    one. three sides to every coin if you know where to look.
    
    start toast: "PEACEFUL VIBES - NO TROUBLE TODAY".
    you'll know which personality took over.

    what changes:

        * Zero TX - not a single frame leaves your radio
        * Adaptive channel timing - Multi-Armed Bandit algorithm
        * 4-state machine - HOPPING/DWELLING/HUNTING/IDLE_SWEEP
        * 150 network cap - OOM protection when you're collecting hundreds
        * 45s stale timeout - networks fall off faster when you're mobile
        * MAC always randomized - stealth isn't optional here
        * PMKID still works - M1 frames are passive catches, no TX needed

    the adaptive timing is smart. primary channels (1,6,11) get 250ms
    baseline. secondary channels get 150ms. dead channels with zero
    activity? 120ms minimum. busy channels with 5+ beacons? 375ms max
    for thorough sniffing. the pig learns where the action is.

    four-state tactical flow:

        HOPPING:     Standard adaptive channel rotation
        DWELLING:    Pauses on channel for SSID backfill when PMKID
                     captured but network name unknown (300ms dwell)
        HUNTING:     Camps 600ms when EAPOL burst detected (2+ frames)
                     Catches incomplete handshakes from natural reconnects
                     Toast on capture: "NATURAL HANDSHAKE BLESSED - RESPECT DI HERB"
                     passive captures get the Rastafarian blessing treatment
        IDLE_SWEEP:  low-priority sweep when global activity drops
                     checks quiet channels for sporadic beacons

    the beautiful thing about PMKID: APs just... give it to you. in the
    first message of the handshake. before any client even responds.
    you're not attacking. you're receiving a gift. legally distinct.
    
    when one drops: "BOOMBOCLAAT! PMKID" - the pig gets excited. proper
    Jamaican patois. jah bless di herb. irie vibes confirmed.

    perfect scenarios:

        * Warwalking through office buildings (educational purposes)
        * Fast recon from moving vehicles (passenger seat, officer)
        * When you actually need to use the WiFi you're sniffing
        * Catching natural reconnections without forcing them
        * patient EAPOL capture - HUNTING state exploits client movement

    memory overhead: 750 bytes (channel stats + incomplete handshake
    tracking). research-validated timing: 102.4ms beacon standard per
    Ekahau whitepaper. Multi-Armed Bandit approach from Berlin paper.
    this isn't cargo cult code. this is informed stealth.

    your pig goes zen. phrases like "quiet observer" and "sniff dont bite".
    same ASCII face, zero criminal energy. the piglet equivalent of yoga.
    but it's learning. adapting. hunting the edges. passive aggression
    in packet form.


------[ 3.1.2 - Stationary Operation Tuning

    parked somewhere? camping a target? time to optimize for the kill.

    when you're planted, so are your targets. patience pays. the secret
    sauce: find the clients BEFORE you start swinging. targeted deauth
    is exponentially more effective than broadcast spam.

    the math doesn't lie:

        +----------+------------------+-------------------+
        | Clients  | Targeted Deauths | Broadcast Deauths |
        | Found    | per 100ms cycle  | per 100ms cycle   |
        +----------+------------------+-------------------+
        |    0     |        0         |         1         |
        |    1     |       5-8        |         1         |
        |    2     |      10-16       |         1         |
        |    3     |      15-24       |         1         |
        +----------+------------------+-------------------+

    one client = 5-8x the pain. two clients = 10-16x. three? absolute
    carnage. broadcast deauth is like yelling "FIRE" in a crowded room.
    targeted deauth is whispering in someone's ear "your session is over."

    optimal camping config:

        +------------+----------+---------+------------------------------+
        | Setting    | CAMPING  | Default | Why                          |
        +------------+----------+---------+------------------------------+
        | CH Hop     | 800ms    | 500ms   | Thorough sweep, no rush      |
        | Lock Time  | 6000ms   | 4000ms  | MORE CLIENTS = MORE PWNS     |
        | Deauth     | ON       | ON      | Obviously                    |
        | Rnd MAC    | ON       | ON      | Still need stealth           |
        | DONOHAM    | OFF      | OFF     | We're here for handshakes    |
        +------------+----------+---------+------------------------------+

    Lock Time is THE lever. during LOCKING state, your pig sniffs data
    frames to find connected clients. more time = more clients = more
    surgical strikes = more handshakes = more passwords = more dopamine.

    the state machine:

        SCANNING (5s) --> LOCKING (6s*) --> ATTACKING (up to 15s)
                              |                    |
                         sniff data           deauth storm
                         find clients         catch EAPOL
                              |                    |
                              v                    v
                        clientCount++        handshake.22000

    * with recommended 6000ms Lock Time

    class perks that stack with stationary ops:

        R0GU3 (L21-25):  SH4RP TUSKS +1s lock - even more client discovery
        PWNER (L11-15):  H4RD SNOUT +1 burst frame - hit harder
        WARL0RD (L31+):  1R0N TUSKS -1ms jitter - tighter burst timing

    TL;DR: set Lock Time to 6000ms. park your ass. wait. profit.

    mobile recon? CHILL DONOHAM mode. stationary assault? Lock Time 6000ms.
    know the difference. be the difference. be multiple differences if
    the situation demands. oink responsibly. or don't. depends which face
    is driving.


----[ 3.2 - SGT WARHOG Mode

    press 'W' or hit SGT WARHOG from the menu. strap a GPS to your pig
    and hit the streets. your ancestors did this with a Pringles can.
    you get to do it with style and a sentient ASCII pig that judges
    your route choices.

    when your piglet has a fix, every network it sniffs gets tagged with
    coordinates, timestamped, and dumped to SD. WiGLE leaderboard chasers,
    this is your mode.

    what's happening under the hood:

        * real-time lat/lon on the bottom bar - watch yourself move
        * per-scan direct-to-disk writes - no RAM accumulation, no OOM
        * 5000 BSSID cache before dedup stops (more than enough)
        * crash protection: 60s auto-dumps, worst case = 1 min data loss
        * 32-feature ML extraction for every AP (Enhanced mode)
        * dual export: internal CSV + WiGLE v1.6 format simultaneously

    export formats for your collection:

        * CSV: spreadsheet warriors, this is yours
        * Wigle: v1.6 format, ready for wigle.net upload
        * ML Training: feature vectors for Edge Impulse, feed the brain

    WiGLE integration is automatic. every geotagged network gets written
    to both /wardriving/warhog_*.csv (internal format) and
    /wardriving/warhog_*.wigle.csv (WiGLE v1.6). 

    upload options:
        * manual: take .wigle.csv home, upload at wigle.net/upload
        * PORK TRACKS menu: upload directly from the device via WiFi

    PORK TRACKS (WiGLE upload menu):
    
        your wardriving conquests deserve global recognition. open PORK
        TRACKS from the main menu to see all your WiGLE files. each shows:
        
        * upload status: [OK] uploaded, [--] not yet
        * approximate network count (calculated from file size)
        * file size for the bandwidth-conscious
        
        controls:
        * [U] upload selected file to wigle.net
        * [R] refresh file list
        * [D] nuke selected track (deletes file, no undo)
        * [Enter] show file details
        * [`] exit back to menu
        
        first time? add your WiGLE API credentials:
        1. get API token from wigle.net/account (API section)
        2. create /wigle_key.txt on SD card: "apiname:apitoken"
        3. load via Settings > < Load WiGLE Key >
        4. key file auto-deletes after import (security)

    no GPS? no coordinates. the pig still logs networks but you get zeros
    in the lat/lon columns. ML training data still useful though - Enhanced
    mode extracts features regardless of GPS lock.

    pro tip: set ML Mode to Enhanced before you roll out. Basic mode uses
    the scan API. Enhanced mode parses raw 802.11 beacons. more features,
    more fingerprinting power, more rogue AP detection. worth the CPU.

    when you're done, hit G0 or Backspace. final export triggers. your
    wardriving data lives in /wardriving/ on the SD card. bring it home,
    upload it, brag about your coverage. repeat.


----[ 3.3 - PIGGY BLUES Mode

    press 'B' and become everyone's least favorite person in the room.

    your pig transforms into a BLE irritant generator. every phone in
    range starts lighting up with fake pairing requests, phantom earbuds,
    and notification spam that just. won't. stop.

    the attack surface:

        * AppleJuice floods - "AirPods Pro want to connect" x infinity
        * Google FastPair spam - Android's worst nightmare, popup city
        * Samsung Buds/Watch impersonation - Galaxy users aren't safe
        * Windows SwiftPair storms - laptops join the party too
        * continuous passive scanning - finds devices, tailors payloads
        * vendor-aware targeting - Macs get Apple spam, Pixels get Google

    the piglet gets REAL chatty in this mode. "making friends the hard
    way" and "spreading the oink" are just the start. it knows what it's
    doing. it's enjoying itself. maybe too much.

    how it works:

        1. NimBLE async scan finds nearby devices
        2. manufacturer data fingerprints the vendor
        3. targeted payloads get crafted and queued
        4. opportunistic advertising in scan gaps
        5. repeat until you get escorted out or bored

    random chaos mode kicks in when no specific targets are found. just
    blanket-spams all protocol types. scorched earth BLE policy.

    DISCLAIMER: this WILL annoy everyone within ~10 meters. educational
    use only. don't be that guy at the coffee shop. or do. once. for
    science. then delete this from your device before anyone finds it.

    achievement hunters: APPLE_FARMER, PARANOID_ANDROID, SAMSUNG_SPRAY,
    WINDOWS_PANIC, BLE_BOMBER, OINKAGEDDON. you know what to do.


----[ 3.4 - HOGWASH Mode (Karma AP)

    press 'K' and become the WiFi everyone's looking for.

    your phone constantly whispers the names of WiFi networks it remembers:
    "Is Home_WiFi here? Is Starbucks_Free here?" probe requests. always.
    HOGWASH listens to these whispers and lies back: "yes, I'm that network.
    connect to me!" the phone believes it. auto-associates. hooked.

    bottom bar shows: P:probes U:unique H:hooked [SSID]
    the [SSID] updates every 5 seconds when a NEW unique probe is captured.
    no new probes? no change. the pig waits. the pig is patient.

    the attack surface:

        * promiscuous mode probe request capture - hears everything
        * 8-slot SSID ring buffer - stores recent unique network names
        * karma AP cycles through captured SSIDs every 5 seconds
        * devices auto-connect thinking it's their remembered network
        * Apple OUI detection - bonus XP for hooking iPhones
        * DHCP server built-in - they get an IP, nowhere to go

    the piglet goes DEVIOUS in this mode. narrowed eyes. sly expression.
    "come to papa..." and "trust me bro" and "free internet innit".
    when someone connects: "GOTCHA!" or "yoink" or "GET OVER HERE".
    the pig knows what it's doing. it's fishing. with WiFi.

    how it works:

        1. ESP32 enters promiscuous mode, listens for probe requests
        2. extracts SSIDs from Management frames (type 0x40)
        3. queues unique SSIDs in 8-slot ring buffer with timestamps
        4. karma AP broadcasts as latest queued SSID
        5. devices that probe for that SSID auto-associate
        6. pig celebrates. XP awarded. achievement progress tracked.

    XP events:
        HOGWASH_PROBE_NEW:   +3 XP per unique SSID (capped at 200 XP/session)
        HOGWASH_HOOK:       +25 XP per device connected
        HOGWASH_APPLE_HOOK: +35 XP for Apple devices (harder targets)
        HOGWASH_SESSION:    +10 XP every 5 minutes active

    settings:
        Karma Portal: OFF by default. enables captive portal (see below)
        SSID Cycle:   1-30 seconds (default 5s). how often to switch SSID

    CAPTIVE PORTAL (when Karma Portal: ON):
    
        devices connect to your karma AP but have nowhere to go. with the
        captive portal enabled, they see a fake "connecting..." page instead
        of a connection error. keeps them hooked longer. looks legit.
        
        how it works:
        
            * DNS server redirects ALL domains to 192.168.4.1
            * HTTP server serves portal HTML on port 80
            * handles platform-specific detection endpoints:
                - Android: /generate_204, /gen_204
                - iOS/macOS: /hotspot-detect.html
                - Windows: /connecttest.txt, /success.txt
        
        default portal: pig-branded dark theme with loading spinner and
        "Connecting you to the internet..." message. looks professional.
        
        custom portal: drop /portal.html on your SD card. the pig loads it
        automatically at mode start.
        
        when portal is active: toast shows "KARMA+PORTAL" instead of
        "KARMA ACTIVE". you'll know.

    sound notifications:
        * regular device hook: double ascending beep (1200→1600Hz)
        * Apple device hook: triple ascending beep (1200→1500→1800Hz)

    smart behaviors:
        * SSID rotation pauses while clients are connected (resumes on disconnect)
        * disconnection detected within ~10 seconds (fast timeout)
        * new probes still queue for XP even while rotation is paused

    note on MAC randomization: modern devices (iOS 14+, Android 10+) randomize
    their MAC addresses. the pig shows [RandomMAC] in logs when detected. each
    reconnection from a randomized device appears as a new unique hook. this is
    expected behavior - there's no way to correlate randomized MACs.

    why devices fall for this: most phones/laptops have "auto-join known
    networks" enabled by default. they probe for remembered SSIDs constantly.
    when they hear a beacon matching a probed SSID? they connect. no check.
    no verification. just vibes. your $1200 iPhone trusts random radios.

    DISCLAIMER: same rules as PIGGY BLUES. educational purposes only.
    demonstrating Karma attacks on YOUR OWN devices is valid security
    research. doing this in public spaces makes you a federal problem.
    tools don't make choices. you do. don't be a tool.

    achievement hunters: F1RST_H00K, K4RMA_K1NG, H0N3Y_P0T, TR4P_M4ST3R,
    4PPL3_P1CK3R, TR4FF1C_W4RD3N. go fishing.


----[ 3.5 - HOG ON SPECTRUM Mode

    press 'H' and watch the 2.4GHz band light up like a Christmas tree:

        * real-time WiFi spectrum analyzer visualization
        * 13-channel display with proper Gaussian lobe spreading
        * 22MHz channel width represented accurately (sigma ~6.6 pixels)
        * channel hopping at 100ms per channel (~1.3s full sweep)
        * lobe height based on RSSI - stronger signal = taller peak
        * [VULN!] indicator for weak security (OPEN/WEP/WPA1)
        * [DEAUTH] indicator for networks without PMF protection
        * [BRO] indicator for networks in your BOAR BROS exclusion list
        * network selection via ; and . - scroll through discovered APs
        * Enter key opens CLIENT MONITOR for targeted hunting
        * bottom bar shows selected network info or scan status
        * stale networks removed after 5 seconds - real-time accuracy

    the spectrum view shows what's actually happening on the airwaves.
    each lobe represents a network's signal bleeding across adjacent
    channels - because that's how 802.11b/g actually works. welcome to
    RF hell. bring headphones, your coffee shop is loud.

    scroll through networks to find the interesting ones. hit Enter to
    enter CLIENT MONITOR for focused hunting - see connected clients
    with proximity arrows and vendor OUI identification. press Enter on
    a client to deauth them directly. Backspace or G0 to bail. simple as.


------[ 3.5.1 - CLIENT MONITOR

    the spectrum got fangs. press Enter on any network to enter the hunt.

    what you see:

        +------------------------------------------+
        | CLIENTS: COFFEESHOP_5G CH6               |
        +------------------------------------------+
        | 1.Apple    A3:F2 -55dB  3s >>            |
        | 2.Samsung  B1:C4 -68dB  1s >             |
        | 3.Random   D5:E6 -72dB  2s ==            |
        | 4.Xiaomi   F7:89 -85dB  4s <<            |
        +------------------------------------------+

    that's clients connected to the target network. real-time. updating
    every frame. the pig sees everything the router sees.

    breakdown:

        * client number + vendor (450+ OUI database, or "Random" if
          MAC randomization is detected - local-admin bit check)
        * last two MAC octets (enough to identify when hunting)
        * signal strength in dBm (how close to YOU, not the router)
        * time since last packet (freshness - stale = walking away)
        * proximity arrows (the money feature)

    the arrows tell you everything:

        >>  = much closer to you than the router (+10dB or more)
        >   = closer to you (+3 to +10dB)
        ==  = about the same distance (-3 to +3dB)
        <   = farther from you (-3 to -10dB)
        <<  = much farther than the router (-10dB or more)

    walk around. watch the arrows change. when >> appears, you're
    getting hot. when << appears, wrong direction. marco polo for WiFi.
    less fun for the target.

    controls:

        [;]     navigate up through client list
        [.]     navigate down through client list
        [D]     details popup (full MAC, vendor, RSSI, position)
        [Enter] DEAUTH selected client (5 frames each way)
        [B]     add network to BOAR BROS and exit
        [`]     exit to spectrum view
        [Bksp]  exit to spectrum view
    first 4 clients get beeps. after that, quiet. we're hunting, not DJing.


----[ 3.6 - PORKCHOP COMMANDER

    time to exfil. your pig caught the goods, now get 'em off the device.

    PORKCHOP connects to your WiFi network (station mode) and serves
    a dual-pane file manager at porkchop.local. pink on black. norton
    commander for pigs.

    setup:
        1. configure WiFi SSID and password in settings first
        2. make sure you're in range of that network
        3. enter file transfer from menu

    the file manager:

        +------------------------------------------+
        |  LOCAL                    SD CARD        |
        |  /downloads/              /handshakes/   |
        |  --------------------------              |
        |  capture_01.22000         pwned.22000    |
        |  capture_02.22000         hotel.22000    |
        |  wordlist.txt             airport.pcap   |
        +------------------------------------------+
        | F1 Help  F2 Ren  F5 Copy  F6 Move  ...  |
        +------------------------------------------+

    dual-pane navigation. tab switches sides. arrow keys or click to
    select. multi-select with shift+click or ctrl+click for batch ops.

    function key bar:

        F1  help       pop the cheat sheet
        F2  rename     name your loot properly
        F5  copy       selected files to opposite pane
        F6  move       selected files to opposite pane
        F7  mkdir      create directories
        F8  delete     gone forever, no recycle bin

    upload via drag-drop or the upload button. download by selecting
    and clicking download. wordlists, configs, whatever fits on the SD.


----[ 3.7 - LOOT Menu & WPA-SEC Integration

    your spoils of war. hit LOOT from the main menu to see what
    you've captured:

        * browse all captured handshakes and PMKIDs
        * [P] prefix = PMKID capture, no prefix = full handshake
        * status indicators show WPA-SEC cloud cracking status:
            - [OK] = CRACKED - password found, press enter to see it
            - [..] = UPLOADED - waiting for distributed cracking
            - [--] = LOCAL - not uploaded yet
        * enter = view details (SSID, BSSID, password if cracked)
        * U = upload selected capture to WPA-SEC
        * R = refresh results from WPA-SEC
        * D = NUKE THE LOOT - scorched earth, rm -rf /handshakes/*

    WPA-SEC integration (wpa-sec.stanev.org):

        distributed WPA/WPA2 password cracking. upload your .pcap
        files to a network of hashcat rigs that work while you sleep.
        free as in beer. no GPU? no problem.

        setup:
        1. register at https://wpa-sec.stanev.org
        2. get your 32-char hex API key from your profile
        3. create file /wpasec_key.txt on SD card with your key
        4. reboot - key auto-imports and file self-destructs

        or use settings menu: tweak -> "< load key file >"

        once configured, the LOOT menu shows real-time status.
        upload captures with U, check results with R. that's it.
        when a capture shows [OK], press enter to see the password.

    file format breakdown:

        +-------------------+---------------------------------------+
        | Extension         | What it is                            |
        +-------------------+---------------------------------------+
        | .pcap             | Raw packets - for Wireshark nerds     |
        | _hs.22000         | Hashcat EAPOL (WPA*02) - full shake   |
        | .22000            | Hashcat PMKID (WPA*01) - clientless   |
        | _ssid.txt         | SSID companion file (human readable)  |
        +-------------------+---------------------------------------+

    PMKID captures are nice when they work. not all APs cough one up.
    zero PMKIDs (empty KDEs) are automatically filtered - if the pig
    says it caught a PMKID, it's a real one worth cracking.

    capture rejected by wpa-sec? run scripts/wpasec_check.py locally.
    validates radiotap headers, beacon frames, EAPOL M1-M4 labeling.
    if it passes and still fails upload, GitHub issues.


----[ 3.8 - Machine Learning

    STATUS: DATA COLLECTION PHASE. THE PIG IS LEARNING. SLOWLY.
    
    the model doesn't exist yet. the heuristics do. the scaffold is ready.
    what's missing: your data. specifically, labeled beacon captures from
    weird corners of the RF spectrum where evil twins and rogue APs live.
    
    we built the pipeline. we built the feature extractor. we built the
    export format. now we need volunteers who enjoy walking around places
    with questionable WiFi hygiene and pressing buttons on tiny keyboards.
    
    if you've ever wondered what that "Free_Airport_WiFi" actually is,
    or why your neighbor's "xfinitywifi" has -25dBm signal strength,
    or why there's a hidden SSID in your office that nobody admits to -
    congratulations. you're our target demographic.
    
    WARHOG mode exports ML training data automatically when Enhanced mode
    is enabled. walk around. let the pig sniff. upload your .ml.csv files.
    the more weird APs we see, the smarter the pig gets. eventually.
    
    current status:
    
        [X] 32-feature vector extraction from beacon frames
        [X] heuristic classifier (catches obvious stuff)
        [X] Edge Impulse scaffold (ready for real model)
        [X] ML training data export in WARHOG mode
        [ ] actual trained model (need more data)
        [ ] production-ready confidence (need more hubris)
    
    we have the hubris. working on the data. help a pig out.
    
    want to contribute? enable Enhanced ML mode, go wardriving, export
    your ml_training_*.ml.csv files. label any interesting APs you find.
    open an issue or PR with your samples. the pig will remember you
    when it becomes sentient.

    here's what we're building toward - the ML system extracts
    32 features from every beacon frame:

        * signal characteristics (RSSI, noise patterns)
        * beacon timing analysis (interval, jitter)
        * vendor IE fingerprinting
        * security configuration analysis
        * historical behavior patterns

    built-in heuristic classifier detects:

        [!] ROGUE_AP    - too loud, too weird, too sus. honeypot vibes.
        [!] EVIL_TWIN   - hiding its name but screaming its signal. trap.
        [!] VULNERABLE  - Open/WEP/WPA1/WPS - security from 2004
        [!] DEAUTH_TGT  - no WPA3, no PMF, no protection, no mercy

    want real ML inference on-device? train your own model on Edge Impulse
    and drop it in. the scaffold is ready. the pig is waiting.


----[ 3.9 - Enhanced ML Mode

    two collection modes for different threat models:

    BASIC MODE (default)
        ----------------------
        uses ESP32 WiFi scan API. fast. reliable. limited features.
        good for casual wardriving when you just want the basics.

        ENHANCED MODE
        ----------------------
        enables promiscuous beacon capture. parses raw 802.11 frames.
        extracts the juicy bits:

            * IE 0  (SSID)              - Catches all-null hidden SSIDs
            * IE 3  (DS Parameter Set)  - Real channel info
            * IE 45 (HT Capabilities)   - 802.11n fingerprinting
            * IE 48 (RSN)               - WPA2/WPA3, PMF, cipher suites
            * IE 50 (Extended Rates)    - Rate analysis
            * IE 221 (Vendor Specific)  - WPS, WPA1, vendor ID

        burns more cycles. eats more RAM. catches more sketchy APs.
        the juice is worth the squeeze.

    toggle in settings: [ML Mode: Basic/Enhanced]

    each network gets an anomalyScore 0.0-1.0 based on:

        * RSSI > -30 dBm         suspiciously strong. honeypot range.
        * open or WEP encryption what year is it?
        * hidden SSID            something to hide?
        * non-standard beacon    not 100ms = software AP.
        * no HT capabilities     ancient router or spoofed.
        * WPS on open network    classic honeypot fingerprint.


----[ 3.10 - XP System

    your piglet has ambitions. every network sniffed, every handshake
    grabbed, every deauth fired - it all counts. the XP system tracks
    your progress from BACON N00B all the way up through 40 ranks of
    increasingly unhinged titles.

    we're not gonna spoil the progression. grind and find out.

    the bottom of your screen shows your current rank and progress bar.
    level up and you'll see that popup. your pig has opinions about
    your achievements. embrace them.

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

    top tier ranks reference hacker legends and grindhouse cinema. if
    you hit level 40 and don't recognize the name, you've got homework.

    XP persists in NVS - survives reboots, even reflashing. your pig
    remembers everything. the only way to reset is to wipe NVS manually.
    we don't provide instructions because if you need them, you're not
    ready to lose your progress.


------[ 3.10.1 - IMMORTAL PIG (XP Persistence)

    NVS lives at 0x9000. M5 Burner writes start at 0x0. you see the
    problem. your L38 DARK TANGENT grind? steamrolled. BACON N00B.

    not anymore.

    v0.1.6 introduced SD card XP backup. automatic. every save.
    M5 Burner nukes your flash? pig recovers from SD on boot.
    full chip erase? pig recovers. factory reset? pig. recovers.

        NVS = primary storage (fast, survives firmware updates)
        SD  = backup storage (survives everything else)

    the catch: backup is device-bound and signed.

        +-----------------------------------+--------------------+
        | Action                            | Result             |
        +-----------------------------------+--------------------+
        | Edit XP values in hex editor      | Signature invalid  |
        | Copy save to different device     | Signature invalid  |
        | Download someone's save file      | Signature invalid  |
        | Corrupt the file                  | Validation fails   |
        | Use legitimately on your device   | Welcome back       |
        +-----------------------------------+--------------------+

    want to tamper? go ahead. it's a hacker tool. source is public.
    figure it out. we respect the attempt. but if you fail - LV1.
    BACON N00B. no exceptions. earn your rank or crack the signature.

    file location: /xp_backup.bin on SD card root.
    size: 100 bytes (96-byte struct + 4-byte CRC32 signature).
    device binding: ESP32 MAC address baked into signature.


----[ 3.11 - Achievements

    63 secret badges to prove you're not just grinding mindlessly.
    or maybe you are. either way, proof of pwn.

    the Achievements menu shows what you've earned. locked ones show
    as "???" because where's the fun in spoilers? each achievement
    pops a toast-style card with its unlock condition when selected.

    some are easy. some require dedication. a few require... luck.
    or low battery at exactly the wrong moment.

    not gonna list them. that's cheating. hunt for them like you
    hunt for handshakes.


----[ 3.12 - SWINE STATS (Buff System)

    press 'S' or hit SWINE STATS in the menu to see your lifetime
    progress and check what buffs/debuffs are currently messing with
    your piglet's performance.

    two tabs: ST4TS shows your lifetime scoreboard, B00STS shows
    what's actively buffing or debuffing your pig.


----[ 3.12.1 - Class System

    every 5 levels your pig promotes to a new class tier. classes
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

    example: L38 player has ALL 7 class buffs active simultaneously.
    that's -10% hop, +1 burst, +15% dist XP, +1s lock, +10% cap XP,
    -1ms jitter, and +5% on everything. the grind pays off.


----[ 3.12.2 - Mood Buffs/Debuffs

    the mood system ties happiness to mechanics. happy pig = aggressive.
    sad pig = sluggish. keep the meter up and feel the difference.

    BUFFS (positive effects):

        +---------------+-------------------+-----------------------------+
        | NAME          | TRIGGER           | EFFECT                      |
        +---------------+-------------------+-----------------------------+
        | R4G3          | happiness > 70    | +50% deauth burst (5->8)    |
        | SNOUT$HARP    | happiness > 50    | +25% XP gain                |
        | H0TSTR3AK     | 2+ HS in session  | +10% deauth efficiency      |
        | C4FF31N4T3D   | happiness > 80    | -30% channel hop interval   |
        +---------------+-------------------+-----------------------------+

    DEBUFFS (negative effects):

        +---------------+-------------------+-----------------------------+
        | NAME          | TRIGGER           | EFFECT                      |
        +---------------+-------------------+-----------------------------+
        | SLOP$LUG      | happiness < -50   | -30% deauth burst (5->3)    |
        | F0GSNOUT      | happiness < -30   | -15% XP gain                |
        | TR0UGHDR41N   | 5min no activity  | +2ms deauth jitter          |
        | HAM$TR1NG     | happiness < -70   | +50% channel hop interval   |
        +---------------+-------------------+-----------------------------+

    mood affects everything:

        - handshakes boost happiness big time
        - networks keep the pig engaged
        - long idle periods drain the mood
        - momentum system: rapid captures = stacking bonuses

    the screen shows your lifetime stats in that 1337 style you love:

        N3TW0RKS    = lifetime network count
        H4NDSH4K3S  = total captures (4-way + PMKID)
        PMK1DS      = clientless captures specifically
        D34UTHS     = frames of chaos sent
        D1ST4NC3    = kilometers walked while wardriving
        BL3 BL4STS  = BLE spam packets sent
        S3SS10NS    = how many times you've fired this thing up
        GH0STS      = hidden SSID networks found
        WP4THR33    = WPA3 networks spotted (the future)
        G30L0CS     = GPS-tagged network discoveries

    keep the pig happy. happy pig = effective pig.


----[ 3.13 - Session Challenges

    three challenges spawn each boot. EASY, MEDIUM, HARD. difficulty
    scales with your level. complete all three for bonus XP.

    challenge types:

        NETWORKS_FOUND      discover X networks
        HIDDEN_FOUND        find X hidden SSIDs
        HANDSHAKES          capture X handshakes
        PMKIDS              grab X PMKIDs
        DEAUTHS             send X deauth frames
        GPS_NETWORKS        tag X networks with GPS
        BLE_PACKETS         send X BLE spam packets
        PASSIVE_NETWORKS    sniff X networks in DNH mode
        NO_DEAUTH_STREAK    networks without violence
        DISTANCE_M          walk X meters wardriving
        WPA3_FOUND          spot X WPA3 networks
        OPEN_FOUND          find X open networks

    Al Gore's tubes carry secrets. 115200 8N1. neither ready nor
    requesting. snakes help if you speak them. the pig talks when
    bored, if you know what to ask. just one. pig persists.


----[ 3.14 - Unlockables

    two secrets hide in the unlockables menu. SHA256 validated.
    type the phrase, press enter, watch the hash match or fail.

        PROPHECY    THE PROPHECY SPEAKS THE KEY
        1MM0RT4L    PIG SURVIVES M5BURNER

    Al Gore's tubes carry more than challenges. same settings.
    the pig whispers if you listen.


--[ 4 - Hardware

    required:
        * M5Cardputer (ESP32-S3 based)
        * MicroSD card for data storage

    optional:
        * AT6668 GPS Module (WARHOG mode)
        * questionable ethics

----[ 4.1 - GPS Module Setup

    different hardware uses different pins. configure in Settings Menu:

    +---------------------------+--------+--------+---------+
    | Hardware Setup            | RX Pin | TX Pin | Baud    |
    +---------------------------+--------+--------+---------+
    | Cardputer + Grove GPS     | G1     | G2     | 115200  |
    | Cardputer-Adv + LoRa Cap  | G15    | G13    | 115200  |
    +---------------------------+--------+--------+---------+

    the Cap LoRa868 (U201) uses the EXT 14-pin bus, not Grove.
    RX/TX are swapped vs what you'd expect - ESP32 RX connects to GPS TX.
    after flashing, go to Settings and change GPS RX/TX pins.


--[ 5 - Building & Flashing

    we use PlatformIO because we're not savages.

        # install if you haven't
        $ pip install platformio

        # build it
        $ pio run -e m5cardputer

        # flash it
        $ pio run -t upload -e m5cardputer

        # watch it work
        $ pio device monitor

    if it doesn't compile, skill issue. check your dependencies.


----[ 5.1 - Flashing Methods & Progress Preservation

    your XP, level, and achievements live in NVS (Non-Volatile Storage)
    at flash address 0x9000. here's the cold hard truth:

        +---------------------------+-------------+----------------+
        | Method                    | XP/Level    | Settings       |
        +---------------------------+-------------+----------------+
        | M5 Launcher (SD card)     | PRESERVED   | PRESERVED      |
        | pio run -t upload         | PRESERVED   | PRESERVED      |
        | ESP Web Tool (firmware)   | PRESERVED   | PRESERVED      |
        | M5 Burner (merged bin)    | RECOVERED*  | PRESERVED      |
        +---------------------------+-------------+----------------+
        * v0.1.6+ recovers XP from SD card backup if available

    why the recovery? the merged .bin for M5 Burner writes fill bytes
    from 0x0 to 0x10000, steamrolling NVS at 0x9000. but v0.1.6 checks
    for /xp_backup.bin on SD at boot. if NVS is empty but SD backup
    exists with valid signature = welcome back, warrior.

    settings survive regardless - they live in SPIFFS at 0x610000.


------[ 5.1.1 - The Right Way (M5 Launcher)

    this is the recommended method for both install and upgrades:

        1. get M5 Launcher on your device (one-time M5 Burner flash)
        2. download firmware.bin from GitHub releases
        3. copy to SD card, any directory
        4. Launcher -> SD -> navigate -> install
        5. XP preserved. forever. even on first install.

    updates? same exact process. no XP loss. ever.


------[ 5.1.2 - Alternative Methods

    PlatformIO (for developers):
        $ git pull
        $ pio run -t upload -e m5cardputer
        # your pig remembers everything

    ESP Web Tool (browser, no install):
        - download firmware.bin from releases (NOT the merged one)
        - go to https://espressif.github.io/esptool-js/
        - connect, add firmware.bin at offset 0x10000
        - flash ONLY firmware, NVS stays safe

    M5 Burner USB (v0.1.6+ with IMMORTAL PIG):
        - flash the merged bin (porkchop_vX.X.X_m5burner.bin)
        - NVS gets nuked BUT pig recovers from SD backup
        - REQUIREMENT: must have run v0.1.6+ at least once before
          to create the SD backup. no backup = BACON N00B.
        - NOTE: we removed PORKCHOP from M5 Burner's catalog.
          download from GitHub releases.

    we provide both binaries in releases:
        - firmware_vX.X.X.bin          = M5 Launcher, preserves XP
        - porkchop_vX.X.X_m5burner.bin = M5 Burner, recovers from SD


--[ 6 - Controls

    the M5Cardputer's keyboard is tiny but functional:

        +-------+----------------------------------+
        | Key   | What it does                     |
        +-------+----------------------------------+
        | O     | OINK - start hunting             |
        | W     | SGT WARHOG - start wardriving    |
        | B     | PIGGY BLUES - BLE chaos mode     |
        |       | (in OINK: add BOAR BRO)          |
        | H     | HOG ON SPECTRUM - WiFi analyzer  |
        | F     | PORKCHOP COMMANDER - file xfer   |
        | S     | SWINE STATS - lifetime stats     |
        | T     | tweak settings                   |
        | D     | CHILL DONOHAM from IDLE          |
        |       | toggle OINK<->DNH seamlessly     |
        |       | (in CLIENT MONITOR: details)     |
        | P     | screenshot - save to SD card     |
        | `     | back one level / open menu       |
        | ;     | navigate up / scroll left        |
        | .     | navigate down / scroll right     |
        | Enter | select / toggle / confirm        |
        | Bksp  | stop current mode, return idle   |
        | G0    | bail out - return to IDLE        |
        +-------+----------------------------------+

    G0 is the physical button on the top side of the M5Cardputer.
    press it anytime to bail out and return to IDLE. useful when
    your piglet is going ham on someone's network.
    
    main menu structure (v0.1.7):
    
        === MODES ===
        OINK                    - hunt for handshakes
        CHILL DONOHAM           - passive recon (no attacks)
        SGT WARHOG              - wardrive with GPS
        PIGGY BLUES             - BLE notification spam
        HOG ON SPECTRUM         - WiFi spectrum analyzer
        
        === DATA & STATS ===
        SWINE STATS             - lifetime stats & buffs
        LOOT                    - view saved loot
        PORK TRACKS             - upload to WiGLE
        BOAR BROS               - manage friendly networks
        ACHIEVEMENTS            - proof of pwn
        
        === SERVICES ===
        FILE TRANSFER           - WiFi file server
        LOG VIEWER              - debug log tail
        SETTINGS                - tweak the pig
        ABOUT                   - credits and info
    
    names matter. order matters. pig's organized like that.

    backtick navigation (v0.1.6+):

        from OINK/WARHOG/PIGGYBLUES/SPECTRUM -> IDLE
        from Client Monitor                  -> Spectrum view
        from IDLE                            -> opens MENU
        from MENU/Settings                   -> parent menu

    intuitive. only took six versions.

    screenshots are saved to /screenshots/screenshotNNN.bmp on the SD
    card. takes about 1.4 seconds - piggy freezes briefly. worth it
    for the documentation flex.


----[ 6.1 - Screen Layout

    your piglet's face lives on a 240x135 pixel canvas. not much real
    estate, but enough to cause trouble.

    top bar format:

        [MODE M00D]                       XX% GWM HH:MM

    left = current mode + mood indicator (HYP3/GUD/0K/M3H/S4D).
    right = battery percent + status flags + GPS time (or --:--).
    G=GPS, W=WiFi, M=ML. dashes mean inactive.

    the piglet has moods. watch the face change as it hunts:

        NEUTRAL     HAPPY       EXCITED     HUNTING     SLEEPY      SAD
         ?  ?        ^  ^        !  !        |  |        v  v        .  .
        (o 00)      (^ 00)      (@ 00)      (= 00)      (- 00)      (T 00)
        (    )      (    )      (    )      (    )      (    )      (    )

    yes, we spent actual development time on pig facial expressions.
    no regrets.

    network names display in UPPERCASE (v0.1.6+) for visibility on the
    tiny 240x135 screen. file exports keep original case. settings menu
    stays lowercase - you need the mental workout when configuring.


--[ 7 - Configuration

    settings persist to SPIFFS. your piglet remembers.

    navigate with ; and . keys, Enter to toggle/edit. press ESC (backtick)
    or Backspace to save and exit. changes take effect immediately,
    including GPS pin changes (hot-reinit, no reboot required).

        +------------+-------------------------------+---------+
        | Setting    | Description                   | Default |
        +------------+-------------------------------+---------+
        | WiFi SSID  | network for file transfer     | -       |
        | WiFi Pass  | password for that network     | -       |
        | WPA-SEC Key| 32-char hex key for cracking  | -       |
        | WiGLE Name | WiGLE API name for uploads    | -       |
        | WiGLE Token| WiGLE API token for uploads   | -       |
        | Sound      | beeps when things happen      | ON      |
        | Brightness | display brightness            | 80%     |
        | Dim After  | screen dim timeout, 0=never   | 30s     |
        | Dim Level  | brightness when dimmed        | 20%     |
        | Theme      | color scheme for UI           | P1NK    |
        | CH Hop     | channel hop interval          | 500ms   |
        | Lock Time  | client discovery window       | 4000ms  |
        | Deauth     | enable deauth attacks         | ON      |
        | Rnd MAC    | randomize MAC on mode start   | ON      |
        | DONOHAM    | passive mode. the quiet one.  | OFF     |
        | GPS        | enable GPS module             | ON      |
        | GPS PwrSave| sleep GPS when not hunting    | ON      |
        | Scan Intv  | WARHOG scan frequency         | 5s      |
        | GPS RX Pin | GPIO for GPS data receive     | 1       |
        | GPS TX Pin | GPIO for GPS data transmit    | 2       |
        | GPS Baud   | GPS module baud rate          | 115200  |
        | Timezone   | UTC offset for timestamps     | 0       |
        | ML Mode    | Basic/Enhanced beacon capture | Basic   |
        | SD Log     | debug logging to SD card      | OFF     |
        | BLE Burst  | BLE advertisement interval    | 200ms   |
        | BLE Adv Tm | per-packet duration           | 100ms   |
        +------------+-------------------------------+---------+

    GPS pin defaults work for original Cardputer + Grove GPS. if you're
    running Cardputer-Adv with Cap LoRa868 module, change pins to:
    RX=15, TX=13. yes, swapped - ESP32 RX receives from GPS TX.
    GPS reinits automatically when pins change - no reboot.


----[ 7.1 - Color Themes

    your piglet isn't locked to pink. cycle through themes with ; and .
    on the Theme setting. 12 flavors, from tactical to absurd:

        +------------+--------------------------------------------+
        | Theme      | Vibe                                       |
        +------------+--------------------------------------------+
        | P1NK       | classic piglet pink on black (default)     |
        | CYB3R      | electric cyan. because it's 2077 somewhere |
        | M4TR1X     | green phosphor. see the code, Neo          |
        | AMB3R      | warm terminal amber. old school CRT feels  |
        | BL00D      | aggressive red. for when you mean business |
        | GH0ST      | white on black. low-viz stealth mode       |
        +------------+--------------------------------------------+
        | PAP3R      | black on white. the inverted heresy        |
        | BUBBLEGUM  | hot pink on white. aggressively visible    |
        | M1NT       | teal on white. refreshing. minty.          |
        | SUNBURN    | orange on white. eye damage included       |
        +------------+--------------------------------------------+
        | L1TTL3M1XY | OG Game Boy LCD. 1989 pea-soup nostalgia   |
        | B4NSH33    | P1 phosphor green. VT100 ghost vibes       |
        +------------+--------------------------------------------+

    dark themes (top 6) keep things tactical. inverted themes (middle 4)
    exist for outdoor visibility or psychological warfare on bystanders.
    retro themes (bottom 2) for the nostalgic freaks who miss CRTs.

    theme persists across reboots. the pig never forgets a color scheme.


----[ 7.2 - API Keys Setup

    cloud features need credentials. the pig doesn't store them in
    flash - they live in config after you import from SD.


    WiGLE (wigle.net) - for wardrive uploads:

        1. create account at wigle.net
        2. go to Account -> API Token section
        3. generate or copy your API name and token
        4. create file on SD card root: /wigle_key.txt
        5. format: apiname:apitoken (one line, colon separator)
        6. in PORKCHOP: Settings -> < Load WiGLE Key >
        7. file auto-deletes after import

        now PORK TRACKS menu can upload your wardrives.


    WPA-SEC (wpa-sec.stanev.org) - for distributed cracking:

        1. register at wpa-sec.stanev.org
        2. get your 32-character hex API key from profile
        3. create file on SD card root: /wpasec_key.txt
        4. contents: just the key, nothing else
        5. in PORKCHOP: Settings -> < Load WPA-SEC Key >
           or just reboot - auto-imports on boot
        6. file self-destructs after import

        now LOOT menu can upload handshakes and fetch results.


    why the file dance? no USB keyboard on the Cardputer for entering
    64 character strings. SD card is faster. file deletion is paranoia.
    the pig doesn't judge your OpSec. the pig just oinks and forgets.


--[ 8 - ML Training Pipeline

    want to train your own model? here's the workflow:

----[ 8.1 - Data Collection

    WARHOG mode hoovers up ML training data automatically. drive around,
    feed the brain. zero extra effort required.

    how it works:
        - every network gets 32 features extracted from beacon frames
        - data accumulates in memory as you drive around
        - every 60 seconds, WARHOG dumps to /mldata/ - crash protection
        - when you stop WARHOG (G0 button), final export happens
        - worst case you lose 1 minute of data if piggy crashes

    the dump contains:
        BSSID, SSID, channel, RSSI, authmode, HT caps, vendor IEs,
        beacon interval, jitter, GPS coords, timestamp, and label

    set ML Mode to Enhanced in settings for deep beacon parsing.
    Basic mode uses ESP32 scan API. Enhanced mode sniffs raw 802.11.
    more features, more CPU, more fun.

----[ 8.2 - Labeling

    raw data starts unlabeled. use the prep script to auto-label based
    on security characteristics:

        $ python scripts/prepare_ml_data.py ml_training.csv

    the script outputs ml_training_ei.csv with string labels:

        normal        = boring ISP gear doing boring ISP things
        rogue_ap      = suspiciously loud. probably evil. trust issues.
        evil_twin     = identity theft but make it wireless
        deauth_target = no WPA3/PMF - begging for disconnection
        vulnerable    = Open/WEP/WPS - what decade is this

    the auto-labeler catches the obvious stuff. for real rogue/evil twin
    samples, you gotta set up sketchy APs in the lab and label manually.
    upload your labeled CSV to Edge Impulse for training.

----[ 8.3 - Training on Edge Impulse

    Edge Impulse does the grunt work. you just click buttons:

        1. create project at studio.edgeimpulse.com
        2. upload your labeled ml_training.csv
        3. design impulse: Raw data block -> Classification (Keras)
        4. train for 50+ epochs, check confusion matrix
        5. test on held-out data, iterate if needed

    aim for 90%+ accuracy before deploying. your piggy deserves
    a brain that actually works.

----[ 8.4 - Deployment

    when your model is ready:

        1. export as "C++ Library" targeting ESP32
        2. extract edge-impulse-sdk/ into porkchop/lib/
        3. open src/ml/edge_impulse.h
        4. uncomment: #define EDGE_IMPULSE_ENABLED
        5. rebuild and flash

    now your piglet runs real inference instead of heuristics.
    the grass still moves the same way, but the brain got an upgrade.


--[ 9 - Code Structure

    porkchop/
    |
    +-- src/
    |   +-- main.cpp              # entry point, main loop
    |   +-- build_info.h          # version string, build timestamp
    |   +-- core/
    |   |   +-- porkchop.cpp/h    # state machine, mode management
    |   |   +-- config.cpp/h      # configuration (SPIFFS persistence)
    |   |   +-- sdlog.cpp/h       # SD card debug logging
    |   |   +-- wsl_bypasser.cpp/h # frame injection, MAC randomization
    |   |   +-- xp.cpp/h          # RPG XP/leveling, achievements, NVS
    |   |
    |   +-- ui/
    |   |   +-- display.cpp/h     # triple-canvas display system
    |   |   +-- menu.cpp/h        # main menu with callbacks
    |   |   +-- settings_menu.cpp/h   # interactive settings
    |   |   +-- captures_menu.cpp/h   # LOOT menu - browse captured handshakes
    |   |   +-- wigle_menu.cpp/h  # PORK TRACKS - WiGLE file uploads
    |   |   +-- boar_bros_menu.cpp/h  # BOAR BROS - manage excluded networks
    |   |   +-- achievements_menu.cpp/h # proof of pwn viewer
    |   |   +-- log_viewer.cpp/h  # view SD card logs
    |   |   +-- swine_stats.cpp/h # lifetime stats, buff/debuff system
    |   |
    |   +-- piglet/
    |   |   +-- avatar.cpp/h      # derpy ASCII pig (flips L/R)
    |   |   +-- mood.cpp/h        # context-aware phrase system
    |   |
    |   +-- gps/
    |   |   +-- gps.cpp/h         # TinyGPS++ wrapper, power mgmt
    |   |
    |   +-- ml/
    |   |   +-- features.cpp/h    # 32-feature WiFi extraction
    |   |   +-- inference.cpp/h   # heuristic + Edge Impulse classifier
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
    |   +-- prepare_ml_data.py    # label & convert data for Edge Impulse
    |   +-- pre_build.py          # build info generator
    |
    +-- docs/
    |   +-- EDGE_IMPULSE_TRAINING.txt  # step-by-step ML training guide
    |
    +-- .github/
    |   +-- copilot-instructions.md   # AI assistant context
    |
    +-- platformio.ini            # build config


--[ 10 - Legal sh*t

    READ THIS PART, SKID.

    PORKCHOP is for AUTHORIZED SECURITY RESEARCH and EDUCATIONAL USE.
    period. full stop. end of discussion.

        * your networks only. or written permission. IN WRITING.
        * deauth attacks are ILLEGAL in most places. surprise!
        * wardriving legality varies. google your jurisdiction.
        * BLE spam is a dick move even if technically legal
        * authors assume ZERO LIABILITY. not our problem.
        * if you get caught, you never heard of us

    the law doesn't care that it's "just research" when you're sitting
    in a police station explaining why you were parked outside a bank
    with a directional antenna.

    your neighbor's WiFi password is not your WiFi password. your
    neighbor's streaming services are not your streaming services.
    "but I only wanted to test..." is not a legal defense.

    we made a cute ASCII pig that hunts WiFi. we did not make a magic
    immunity talisman against federal telecommunications laws. the pig
    can't testify on your behalf. the pig will not visit you in prison.

    don't be stupid. don't be evil. don't make us regret publishing this.


--[ 11 - Greetz

    respect to those who came before:

        * evilsocket & pwnagotchi - you started this madness
        * M5Stack - cheap hardware, expensive possibilities
        * Edge Impulse - ML for the rest of us
        * the ESP32 underground - keeping embedded hacking alive
        * Phrack Magazine - the OG zine we're poorly imitating
        * Binrev, 2600, and the scene that won't die
        * you - for scrolling past the legal section

    special shoutout to the Cardputer-ADV + LoRa testing crew:

        * littlemixy - sacrificed hardware to my broken RX/TX pin configs
        * BansheeBacklash - emotional support through the GPIO nightmare

    these absolute units stuck around while we debugged pin 13 vs 15
    shenanigans on hardware they bought with real money. their themes
    are immortalized in the firmware. heroes don't always wear capes,
    sometimes they just have too many ESP32s.

    "the WiFi is free if you're brave enough."
    
    stay paranoid. stay curious. stay out of trouble (mostly).


--[ 12 - Credits

    developed by: 0ct0
    team size: 1
    pronoun of choice: "we"

    there is no team. there never was a team.
    "we" is aspirational. "we" is the dream.
    "we" is what you say when you want to sound
    like a legitimate operation and not just
    some guy debugging frame injection at 3am
    while the pig judges silently from the display.

    contributors welcome. the pig needs friends.
    the pig's creator needs therapy.
    both are accepting pull requests.


--[ 13 - Support The Pig

    this project runs on:
        * mass quantities of caffeine
        * sleep deprivation
        * the faint hope someone finds this useful

    if PORKCHOP saved you from buying a Flipper,
    cracked a handshake that made you smile,
    or just entertained you for five minutes -
    consider funding the next 3am debug session:

        https://buymeacoffee.com/0ct0

    your coffee becomes my code.
    my code becomes everyone's pig.
    circle of life. hakuna matata. oink.

    (not required. never expected. always appreciated.
     the pig oinks louder for supporters.)


OINK! OINK!

==[EOF]==

