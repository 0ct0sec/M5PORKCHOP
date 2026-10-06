"""
OINK State Machine Timing Simulation
=====================================
Models the timing difference between sequential (current) and parallel C5
coprocessor architectures for 5GHz handshake capture.

Scenario: 15 networks (10 x 2.4GHz, 5 x 5GHz)
Runs 1000 Monte Carlo iterations with randomized timings.
"""

import random
import statistics
from dataclasses import dataclass, field

NUM_ITERATIONS = 1000
NUM_24GHZ = 10
NUM_5GHZ = 5
TARGET_MAX_ATTEMPTS = 4

random.seed(42)


@dataclass
class SimResult:
    total_time: float = 0.0
    captures_24: int = 0
    captures_5: int = 0
    time_blocked: float = 0.0  # time not actively attacking 2.4GHz


def rand_locking_time(has_client: bool) -> float:
    """LOCKING phase for 2.4GHz target."""
    if has_client:
        # Fast track: ~2.5s avg, modeled as normal clipped [1.0, 4.0]
        return max(1.0, min(4.0, random.gauss(2.5, 0.6)))
    else:
        # No client: timeout at 7s, modeled as uniform [5.0, 7.0]
        return random.uniform(5.0, 7.0)


def rand_attacking_time_24(captured: bool) -> float:
    """ATTACKING phase for 2.4GHz target."""
    if captured:
        # Successful capture: avg 8s, spread [2, 14]
        return max(2.0, min(14.0, random.gauss(8.0, 3.0)))
    else:
        # Timeout at 15s
        return 15.0


def rand_attacking_time_5_sequential(success: bool) -> float:
    """ATTACKING phase for 5GHz target in sequential model."""
    if success:
        # avg 25s for success, spread [10, 55]
        return max(10.0, min(55.0, random.gauss(25.0, 10.0)))
    else:
        # Timeout at 60s
        return 60.0


def rand_attacking_time_5_parallel(success: bool) -> float:
    """C5 attack time in parallel model (reduced timeout)."""
    if success:
        # avg 20s for success, spread [8, 28]
        return max(8.0, min(28.0, random.gauss(20.0, 5.0)))
    else:
        # Timeout at 30s
        return 30.0


