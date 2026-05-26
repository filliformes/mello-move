#!/usr/bin/env python3
"""Python reimplementation of mello_parse_midi_note for parser verification."""
import os, sys

BASE = {'C':0,'D':2,'E':4,'F':5,'G':7,'A':9,'B':11}

def try_note_at(s, i):
    ch = s[i].upper()
    if ch not in BASE: return -1
    base = BASE[ch]
    j = i + 1
    acc = 0
    if j < len(s):
        c = s[j]
        if c in '#sS': acc = 1; j += 1
        elif c == 'b': acc = -1; j += 1
    sign = 1
    if j < len(s) and s[j] == '-':
        sign = -1; j += 1
    if j >= len(s) or not s[j].isdigit(): return -1
    oct = (ord(s[j]) - ord('0')) * sign
    midi = (oct + 1) * 12 + base + acc
    if midi < 0 or midi > 127: return -1
    return midi

def trailing_seq(name):
    if not name: return -1
    i = len(name) - 1
    if not name[i].isdigit(): return -1
    while i > 0 and name[i-1].isdigit():
        i -= 1
    if i == 0 or name[i-1] != '-': return -1
    return int(name[i:])

def parse_midi(filename, seq_base=40):
    name = os.path.basename(filename)
    if '.' in name: name = name.rsplit('.', 1)[0]
    found = -1
    for i in range(len(name)):
        if i > 0 and name[i-1].isalpha(): continue   # word-boundary guard (v0.1.6)
        m = try_note_at(name, i)
        if m >= 0: found = m
    if found >= 0: return found
    if seq_base >= 0:
        s = trailing_seq(name)
        if s >= 1:
            m = s + seq_base
            if 0 <= m <= 127: return m
    return -1

def midi_to_name(m):
    if m < 0: return "?"
    return ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"][m % 12] + str(m // 12 - 1)

if __name__ == "__main__":
    print("=== Self-test (matches C harness) ===")
    cases = [
        ("M400Cello-1.wav",  40, 41, "F2"),
        ("M400Cello-8.wav",  40, 48, "C3"),
        ("M400Cello-20.wav", 40, 60, "C4"),
        ("M400Cello-32.wav", 40, 72, "C5"),
        ("MkiiFlute-1.wav",  40, 41, "F2"),
        ("MkiiFlute-20.wav", 40, 60, "C4"),
    ]
    for fn, sb, exp, ename in cases:
        got = parse_midi(fn, sb)
        flag = "OK" if got == exp else "BUG"
        print(f"  {fn:<25} sb={sb} -> {got} ({midi_to_name(got)})  expect {exp} ({ename}) [{flag}]")

    if len(sys.argv) > 1:
        bank_dir = sys.argv[1]
        print(f"\n=== Scan {bank_dir} ===")
        files = sorted(os.listdir(bank_dir))
        slot_assignments = {}
        for fn in files:
            if not fn.lower().endswith('.wav'): continue
            m = parse_midi(fn, 40)
            if m in slot_assignments:
                print(f"  COLLISION: slot {m} ({midi_to_name(m)}) - {slot_assignments[m]} AND {fn}")
            slot_assignments[m] = fn
            print(f"  {fn:<30} -> MIDI {m:>3} ({midi_to_name(m)})")
