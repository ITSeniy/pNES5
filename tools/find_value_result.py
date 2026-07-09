from pathlib import Path
t = Path(__file__).with_name("AccuracyCoin.asm").read_text(errors="replace").splitlines()
for i, l in enumerate(t, 1):
    if "ValueAtAddressResult" in l or "ValueAtAddressForTest" in l:
        print(f"{i}:{l[:160]}")