def simulate_model_a() -> SimResult:
    """
    Model A: Current Sequential Architecture
    -----------------------------------------
    Single state machine processes all targets one-by-one.
    5GHz targets block the state machine (LOCKING=0, ATTACKING=up to 60s).
    """
    res = SimResult()
    elapsed = 0.0
    time_attacking_24 = 0.0

    # --- SCANNING phase ---
    scan_time = random.gauss(5.0, 0.3)
    elapsed += max(4.0, min(6.0, scan_time))

    # --- PMKID_HUNTING phase ---
    # 15 APs, 300ms each, assume all are WPA2 for simplicity
    # 60% respond with PMKID
    num_wpa2 = 15
    pmkid_time = min(num_wpa2 * 0.3, 30.0)
    elapsed += pmkid_time
    pmkid_captures_24 = sum(1 for _ in range(NUM_24GHZ) if random.random() < 0.60)
    pmkid_captures_5 = sum(1 for _ in range(NUM_5GHZ) if random.random() < 0.60)

    time_not_attacking_24 = elapsed  # scanning + pmkid is not attacking 2.4

    # Build target list: interleave 2.4 and 5GHz as the state machine would
    # In practice, targets are sorted by signal strength; we model random order
    targets = []
    for i in range(NUM_24GHZ):
        has_client = random.random() < 0.60
        targets.append(('2.4', has_client, i))
    for i in range(NUM_5GHZ):
        targets.append(('5', False, i))

    random.shuffle(targets)

    # Track per-network capture state
    captured_24 = set()  # indices of captured 2.4GHz networks
    captured_5 = set()   # indices of captured 5GHz networks

    # Add PMKID captures (random subset)
    pmkid_24_indices = random.sample(range(NUM_24GHZ), pmkid_captures_24)
    pmkid_5_indices = random.sample(range(NUM_5GHZ), pmkid_captures_5)
    captured_24.update(pmkid_24_indices)
    captured_5.update(pmkid_5_indices)

    # Process each target up to TARGET_MAX_ATTEMPTS
    attempt_counts = {}
    for band, has_client, idx in targets:
        key = (band, idx)
        if key not in attempt_counts:
            attempt_counts[key] = 0

    # We simulate going through the target list multiple times (up to max attempts)
    for attempt_round in range(TARGET_MAX_ATTEMPTS):
        for band, has_client, idx in targets:
            key = (band, idx)

            # Skip if already captured
            if band == '2.4' and idx in captured_24:
                continue
            if band == '5' and idx in captured_5:
                continue

            attempt_counts[key] = attempt_counts.get(key, 0) + 1
            if attempt_counts[key] > TARGET_MAX_ATTEMPTS:
                continue

            # Overhead between targets
            overhead = random.uniform(0.08, 0.12)
            elapsed += overhead
            time_not_attacking_24 += overhead

            if band == '5':
                # 5GHz: LOCKING=0, ATTACKING=up to 60s
                success = random.random() < 0.40
                attack_time = rand_attacking_time_5_sequential(success)
                elapsed += attack_time
                time_not_attacking_24 += attack_time  # blocks 2.4 work

                # WAITING
                wait = random.gauss(4.5, 0.3)
                elapsed += max(3.5, min(5.5, wait))
                time_not_attacking_24 += max(3.5, min(5.5, wait))

                if success:
                    captured_5.add(idx)

            else:
                # 2.4GHz: LOCKING -> ATTACKING -> WAITING
                lock_time = rand_locking_time(has_client)
                elapsed += lock_time
                time_not_attacking_24 += lock_time  # locking is not attacking

                # Determine capture success: 50% before timeout
                success = random.random() < 0.50
                attack_time = rand_attacking_time_24(success)
                elapsed += attack_time
                time_attacking_24 += attack_time

                # WAITING: 4.5s normally, 9s if M1 captured (partial)
                m1_captured = random.random() < 0.30 if not success else False
                if success:
                    wait_base = 4.5
                elif m1_captured:
                    wait_base = 9.0
                else:
                    wait_base = 4.5
                wait = random.gauss(wait_base, 0.3)
                wait = max(wait_base - 1.0, min(wait_base + 1.0, wait))
                elapsed += wait
                time_not_attacking_24 += wait

                if success:
                    captured_24.add(idx)

    res.total_time = elapsed
    res.captures_24 = len(captured_24)
    res.captures_5 = len(captured_5)
    res.time_blocked = time_not_attacking_24
    return res


