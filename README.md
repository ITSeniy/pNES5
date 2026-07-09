# EmuC0re

Emulators running as native x86_64 shellcode on PS5 through the [LuaC0re](https://github.com/Gezine/Luac0re) JIT exploit.

PoC that full homebrew apps can be built and run from PS5 userland — no kernel exploit needed.  
Works on all PS5 firmwares up to **13.00** (latest).

## NES Emulator

First emulator in the project

- Full 6502 CPU (all 256 opcodes including illegals)
- PPU with scrolling, sprites, sprite 0 hit
- APU with 48kHz output
- DualSense native pad support
- Web-based touch/WebHID controller
- Built-in FTP server for ROM upload
- ROM picker menu
- Save states
- NTSC/PAL

### Supported Mappers

Mapper logic lives in `src/mapper.c` (init / PRG / CHR / IRQ hooks). The CPU/PPU bus in `src/bus.c` only dispatches.

| #   | Name      | Notes |
| --- | --------- | ----- |
| 0   | NROM      | |
| 1   | MMC1      | Consecutive-write filter, PRG-RAM disable |
| 2   | UxROM     | |
| 3   | CNROM     | |
| 4   | MMC3      | A12 + scanline IRQ, WRAM protect, four-screen |
| 7   | AxROM     | |
| 9   | MMC2      | |
| 10  | MMC4      | |
| 11  | Color Dreams | |
| 13  | CPROM     | 4KB CHR-RAM switch at `$1000` |
| 34  | BNROM / NINA-001 | `$8000` BNROM + `$7FFD–7FFF` NINA |
| 66  | GxROM     | |
| 69  | FME-7     | Correct PRG slots, `$6000` ROM/RAM, CPU IRQ |
| 70  | Bandai 74161 | |
| 71  | Camerica  | |
| 78  | Irem/Holy Diver | |
| 79  | NINA-03/06 | AVE multicarts / unlicensed |
| 87  | J87       | |
| 93  | Sunsoft-2 | |
| 94  | UxROM V   | |
| 113 | NINA-03/06 | + mirroring control |
| 140 | Jaleco JF-11 | |
| 152 | Bandai 74161 | |
| 180 | Inv UxROM | |
| 185 | CNROM CP  | CHR disable / open bus |
| 206 | DxROM     | |

CHR banking applies to both CHR-ROM and CHR-RAM. Games on unsupported mappers won't run.

**Known limits:** MMC3 scanline IRQ timing is approximate (not PPU-dot accurate); blargg `4-scanline_timing` fails. MMC6-only WRAM tests are not fully covered.

## Requirements

- PS5 console (any firmware, tested up to 13.00)
- [LuaC0re](https://github.com/Gezine/Luac0re) set up and working
- _Star Wars Racer Revenge_ — US (CUSA03474) or EU (CUSA03492)  
  If you're on latest FW you can grab the digital version from the PS Store
- Python 3 on your PC
- PC and PS5 on the same network

## Building

```
make clean && make
make payload          # embed nes_emu.bin hex into nes.lua (sc=)
```

Or one shot via the CLI:

```
python emuc0re.py build
python emuc0re.py build --clean
```

## Launch (recommended)

```
# save defaults once
python emuc0re.py config --ps5 192.168.1.50

# build + send + upload roms/ + optional UDP log
python emuc0re.py run --build --log
python emuc0re.py 192.168.1.50 --build --log

# upload only (emu already running)
python emuc0re.py upload

# UDP debug log only
python emuc0re.py log
```

`emuc0re.py` patches `PC_IP` in the payload on the fly (for UDP logs on port 9027), remembers the last PS5 IP in `.emuc0re.json`, and skips ROM uploads that are already on the console.

Legacy: `python nes_launcher.py <PS5_IP>` still works.

## Host ROM Tests

The core can also be exercised from WSL/Linux without launching the PS5 payload.

```
make test
make romtest
```

Run blargg-style test ROMs:

```
./tests/nes_romtest path/to/test.nes --frames 600 --expect-pass
```

Run a quick `nestest.nes` CPU smoke pass starting at `$C000`:

```
./tests/nes_romtest path/to/nestest.nes --nestest --steps 8991
```

For blargg ROMs the runner prints the standard `$6000` status and the message
string at `$6004` when present.

## Usage

### Setup

Optional — listen for debug log on your PC:

Edit `nes.lua` and set your PC's IP for debug logs:

```lua
local PC_IP = "192.168.1.121"
```

```
nc -u -l -p 9027
```

### Launch

```
python nes_launcher.py <PS5_IP>
```

This sends the payload to the LuaC0re loader (port 9026), waits for the FTP server, uploads ROMs from the `roms/` folder, then starts the emulator.

Options:

```
--roms-dir PATH    ROM folder (default: ./roms)
--skip-upload      Launch without uploading ROMs
--launcher PATH    Custom lua file (default: ./nes.lua)
--ext .nes .rom    File extensions to scan
--ftp-wait SEC     FTP timeout (default: 10)
```

### ROMs

**FTP upload :** put `.nes` files in a `roms/` folder next to the script, the launcher handles the rest.

or

**Manual:** place files directly in the savedata directory, they'll show up in the nes emu picker.

### Controls

ROM picker:

- **D-Pad** — navigate
- **Cross / Start** — launch
- **L1** — back to menu
- **R1** — exit

In-game:

| DualSense        | NES    |
| ---------------- | ------ |
| Cross            | A      |
| Square           | B      |
| Triangle         | Select |
| Circle / Options | Start  |
| D-Pad            | D-Pad  |
| L2               | Save state |
| R2               | Load state |
| L1               | Menu |
| R1               | Exit |

### Input Sources

Two options: **native DualSense** or **web controller** (open `http://<PS5_IP>:9030` on your phone/PC)

The input source locks on the first button press and stays for the entire emulator session. You can't switch mid-session — you need to relaunch the emulator to change input method.

The web controller supports touch buttons, Gamepad API, and DualSense WebHID (Chrome/Edge, USB or BT).
The web keyboard shortcuts are **F5** for save state, **F9** for load state, **Esc** for menu, and **Tab** for exit.

## TODO

- [ ] More mappers (VRC2/4/6, MMC5, Namco 163, 79/113, …)
- [ ] Cycle-accurate MMC3 A12 during rendering
- [ ] SNES emulator
- [ ] Game Boy / GBC emulator
- [ ] maybe more emulators lets see...

## Credits

**EgyDevTeam**

Special thanks to [Abkarino](https://github.com/AbkarinoMHM) — co-founder of EgyDevTeam.

- [Gezine](https://github.com/Gezine/Luac0re) — LuaC0re framework and JIT exploit
- [CTurt](https://github.com/CTurt) — [mast1c0re](https://cturt.github.io/mast1c0re.html) writeup
- [McCaulay](https://github.com/McCaulay) — [mast1c0re](https://mccaulay.co.uk/mast1c0re-part-2-arbitrary-ps2-code-execution/) writeup and [Okage](https://github.com/McCaulay/mast1c0re) reference implementation
- [ChampionLeake](https://github.com/ChampionLeake) — PS2 _Star Wars Racer Revenge_ exploit writeup on [psdevwiki](https://www.psdevwiki.com/ps2/Vulnerabilities)
- [shahrilnet](https://github.com/shahrilnet/remote_lua_loader) & [null_ptr](https://github.com/n0llptr) — Code references from [remote_lua_loader](https://github.com/shahrilnet/remote_lua_loader)
- [NESDev Wiki](https://www.nesdev.org/wiki/) & community — NES hardware documentation
- [nondebug/dualsense](https://github.com/nondebug/dualsense) — DualSense HID docs

## Disclaimer

For research and educational purposes only. Use at your own risk.
