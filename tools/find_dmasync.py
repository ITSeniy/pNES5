from pathlib import Path
t = Path(__file__).with_name("AccuracyCoin.asm").read_text(errors="replace").splitlines()
for i, l in enumerate(t, 1):
    if "DMASync" in l and not l.strip().startswith(";"):
        print(f"{i}:{l[:140]}")