def simulate_model_b() -> SimResult:
    """
    Model B: Proposed Parallel C5 Architecture
    -------------------------------------------
    C5 coprocessor handles 5GHz in background.
    State machine only processes 2.4GHz targets.
    5GHz targets dispatched during SCANNING/PMKID phase.
    """
    res = SimResult()
    elapsed = 0.0
    time_attacking_24 = 0.0

    # --- SCANNING phase ---
    scan_time = random.gauss(5.0, 0.3)
    scan_time = max(4.0, min(6.0, scan_time))
    elapsed += scan_time

    # --- PMKID_HUNTING phase ---
    num_wpa2 = 15
    pmkid_time = min(num_wpa2 * 0.3, 30.0)
    elapsed += pmkid_time
    pmkid_captures_24 = sum(1 for _ in range(NUM_24GHZ) if random.random() < 0.60)
    pmkid_captures_5 = sum(1 for _ in range(NUM_5GHZ) if random.random() < 0.60)

    time_not_attacking_24 = elapsed  # scanning + pmkid

    # --- Dispatch 5GHz to C5 (fire-and-forget during scan/pmkid) ---
    # C5 processes all 5GHz targets in parallel (in background)
    # Each 5GHz target: 40% success, timeout 30s
    c5_total_time = 0.0
    captured_5 = set()
    pmkid_5_indices = random.sample(range(NUM_5GHZ), pmkid_captures_5)
    captured_5.update(pmkid_5_indices)

    for i in range(NUM_5GHZ):
        if i in captured_5:
            # Already got PMKID, C5 skips or spends minimal time
            c5_total_time = max(c5_total_time, random.uniform(0.5, 1.0))
            continue
        # C5 processes sequentially on 5GHz radio but independently of main state machine
        best_attempt_time = float('inf')
        c5_target_time = 0.0
        target_captured = False
        for attempt in range(TARGET_MAX_ATTEMPTS):
            success = random.random() < 0.40
            t = rand_attacking_time_5_parallel(success)
            c5_target_time += t + random.uniform(0.5, 1.5)  # inter-attempt gap
            if success:
                target_captured = True
                break
        c5_total_time += c5_target_time
        if target_captured:
            captured_5.add(i)

    # --- 2.4GHz targets (main state machine, unblocked) ---
    targets_24 = []
    for i in range(NUM_24GHZ):
        has_client = random.random() < 0.60
        targets_24.append((has_client, i))

    random.shuffle(targets_24)

    captured_24 = set()
    pmkid_24_indices = random.sample(range(NUM_24GHZ), pmkid_captures_24)
    captured_24.update(pmkid_24_indices)

    attempt_counts = {}

    for attempt_round in range(TARGET_MAX_ATTEMPTS):
        for has_client, idx in targets_24:
            if idx in captured_24:
                continue

            attempt_counts[idx] = attempt_counts.get(idx, 0) + 1
            if attempt_counts[idx] > TARGET_MAX_ATTEMPTS:
                continue

            # Overhead
            overhead = random.uniform(0.08, 0.12)
            elapsed += overhead
            time_not_attacking_24 += overhead

            # LOCKING
            lock_time = rand_locking_time(has_client)
            elapsed += lock_time
            time_not_attacking_24 += lock_time

            # ATTACKING
            success = random.random() < 0.50
            attack_time = rand_attacking_time_24(success)
            elapsed += attack_time
            time_attacking_24 += attack_time

            # WAITING
            m1_captured = random.random() < 0.30 if not success else False
            if success:
                wait_base = 4.5
            elif m1_captured:
                wait_base = 9.0
            else:
                wait_base = 4.5
            wait = random.gauss(wait_base, 0.3)
            wait = max(wait_base - 1.0, min(wait_base + 1.0, wait))
            elapsed += wait
            time_not_attacking_24 += wait

            if success:
                captured_24.add(idx)

    # Total time is max(main_state_machine, c5_time) since they run in parallel
    # But C5 starts during scan+pmkid, so it has a head start
    c5_start_offset = scan_time + pmkid_time  # C5 dispatched during these phases
    # Actually C5 is dispatched at start of scan, so it runs for the full duration
    # The main SM elapsed already includes scan+pmkid+all 2.4GHz work
    # C5 runs concurrently, so total = max(main_elapsed, c5_total_time)
    # (C5 starts at roughly the same time as scanning begins)

    main_elapsed = elapsed
    # C5 starts at beginning of scan phase, so its wall-clock end =  c5_total_time
    # Main SM wall-clock end = main_elapsed
    total_elapsed = max(main_elapsed, c5_total_time)

    res.total_time = total_elapsed
    res.captures_24 = len(captured_24)
    res.captures_5 = len(captured_5)
    # Time blocked = time in main SM not actively attacking 2.4GHz
    res.time_blocked = time_not_attacking_24
    return res


