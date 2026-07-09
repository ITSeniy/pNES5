#!/usr/bin/env python3
"""
pNES5 — one-stop CLI for build, payload, launch, ROM upload, and UDP logs.

Examples:
  python pnes5.py                  # interactive / uses saved config
  python pnes5.py 192.168.1.50     # launch to PS5
  python pnes5.py build            # make + patch nes.lua sc=
  python pnes5.py run 192.168.1.50
  python pnes5.py run --build --log
  python pnes5.py upload 192.168.1.50
  python pnes5.py log              # listen for UDP debug on :9027
  python pnes5.py config --ps5 192.168.1.50 --pc 192.168.1.10
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / ".pnes5.json"
LUA_PATH = ROOT / "nes.lua"
BIN_PATH = ROOT / "nes_emu.bin"
ROMS_DIR = ROOT / "roms"

PAYLOAD_PORT = 9026
FTP_PORT = 1337
LOG_PORT = 9027
JIT_SIZE = 0x10000


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

def load_config() -> dict:
    # Prefer .pnes5.json; fall back to legacy .emuc0re.json if present.
    for path in (CONFIG_PATH, ROOT / ".emuc0re.json"):
        if path.is_file():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except Exception:
                pass
    return {}


def save_config(cfg: dict) -> None:
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")


def guess_local_ip(remote: str | None = None) -> str | None:
    """Best-effort local IPv4 used to reach the PS5 (for PC_IP in nes.lua)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.5)
        target = remote or "8.8.8.8"
        s.connect((target, 1))
        ip = s.getsockname()[0]
        s.close()
        if ip and not ip.startswith("127."):
            return ip
    except Exception:
        pass
    try:
        return socket.gethostbyname(socket.gethostname())
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Build / payload
# ---------------------------------------------------------------------------

def _wsl_path(p: Path) -> str:
    """Windows path → WSL /mnt/<drive>/... path."""
    s = str(p.resolve()).replace("\\", "/")
    if len(s) >= 2 and s[1] == ":":
        drive = s[0].lower()
        rest = s[2:].lstrip("/")
        return f"/mnt/{drive}/{rest}"
    return s


def run_make(target: str = "") -> int:
    make = shutil.which("make")
    # On pure Windows, system32 make may be absent; use WSL if available.
    use_wsl = False
    if not make or sys.platform == "win32":
        wsl = shutil.which("wsl")
        if wsl and (not make or sys.platform == "win32"):
            # Prefer WSL gcc/toolchain for freestanding x86_64 payload
            if not make or os.environ.get("PNES5_USE_WSL", "1") != "0":
                use_wsl = bool(wsl)

    if use_wsl:
        wsl = shutil.which("wsl")
        tgt = target.strip()
        inner = f"cd {_wsl_path(ROOT)} && make {tgt}".strip()
        cmd = [wsl, "-e", "bash", "-lc", inner]
        print(f"  $ {' '.join(cmd)}")
        return subprocess.call(cmd)

    if not make:
        print("  [FAIL] make not found (install build tools or use WSL)", file=sys.stderr)
        return 1
    cmd = [make] + ([target] if target else [])
    print(f"  $ {' '.join(cmd)}")
    return subprocess.call(cmd, cwd=str(ROOT))


def patch_sc_from_bin() -> int:
    sys.path.insert(0, str(ROOT / "tools"))
    from update_nes_lua_sc import update_sc  # type: ignore

    size = update_sc(BIN_PATH, LUA_PATH)
    free = JIT_SIZE - size
    print(f"  nes_emu.bin → sc: {size} bytes  (JIT free {free})")
    if free < 0:
        print("  [WARN] binary exceeds JIT_SIZE 0x10000", file=sys.stderr)
        return 1
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    print("[build] compiling...")
    if getattr(args, "clean", False):
        run_make("clean")
    rc = run_make("")
    if not BIN_PATH.is_file():
        print(f"  [FAIL] missing {BIN_PATH} (make exited {rc})", file=sys.stderr)
        return rc if rc else 1
    if rc != 0:
        # WSL sometimes returns non-zero while still producing a binary
        print(f"  [WARN] make exited {rc}, but {BIN_PATH.name} exists — continuing")
    print("[build] embedding into nes.lua...")
    return patch_sc_from_bin()


