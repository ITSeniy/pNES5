# pNES5

<p align="center">
  <img src="pNES5.png" alt="pNES5" width="280">
</p>

NES emulator for PS5 as native x86_64 shellcode, running through [LuaC0re](https://github.com/Gezine/Luac0re) (no kernel exploit). Tested up to firmware **13.00**.

Forked from [EmuC0re](https://github.com/egycnq/EmuC0re) (EgyDevTeam / egycnq). Same LuaC0re shellcode approach; this repo focuses on a single NES host with tighter APU/PPU behavior, DualSense controls, and an in-game settings menu.

## Features

- Full 6502 (including illegals)
- PPU: scrolling, sprites, sprite 0
- APU at 48 kHz (vsync-friendly buffering)
- DualSense (native)
- FTP ROM upload, library picker, save states
- Settings: pixel-perfect / stretch / 2×–4× scale, turbo rate, reset

## Mappers

Logic is in `src/mapper.c`. The bus only dispatches.

| # | Name | Notes |
| --- | --- | --- |
| 0 | NROM | |
| 1 | MMC1 | Consecutive-write filter, PRG-RAM disable |
| 2 | UxROM | |
| 3 | CNROM | |
| 4 | MMC3 | A12 + scanline IRQ, WRAM protect, four-screen |
| 7 | AxROM | |
| 9 | MMC2 | |
| 10 | MMC4 | |
| 11 | Color Dreams | |
| 13 | CPROM | 4KB CHR-RAM at `$1000` |
| 34 | BNROM / NINA-001 | `$8000` BNROM + `$7FFD–7FFF` NINA |
| 66 | GxROM | |
| 69 | FME-7 | PRG slots, `$6000` ROM/RAM, CPU IRQ |
| 70 | Bandai 74161 | |
| 71 | Camerica | |
| 78 | Irem / Holy Diver | |
| 79 | NINA-03/06 | |
| 87 | J87 | |
| 93 | Sunsoft-2 | |
| 94 | UxROM V | |
| 113 | NINA-03/06 | + mirroring |
| 140 | Jaleco JF-11 | |
| 152 | Bandai 74161 | |
| 180 | Inv UxROM | |
| 185 | CNROM CP | CHR disable / open bus |
| 206 | DxROM | |

Unsupported mappers will not run. MMC3 scanline IRQ is approximate (blargg `4-scanline_timing` fails). MMC6-only WRAM tests are incomplete.

## Requirements

- PS5 (tested through 13.00)
- [LuaC0re](https://github.com/Gezine/Luac0re)
- *Star Wars Racer Revenge* — US `CUSA03474` or EU `CUSA03492`
- Python 3 on the PC, same LAN as the console

## Build & launch

```bash
make clean && make
make payload          # embed nes_emu.bin into nes.lua (sc=)
```

Or:

```bash
python pnes5.py config --ps5 192.168.1.50
python pnes5.py run --build --log
python pnes5.py upload
python pnes5.py log
```

`pnes5.py` patches `PC_IP` in the payload for UDP logs on port **9027**, stores settings in `.pnes5.json`, and skips ROM uploads that are already on the console.

Legacy: `python nes_launcher.py <PS5_IP>` still works.

## Host tests (no PS5)

```bash
make test
make romtest
./tests/nes_romtest path/to/test.nes --frames 600 --expect-pass
./tests/nes_romtest path/to/nestest.nes --nestest --steps 8991
```

## Usage

Put `.nes` files in `roms/` next to the CLI; the launcher FTPs them after the payload starts (port **1337**). You can also drop ROMs into the savedata path and refresh the library with **L1**.

Optional UDP log: set `PC_IP` in `nes.lua`, then `nc -u -l -p 9027` or `python pnes5.py log`.

### Controls

**Library**

| Input | Action |
| --- | --- |
| D-Pad | Navigate |
| Cross / Start | Launch |
| L1 | Rescan ROMs on disk |
| R1 | Exit |

**In-game**

| DualSense | Action |
| --- | --- |
| Cross | A |
| Circle | B |
| Square | Turbo A |
| Triangle | Turbo B |
| Create / Touch pad | Select |
| Options | Start |
| D-Pad | D-Pad |
| L2 / R2 | Save / load state (edge) |
| L1 | Library |
| R1 | Settings |
| L3 + R3 | Soft reset |

**Settings (R1)** — scale (pixel perfect / stretch / 2×–4×), turbo rate, save/load, reset, library, exit. Cross confirms, Circle closes, Left/Right change values.

## TODO

- [ ] More mappers (VRC2/4/6, MMC5, Namco 163, …)
- [ ] Cycle-accurate MMC3 A12 during rendering

## Credits

- [EmuC0re](https://github.com/egycnq/EmuC0re) — EgyDevTeam / [egycnq](https://github.com/egycnq), with [Abkarino](https://github.com/AbkarinoMHM)
- [Gezine](https://github.com/Gezine/Luac0re) — LuaC0re
- [CTurt](https://github.com/CTurt), [McCaulay](https://github.com/McCaulay) — mast1c0re
- [ChampionLeake](https://github.com/ChampionLeake) — Racer Revenge notes on [psdevwiki](https://www.psdevwiki.com/ps2/Vulnerabilities)
- [shahrilnet](https://github.com/shahrilnet/remote_lua_loader), [null_ptr](https://github.com/n0llptr)
- [NESDev](https://www.nesdev.org/wiki/), [nondebug/dualsense](https://github.com/nondebug/dualsense)

## Disclaimer

Research / educational use only. Use at your own risk.

## Русский

pNES5 — эмулятор NES для PS5 в виде нативного x86-64 shellcode, запускаемого через LuaC0re.

MIT относится только к авторскому коду; лицензии сторонних компонентов сохраняются.
