#!/usr/bin/env python3
"""Audit every bank in every library: count WAVs, flag any with < 35 samples
or with gaps in sequential -N numbering. Can also compare against Move."""
import os, sys, subprocess, re

LIBS = ["MT", "SB", "90", "LL"]
M400_KEYS = 35       # G2..F5 standard
EXTENDED  = 37       # F2..F5 (MellowTrawn convention)

SEQ_RE = re.compile(r'-(\d+)\.wav$', re.IGNORECASE)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_parser import parse_midi   # uses the SAME word-boundary logic as v0.1.6 wav_bank.c

NOTE_NAME_BOUNDARY_RE = re.compile(r'(?:^|[^A-Za-z])[A-G][#b]?-?\d', re.IGNORECASE)

def scan_bank_local(path):
    """Return (wav_count, missing_seq_indices, max_seq, classifier).
    classifier is 'note-name' (parser found note name) or 'sequential' or 'mixed'."""
    if not os.path.isdir(path): return None
    files = [f for f in os.listdir(path) if f.lower().endswith('.wav')]
    if not files: return None
    seq_indices = []
    note_name_count = 0
    for f in files:
        # Use the actual parser logic to classify
        midi_note = parse_midi(f, -1)   # seq_base=-1 → only note-name path runs
        if midi_note >= 0:
            note_name_count += 1
        else:
            m = SEQ_RE.search(f)
            if m:
                seq_indices.append(int(m.group(1)))
    classifier = ('note-name' if note_name_count == len(files) else
                  'sequential' if note_name_count == 0 else 'mixed')
    seq_indices.sort()
    missing = []
    if classifier == 'sequential' and seq_indices:
        max_n = max(seq_indices)
        present = set(seq_indices)
        missing = [i for i in range(1, max_n + 1) if i not in present]
    return (len(files), missing, max(seq_indices) if seq_indices else 0, classifier)

def scan_move():
    """Return {lib: {bank: count}} for every bank on the Move."""
    out = subprocess.run(
        ["ssh", "ableton@move.local",
         "find /data/UserData/schwung/modules/sound_generators/mello/instruments -name '*.wav' | sort"],
        capture_output=True, text=True, timeout=60)
    result = {}
    for line in out.stdout.splitlines():
        # path: /data/.../instruments/<lib>/<bank>/<file>.wav
        parts = line.rsplit('/', 3)
        if len(parts) < 4: continue
        lib = parts[-3]
        bank = parts[-2]
        result.setdefault(lib, {}).setdefault(bank, 0)
        result[lib][bank] += 1
    return result

def main():
    print("=" * 78)
    print("MELLO BANK AUDIT — local first, then Move-side comparison")
    print("=" * 78)

    move_state = None
    if "--no-move" not in sys.argv:
        print("\nQuerying Move (move.local) ...")
        try:
            move_state = scan_move()
            print(f"  got data for {sum(len(v) for v in move_state.values())} banks across {len(move_state)} libraries")
        except Exception as e:
            print(f"  ERROR: {e} (continuing with local-only audit)")

    summary = {"total": 0, "under35": 0, "with_gaps": 0, "move_short": 0, "move_missing": 0}

    for lib in LIBS:
        lib_dir = f"instruments/{lib}"
        if not os.path.isdir(lib_dir):
            continue
        banks = sorted(os.listdir(lib_dir))
        short = []
        gaps = []
        move_short_list = []
        move_missing_list = []
        for bank in banks:
            r = scan_bank_local(f"{lib_dir}/{bank}")
            if r is None: continue
            count, missing, max_n, classifier = r
            summary["total"] += 1

            # Tolerance: banks with note-name filenames vary (some Mellotron tapes are partial
            # like SFX banks — accept anything > 5 as "intentional"). Only flag sequential
            # banks with obvious gaps or low counts as suspicious.
            if count < 35:
                short.append((bank, count, classifier))
                summary["under35"] += 1
            if missing:
                gaps.append((bank, missing, max_n))
                summary["with_gaps"] += 1
            if move_state is not None:
                move_count = move_state.get(lib, {}).get(bank, None)
                if move_count is None:
                    move_missing_list.append(bank)
                    summary["move_missing"] += 1
                elif move_count < count:
                    move_short_list.append((bank, count, move_count))
                    summary["move_short"] += 1

        print(f"\n{'-'*78}")
        print(f"{lib}/  ({len(banks)} banks)")
        print(f"{'-'*78}")
        if short:
            print(f"  {len(short)} banks under 35 samples (some intentional for SFX/partial banks):")
            for bank, n, classifier in short[:25]:
                print(f"    {bank:<40} {n:>3} WAVs  [{classifier}]")
            if len(short) > 25: print(f"    ... + {len(short)-25} more")
        else:
            print("  all banks have >=35 samples")
        if gaps:
            print(f"\n  {len(gaps)} banks with gaps in sequential numbering:")
            for bank, miss, maxn in gaps[:25]:
                print(f"    {bank:<40} max=-{maxn} missing={miss}")
            if len(gaps) > 25: print(f"    ... + {len(gaps)-25} more")
        else:
            print("  all sequential banks numbered without gaps")
        if move_state is not None:
            if move_missing_list:
                print(f"\n  {len(move_missing_list)} banks MISSING on Move (need install_banks.sh):")
                for b in move_missing_list[:10]:
                    print(f"    {b}")
                if len(move_missing_list) > 10: print(f"    ... + {len(move_missing_list)-10} more")
            if move_short_list:
                print(f"\n  {len(move_short_list)} banks SHORT on Move (partial transfer):")
                for b, l, m in move_short_list[:10]:
                    print(f"    {b:<40} local={l:>3}  move={m:>3}  short by {l-m}")
                if len(move_short_list) > 10: print(f"    ... + {len(move_short_list)-10} more")

    print("\n" + "=" * 78)
    print("SUMMARY")
    print(f"  total banks scanned        : {summary['total']}")
    print(f"  banks with < 35 samples    : {summary['under35']}")
    print(f"  banks with seq numbering gaps: {summary['with_gaps']}")
    if move_state is not None:
        print(f"  banks missing entirely on Move: {summary['move_missing']}")
        print(f"  banks short on Move (partial) : {summary['move_short']}")
    print("=" * 78)

if __name__ == "__main__":
    main()