# ---------------------------------------------------------------------------
# Lua payload prep (PC_IP on the fly)
# ---------------------------------------------------------------------------

def prepare_lua_payload(pc_ip: str | None) -> bytes:
    text = LUA_PATH.read_text(encoding="utf-8", errors="replace")
    if pc_ip:
        new_text, n = re.subn(
            r'(local PC_IP\s*=\s*")[^"]*(")',
            rf"\g<1>{pc_ip}\2",
            text,
            count=1,
        )
        if n != 1:
            print(f"  [WARN] could not patch PC_IP (kept file value)")
        else:
            text = new_text
            print(f"  PC_IP → {pc_ip} (for UDP logs)")
    data = text.encode("utf-8")
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
        print("  stripped UTF-8 BOM")
    return data


def send_payload(host: str, data: bytes, port: int = PAYLOAD_PORT) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        sock.connect((host, port))
        sock.sendall(data)
    finally:
        sock.close()
    print(f"  sent payload ({len(data):,} bytes) → {host}:{port}")


# ---------------------------------------------------------------------------
# FTP / ROMs
# ---------------------------------------------------------------------------

def scan_roms(roms_dir: Path, extensions: set[str]) -> list[Path]:
    if not roms_dir.is_dir():
        return []
    out = []
    for f in sorted(roms_dir.iterdir()):
        if f.is_file() and f.suffix.lower() in extensions:
            out.append(f)
    return out


