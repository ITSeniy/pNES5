from pathlib import Path
t = Path(__file__).with_name("AccuracyCoin.asm").read_text(errors="replace").splitlines()
for i, l in enumerate(t, 1):
    s = l.strip()
    if s.startswith("DMASync") and not s.startswith("DMASync_50") and "JSR" not in s and "LDA" not in s:
        print(f"{i}:{l[:140]}")
    if s in ("DMASync:", "DMASyncWith40:", "DMASyncWith48:", "DMASyncWith60:", "DMASyncWith90:"):
        print(f"{i}:{l[:140]}")
# also search bare
for i, l in enumerate(t, 1):
    if l.strip().replace("\t", "") in ("DMASync:",) or l.startswith("DMASync:"):
        print("BARE", i, l)
for i, l in enumerate(t, 1):
    if "DMASync:" in l and "JSR" not in l:
        print(f"DEF {i}:{l[:140]}")
