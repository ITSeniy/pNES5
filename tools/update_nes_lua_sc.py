#!/usr/bin/env python3
"""Convert nes_emu.bin to space-separated hex and patch local sc in nes.lua."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN_PATH = ROOT / "nes_emu.bin"
LUA_PATH = ROOT / "nes.lua"


def bin_to_hex(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def patch_sc(lua_text: str, hex_str: str) -> str:
    pat = re.compile(r'(local sc = ")[^"]*(")', re.DOTALL)
    if not pat.search(lua_text):
        raise SystemExit('could not find local sc = "..." in nes.lua')
    new_text, n = pat.subn(rf"\g<1>{hex_str}\2", lua_text, count=1)
    if n != 1:
        raise SystemExit(f"unexpected sc replace count={n}")
    return new_text


def update_sc(bin_path: Path = BIN_PATH, lua_path: Path = LUA_PATH) -> int:
    if not bin_path.is_file():
        raise SystemExit(f"missing {bin_path}")
    if not lua_path.is_file():
        raise SystemExit(f"missing {lua_path}")

    data = bin_path.read_bytes()
    hex_str = bin_to_hex(data)
    text = lua_path.read_text(encoding="utf-8", errors="replace")
    new_text = patch_sc(text, hex_str)
    lua_path.write_text(new_text, encoding="utf-8", newline="\n")
    return len(data)


def main() -> int:
    size = update_sc()
    print(f"bin size: {size} bytes")
    print(f"updated {LUA_PATH.name}: {size} bytes")
    free = 0x10000 - size
    print(f"JIT room: {free} bytes ({free / 0x10000 * 100:.1f}%)")
    if free < 0:
        print("WARNING: binary exceeds JIT_SIZE 0x10000", file=sys.stderr)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
