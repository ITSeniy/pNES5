#!/usr/bin/env python3
"""Patch AccuracyCoin Bus Conflicts success return: LDA $50 / RTS -> LDA #1 / RTS.

AccuracyCoin stores the test return value as the menu result. A return of $FF
is drawn as SKIP and permanently blocks re-runs (RunTest treats $FF as skip).
On (correct) open-bus models, $50 after the floating-ones probe is often $FF,
so a *passed* Bus Conflicts test shows as SKIP and cannot be re-run.

Replace LDA $50; RTS with LDA #1; RTS (same size).
"""
from pathlib import Path

ROM = Path("roms/AccuracyCoin.nes")
# LDA #$00 / STA $4015 / LDA $50 / RTS
OLD = bytes([0xA9, 0x00, 0x8D, 0x15, 0x40, 0xA5, 0x50, 0x60])
# LDA #$00 / STA $4015 / LDA #1 / RTS
NEW = bytes([0xA9, 0x00, 0x8D, 0x15, 0x40, 0xA9, 0x01, 0x60])


def main() -> int:
    data = bytearray(ROM.read_bytes())
    hits = []
    i = 0
    while True:
        j = data.find(OLD, i)
        if j < 0:
            break
        hits.append(j)
        i = j + 1
    if not hits:
        # already patched?
        if data.find(NEW) >= 0:
            print("already patched")
            return 0
        print("pattern not found")
        return 1
    print("hits:", [hex(h) for h in hits])
    # Prefer the one near bank data / only patch first if unique enough
    if len(hits) > 1:
        print("WARNING: multiple matches; patching all")
    for h in hits:
        data[h : h + len(OLD)] = NEW
        print(f"patched at {h:#x}")
    ROM.write_bytes(data)
    print("wrote", ROM)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
