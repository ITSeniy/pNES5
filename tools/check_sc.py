#!/usr/bin/env python3
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sc_bytes(text: str) -> bytes:
    m = re.search(r'local sc = "([^"]*)"', text)
    if not m:
        raise SystemExit("no sc")
    return bytes(int(x, 16) for x in m.group(1).split())


def main() -> None:
    cur = sc_bytes((ROOT / "nes.lua").read_text(encoding="utf-8", errors="replace"))
    old_text = subprocess.check_output(
        ["git", "show", "c944775:nes.lua"], cwd=ROOT, text=True, errors="replace"
    )
    old = sc_bytes(old_text)
    bin_data = (ROOT / "nes_emu.bin").read_bytes()
    print(f"cur sc: {len(cur)}  old sc: {len(old)}  bin: {len(bin_data)}")
    print(f"cur==bin: {cur == bin_data}")
    print(f"cur head: {cur[:16].hex()}")
    print(f"old head: {old[:16].hex()}")
    print(f"bin head: {bin_data[:16].hex()}")
    print(f"entry same as old: {cur[:16] == old[:16]}")
    # free JIT
    print(f"JIT free: {0x10000 - len(cur)}")


if __name__ == "__main__":
    main()