def wait_for_ftp(host: str, port: int, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(1.5)
        try:
            s.connect((host, port))
            banner = s.recv(256)
            s.close()
            if b"220" in banner:
                return True
        except Exception:
            try:
                s.close()
            except Exception:
                pass
            time.sleep(0.4)
    return False


def send_site_exit(host: str, port: int) -> None:
    try:
        ftp = FTP()
        ftp.connect(host, port, timeout=10)
        ftp.login("anonymous", "")
        ftp.sendcmd("SITE EXIT")
        try:
            ftp.quit()
        except Exception:
            pass
        print("  FTP released (SITE EXIT)")
    except Exception as e:
        print(f"  [WARN] SITE EXIT: {e}")


def upload_roms(host: str, roms: list[Path], port: int = FTP_PORT) -> None:
    total = len(roms)
    total_size = sum(r.stat().st_size for r in roms)
    print(f"  {total} file(s), {total_size / 1048576:.1f} MB")

    ftp = FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login("anonymous", "")
    ftp.sendcmd("TYPE I")

    existing: set[str] = set()
    try:
        existing = set(ftp.nlst())
    except Exception:
        pass

    to_upload = []
    for rom in roms:
        fn = rom.name
        sz = rom.stat().st_size
        if fn in existing:
            try:
                if ftp.size(fn) == sz:
                    continue
            except Exception:
                pass
        to_upload.append(rom)

    try:
        ftp.sendcmd(f"SITE TOTAL {len(to_upload)}")
    except Exception:
        pass

    uploaded = 0
    skipped = total - len(to_upload)
    bytes_sent = 0
    t0 = time.time()

    for i, rom in enumerate(to_upload, 1):
        fn = rom.name
        sz = rom.stat().st_size
        try:
            with rom.open("rb") as f:
                ftp.storbinary(f"STOR {fn}", f, blocksize=8192)
            uploaded += 1
            bytes_sent += sz
            elapsed = time.time() - t0
            speed = bytes_sent / elapsed / 1024 if elapsed > 0 else 0
            print(f"  [{i}/{len(to_upload)}] {fn}  ({speed:.0f} KB/s)")
        except Exception as e:
            print(f"  [ERR] {fn}: {e}")

    elapsed = time.time() - t0
    print(f"  done: {uploaded} up, {skipped} skip  ({bytes_sent / 1048576:.1f} MB in {elapsed:.1f}s)")

    try:
        ftp.sendcmd("SITE EXIT")
    except Exception:
        pass
    try:
        ftp.quit()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# UDP log listener
# ---------------------------------------------------------------------------

def cmd_log(args: argparse.Namespace) -> int:
    port = args.port or LOG_PORT
    bind = args.bind or "0.0.0.0"
    print(f"[log] UDP listen {bind}:{port}  (Ctrl+C to stop)")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((bind, port))
    except OSError as e:
        print(f"  [FAIL] bind: {e}", file=sys.stderr)
        return 1
    try:
        while True:
            data, addr = sock.recvfrom(4096)
            try:
                msg = data.decode("utf-8", errors="replace").rstrip("\n")
            except Exception:
                msg = repr(data)
            print(f"[{addr[0]}] {msg}", flush=True)
    except KeyboardInterrupt:
        print("\n[log] stopped")
    finally:
        sock.close()
    return 0


# ---------------------------------------------------------------------------
# Run / upload
# ---------------------------------------------------------------------------

def resolve_ps5(args: argparse.Namespace, cfg: dict) -> str | None:
    ip = getattr(args, "ps5", None) or getattr(args, "ps5_ip", None) or cfg.get("ps5_ip")
    if ip:
        return ip
    return None


def resolve_pc_ip(args: argparse.Namespace, cfg: dict, ps5: str | None) -> str | None:
    if getattr(args, "pc_ip", None):
        return args.pc_ip
    if getattr(args, "no_pc_ip", False):
        return None
    if cfg.get("pc_ip"):
        return cfg["pc_ip"]
    return guess_local_ip(ps5)


def cmd_run(args: argparse.Namespace) -> int:
    cfg = load_config()
    ps5 = resolve_ps5(args, cfg)
    if not ps5:
        print("  [FAIL] PS5 IP required. Example: python pnes5.py run 192.168.1.50", file=sys.stderr)
        print("         or: python pnes5.py config --ps5 192.168.1.50", file=sys.stderr)
        return 2

    if getattr(args, "build", False):
        rc = cmd_build(args)
        if rc != 0:
            return rc
        print()

    if not LUA_PATH.is_file():
        print(f"  [FAIL] missing {LUA_PATH}", file=sys.stderr)
        return 1

    pc_ip = resolve_pc_ip(args, cfg, ps5)
    roms_dir = Path(args.roms_dir) if getattr(args, "roms_dir", None) else Path(cfg.get("roms_dir", ROMS_DIR))
    if not roms_dir.is_absolute():
        roms_dir = ROOT / roms_dir
    extensions = {e if e.startswith(".") else f".{e}" for e in (args.ext or [".nes"])}

    print(f"pNES5 → {ps5}")
    if pc_ip:
        print(f"  PC log IP: {pc_ip}:{LOG_PORT}")
    print(f"  ROMs: {roms_dir}")
    print()

    # remember IP
    cfg["ps5_ip"] = ps5
    if pc_ip:
        cfg["pc_ip"] = pc_ip
    save_config(cfg)

    log_stop = None
    if getattr(args, "log", False):
        import threading

        log_stop = threading.Event()

        def _log_thread():
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind(("0.0.0.0", LOG_PORT))
            except OSError as e:
                print(f"  [WARN] log bind :{LOG_PORT}: {e}")
                return
            sock.settimeout(0.5)
            print(f"[log] listening on :{LOG_PORT}")
            try:
                while not log_stop.is_set():
                    try:
                        data, addr = sock.recvfrom(4096)
                        msg = data.decode("utf-8", errors="replace").rstrip("\n")
                        print(f"[{addr[0]}] {msg}", flush=True)
                    except socket.timeout:
                        continue
            finally:
                sock.close()

        threading.Thread(target=_log_thread, daemon=True).start()
        time.sleep(0.15)

    print("[1] Sending launcher...")
    payload = prepare_lua_payload(pc_ip)
    try:
        send_payload(ps5, payload)
    except OSError as e:
        print(f"  [FAIL] connect {ps5}:{PAYLOAD_PORT}: {e}", file=sys.stderr)
        print("  Is LuaC0re listening? (Star Wars Racer Revenge + loader)", file=sys.stderr)
        return 1
    print()

    skip_upload = getattr(args, "skip_upload", False)
    roms: list[Path] = []
    if not skip_upload:
        roms = scan_roms(roms_dir, extensions)
        if roms:
            print(f"[2] Found {len(roms)} ROM(s)")
        else:
            print(f"[2] No ROMs in {roms_dir}")

    ftp_wait = getattr(args, "ftp_wait", 12)
    print(f"[3] Waiting for FTP :{FTP_PORT} (up to {ftp_wait}s)...")
    if wait_for_ftp(ps5, FTP_PORT, ftp_wait):
        print("  FTP ready")
        if roms and not skip_upload:
            print("[4] Uploading...")
            try:
                upload_roms(ps5, roms)
            except Exception as e:
                print(f"  FTP error: {e}")
                send_site_exit(ps5, FTP_PORT)
        else:
            send_site_exit(ps5, FTP_PORT)
    else:
        print(f"  FTP not ready after {ftp_wait}s (emu may still be running)")

    print()
    print("Done.  Emulator running on {}".format(ps5))
    if log_stop is not None:
        print("UDP log active — Ctrl+C to exit listener")
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            log_stop.set()
            print("\nbye")
    return 0


def cmd_upload(args: argparse.Namespace) -> int:
    cfg = load_config()
    ps5 = resolve_ps5(args, cfg)
    if not ps5:
        print("  [FAIL] PS5 IP required", file=sys.stderr)
        return 2
    roms_dir = Path(args.roms_dir) if args.roms_dir else Path(cfg.get("roms_dir", ROMS_DIR))
    if not roms_dir.is_absolute():
        roms_dir = ROOT / roms_dir
    extensions = {e if e.startswith(".") else f".{e}" for e in (args.ext or [".nes"])}
    roms = scan_roms(roms_dir, extensions)
    if not roms:
        print(f"  No ROMs in {roms_dir}")
        return 1
    if not wait_for_ftp(ps5, FTP_PORT, args.ftp_wait):
        print("  [FAIL] FTP not reachable — is the emu running?", file=sys.stderr)
        return 1
    upload_roms(ps5, roms)
    return 0


def cmd_config(args: argparse.Namespace) -> int:
    cfg = load_config()
    changed = False
    if args.ps5:
        cfg["ps5_ip"] = args.ps5
        changed = True
    if args.pc:
        cfg["pc_ip"] = args.pc
        changed = True
    if args.roms_dir:
        cfg["roms_dir"] = args.roms_dir
        changed = True
    if args.show or not changed:
        print("Config:", CONFIG_PATH)
        print(json.dumps(cfg, indent=2) if cfg else "  (empty)")
        g = guess_local_ip(cfg.get("ps5_ip"))
        if g:
            print(f"  guessed PC IP: {g}")
    if changed:
        save_config(cfg)
        print("  saved.")
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    cfg = load_config()
    print("pNES5 status")
    print(f"  root:     {ROOT}")
    print(f"  bin:      {BIN_PATH} {'OK' if BIN_PATH.is_file() else 'MISSING'}", end="")
    if BIN_PATH.is_file():
        sz = BIN_PATH.stat().st_size
        print(f"  ({sz} bytes, JIT free {JIT_SIZE - sz})")
    else:
        print()
    print(f"  lua:      {LUA_PATH} {'OK' if LUA_PATH.is_file() else 'MISSING'}")
    if LUA_PATH.is_file():
        m = re.search(r'local PC_IP\s*=\s*"([^"]*)"', LUA_PATH.read_text(encoding="utf-8", errors="replace"))
        if m:
            print(f"  PC_IP:    {m.group(1)} (in nes.lua)")
    print(f"  config:   {CONFIG_PATH}")
    print(f"  ps5_ip:   {cfg.get('ps5_ip', '(not set)')}")
    print(f"  pc_ip:    {cfg.get('pc_ip', guess_local_ip(cfg.get('ps5_ip')) or '(unknown)')}")
    roms = scan_roms(ROMS_DIR, {".nes"})
    print(f"  roms/:    {len(roms)} .nes file(s)")
    for r in roms[:8]:
        print(f"            - {r.name}")
    if len(roms) > 8:
        print(f"            ... +{len(roms) - 8} more")
    return 0


# ---------------------------------------------------------------------------
# argparse
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="pnes5",
        description="Build, embed, and launch pNES5 NES on PS5",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = p.add_subparsers(dest="cmd")

    # bare: python pnes5.py [IP]  → run
    p.add_argument("ps5_positional", nargs="?", help="PS5 IP (shortcut for 'run')")
    p.add_argument("--build", action="store_true", help="build+embed before launch")
    p.add_argument("--clean", action="store_true", help="make clean before build")
    p.add_argument("--log", action="store_true", help="listen for UDP debug logs")
    p.add_argument("--skip-upload", action="store_true")
    p.add_argument("--roms-dir", default=None)
    p.add_argument("--pc-ip", dest="pc_ip", default=None, help="PC IP for UDP logs (patched into payload)")
    p.add_argument("--no-pc-ip", action="store_true", help="do not patch PC_IP")
    p.add_argument("--ext", nargs="+", default=[".nes"])
    p.add_argument("--ftp-wait", type=int, default=12)

    b = sub.add_parser("build", help="make + embed sc into nes.lua")
    b.add_argument("--clean", action="store_true")

    r = sub.add_parser("run", help="send payload (+ optional upload)")
    r.add_argument("ps5", nargs="?", help="PS5 IP")
    r.add_argument("--build", action="store_true")
    r.add_argument("--clean", action="store_true")
    r.add_argument("--log", action="store_true")
    r.add_argument("--skip-upload", action="store_true")
    r.add_argument("--roms-dir", default=None)
    r.add_argument("--pc-ip", dest="pc_ip", default=None)
    r.add_argument("--no-pc-ip", action="store_true")
    r.add_argument("--ext", nargs="+", default=[".nes"])
    r.add_argument("--ftp-wait", type=int, default=12)

    u = sub.add_parser("upload", help="upload ROMs to running emu FTP")
    u.add_argument("ps5", nargs="?", help="PS5 IP")
    u.add_argument("--roms-dir", default=None)
    u.add_argument("--ext", nargs="+", default=[".nes"])
    u.add_argument("--ftp-wait", type=int, default=8)

    lg = sub.add_parser("log", help="UDP log listener")
    lg.add_argument("--port", type=int, default=LOG_PORT)
    lg.add_argument("--bind", default="0.0.0.0")

    c = sub.add_parser("config", help="save default PS5/PC IPs")
    c.add_argument("--ps5", default=None)
    c.add_argument("--pc", default=None)
    c.add_argument("--roms-dir", default=None)
    c.add_argument("--show", action="store_true")

    sub.add_parser("status", help="show paths, sizes, config")

    args = p.parse_args(argv)

    # subcommand dispatch
    if args.cmd == "build":
        return cmd_build(args)
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "upload":
        return cmd_upload(args)
    if args.cmd == "log":
        return cmd_log(args)
    if args.cmd == "config":
        return cmd_config(args)
    if args.cmd == "status":
        return cmd_status(args)

    # no subcommand: positional IP or flags → run / status
    if args.ps5_positional or args.build or args.log:
        # synthesize run args
        class R:
            pass
        rargs = R()
        rargs.ps5 = args.ps5_positional
        rargs.build = args.build
        rargs.clean = args.clean
        rargs.log = args.log
        rargs.skip_upload = args.skip_upload
        rargs.roms_dir = args.roms_dir
        rargs.pc_ip = args.pc_ip
        rargs.no_pc_ip = args.no_pc_ip
        rargs.ext = args.ext
        rargs.ftp_wait = args.ftp_wait
        return cmd_run(rargs)

    return cmd_status(args)


if __name__ == "__main__":
    raise SystemExit(main())