def run_simulations():
    results_a = []
    results_b = []

    for _ in range(NUM_ITERATIONS):
        results_a.append(simulate_model_a())
        results_b.append(simulate_model_b())

    def summarize(results, label):
        total_times = [r.total_time for r in results]
        caps_24 = [r.captures_24 for r in results]
        caps_5 = [r.captures_5 for r in results]
        caps_total = [r.captures_24 + r.captures_5 for r in results]
        rates = [(r.captures_24 + r.captures_5) / (r.total_time / 60.0) for r in results]
        blocked = [r.time_blocked for r in results]
        blocked_pct = [r.time_blocked / r.total_time * 100 for r in results]

        return {
            'label': label,
            'total_time_avg': statistics.mean(total_times),
            'total_time_std': statistics.stdev(total_times),
            'total_time_min': min(total_times),
            'total_time_max': max(total_times),
            'caps_24_avg': statistics.mean(caps_24),
            'caps_5_avg': statistics.mean(caps_5),
            'caps_total_avg': statistics.mean(caps_total),
            'rate_avg': statistics.mean(rates),
            'rate_std': statistics.stdev(rates),
            'blocked_avg': statistics.mean(blocked),
            'blocked_pct_avg': statistics.mean(blocked_pct),
        }

    sa = summarize(results_a, 'Model A: Sequential')
    sb = summarize(results_b, 'Model B: Parallel C5')

    # Print comparison table
    print("=" * 80)
    print("  OINK STATE MACHINE TIMING SIMULATION")
    print("  Scenario: 15 networks (10 x 2.4GHz, 5 x 5GHz)")
    print(f"  Iterations: {NUM_ITERATIONS} | Max attempts/target: {TARGET_MAX_ATTEMPTS}")
    print("=" * 80)
    print()

    header = f"{'Metric':<40} {'Sequential':>16} {'Parallel C5':>16}"
    print(header)
    print("-" * 72)

    rows = [
        ("Total cycle time (avg)", f"{sa['total_time_avg']:.1f}s", f"{sb['total_time_avg']:.1f}s"),
        ("Total cycle time (std dev)", f"{sa['total_time_std']:.1f}s", f"{sb['total_time_std']:.1f}s"),
        ("Total cycle time (min)", f"{sa['total_time_min']:.1f}s", f"{sb['total_time_min']:.1f}s"),
        ("Total cycle time (max)", f"{sa['total_time_max']:.1f}s", f"{sb['total_time_max']:.1f}s"),
        ("", "", ""),
        ("2.4GHz handshakes captured (avg)", f"{sa['caps_24_avg']:.2f}", f"{sb['caps_24_avg']:.2f}"),
        ("5GHz handshakes captured (avg)", f"{sa['caps_5_avg']:.2f}", f"{sb['caps_5_avg']:.2f}"),
        ("Total handshakes captured (avg)", f"{sa['caps_total_avg']:.2f}", f"{sb['caps_total_avg']:.2f}"),
        ("", "", ""),
        ("Captures per minute (avg)", f"{sa['rate_avg']:.2f}", f"{sb['rate_avg']:.2f}"),
        ("Captures per minute (std dev)", f"{sa['rate_std']:.2f}", f"{sb['rate_std']:.2f}"),
        ("", "", ""),
        ("Time blocked/idle (avg)", f"{sa['blocked_avg']:.1f}s", f"{sb['blocked_avg']:.1f}s"),
        ("Blocked % of total (avg)", f"{sa['blocked_pct_avg']:.1f}%", f"{sb['blocked_pct_avg']:.1f}%"),
    ]

    for label, va, vb in rows:
        if label == "":
            print()
        else:
            print(f"  {label:<38} {va:>16} {vb:>16}")

    print()
    print("-" * 72)

    # Deltas
    time_saved = sa['total_time_avg'] - sb['total_time_avg']
    time_saved_pct = time_saved / sa['total_time_avg'] * 100
    rate_improvement = sb['rate_avg'] - sa['rate_avg']
    rate_improvement_pct = rate_improvement / sa['rate_avg'] * 100
    extra_5_captures = sb['caps_5_avg'] - sa['caps_5_avg']

    print()
    print("  IMPROVEMENT SUMMARY (Parallel C5 vs Sequential)")
    print("  " + "-" * 50)
    print(f"  Cycle time reduction:     {time_saved:+.1f}s ({time_saved_pct:+.1f}%)")
    print(f"  Capture rate improvement: {rate_improvement:+.2f} cap/min ({rate_improvement_pct:+.1f}%)")
    print(f"  Extra 5GHz captures/cycle:{extra_5_captures:+.2f}")
    print(f"  Blocked time reduction:   {sa['blocked_avg'] - sb['blocked_avg']:+.1f}s")
    print()

    # Detailed breakdown
    print("=" * 80)
    print("  TIMING BREAKDOWN (averages)")
    print("=" * 80)
    print()

    # Model A breakdown
    a_scan = 5.0 + 4.5  # scan + pmkid
    print("  Model A: Sequential")
    print(f"    Scan + PMKID phase:          ~{a_scan:.1f}s")
    print(f"    Per 5GHz target (blocked):   ~{(sa['total_time_avg'] - sb['total_time_avg']) / NUM_5GHZ:.1f}s avg")
    print(f"    Total 5GHz blocking:         ~{sa['blocked_avg'] - sb['blocked_avg']:.1f}s")
    print()
    print("  Model B: Parallel C5")
    print(f"    Scan + PMKID phase:          ~{a_scan:.1f}s")
    print(f"    5GHz blocking:               0.0s (runs on C5)")
    print(f"    C5 background time:          overlapped with 2.4GHz work")
    print()
    print("=" * 80)


if __name__ == '__main__':
    run_simulations()
