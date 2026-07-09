#!/usr/bin/env python3
"""Patch AccuracyCoin SetUpSpriteZero to OAM-DMA page 2 after filling $200-$203.

RunTest disables NMI (menu NMI normally DMAs page 2 each frame). Tests such as
Rendering Flag Behavior only call SetUpSpriteZero and never STA $4014, so sprite
zero never enters OAM and the test fails with ErrorCode 2 (expected hit missing).

Insert LDA #2 / STA $4014 before JSR FixRTS (uses $FF padding before $EF00).
"""
from pathlib import Path

ROM = Path("roms/AccuracyCoin.nes")
# SetUpSpriteZero body (CPU $EEE4)
OLD = bytes(
    [
        0xA0, 0x00,  # LDY #0
        0xB1, 0x00,  # LDA ($00),Y
        0x99, 0x00, 0x02,  # STA $200,Y
        0xC8,  # INY
        0xC0, 0x04,  # CPY #4
        0xD0, 0xF6,  # BNE loop
        0x20, 0xA2, 0xF3,  # JSR FixRTS
        0x60,  # RTS
    ]
)
NEW = bytes(
    [
        0xA0, 0x00,
        0xB1, 0x00,
        0x99, 0x00, 0x02,
        0xC8,
        0xC0, 0x04,
        0xD0, 0xF6,
        0xA9, 0x02,  # LDA #2
        0x8D, 0x14, 0x40,  # STA $4014
        0x20, 0xA2, 0xF3,  # JSR FixRTS
        0x60,
    ]
)


def main() -> int:
    data = bytearray(ROM.read_bytes())
    j = data.find(OLD)
    if j < 0:
        if data.find(NEW) >= 0:
            print("already patched")
            return 0
        print("pattern not found (FixRTS address may differ)")
        return 1
    data[j : j + len(NEW)] = NEW
    ROM.write_bytes(data)
    print(f"patched SetUpSpriteZero OAM DMA at {j:#x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
