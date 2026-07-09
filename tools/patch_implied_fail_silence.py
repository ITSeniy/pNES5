#!/usr/bin/env python3
"""
Patch AccuracyCoin.nes so Implied Dummy Reads FAIL path silences the APU.

Without this, a FAIL while DMC is looping ($4010=$4F) leaves the channel
running into the menu / next attempt — audible noise and bus thrash that can
look like palette corruption.

FAIL was:  JSR RestoreRAM / JMP TEST_Fail
Now:       JMP stub  (stub: STA $4015=#0, JSR Restore, JMP Fail)
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROM = ROOT / "roms" / "AccuracyCoin.nes"

# PRG offsets (CPU $8000 + off)
FAIL_OFF = 0xD340 - 0x8000  # 20 66 D6 4C 38 A0
STUB_OFF = 0x99E7 - 0x8000  # free $FF run

# RestoreRAM @ $D666, TEST_Fail @ $A038
STUB = bytes(
    [
        0xA9,
        0x00,  # LDA #0
        0x8D,
        0x15,
        0x40,  # STA $4015
        0x8D,
        0x10,
        0x40,  # STA $4010 (clear loop/irq/rate)
        0x20,
        0x66,
        0xD6,  # JSR RestoreRAM
        0x4C,
        0x38,
        0xA0,  # JMP TEST_Fail
    ]
)


def main() -> int:
    data = bytearray(ROM.read_bytes())
    if data[:4] != b"NES\x1a":
        raise SystemExit("bad iNES")
    prg = memoryview(data)[16:]

    fail = bytes(prg[FAIL_OFF : FAIL_OFF + 6])
    if fail not in (bytes([0x20, 0x66, 0xD6, 0x4C, 0x38, 0xA0]), bytes([0x4C, 0xE7, 0x99, 0xEA, 0xEA, 0xEA])):
        raise SystemExit(f"unexpected FAIL bytes: {fail.hex()}")

    # Ensure stub region is free or already our stub
    region = bytes(prg[STUB_OFF : STUB_OFF + len(STUB)])
    if region != b"\xff" * len(STUB) and region != STUB:
        raise SystemExit(f"stub region not free: {region.hex()}")

    data[16 + STUB_OFF : 16 + STUB_OFF + len(STUB)] = STUB
    # JMP $99E7; NOP NOP NOP
    data[16 + FAIL_OFF : 16 + FAIL_OFF + 6] = bytes([0x4C, 0xE7, 0x99, 0xEA, 0xEA, 0xEA])

    ROM.write_bytes(data)
    print(f"patched {ROM}: FAIL → silence stub @ $99E7 ({len(STUB)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
