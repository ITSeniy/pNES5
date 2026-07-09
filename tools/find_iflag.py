from pathlib import Path
t = Path(__file__).with_name("AccuracyCoin.asm").read_text(errors="replace").splitlines()
for i, l in enumerate(t, 1):
    if any(k in l for k in (
        "TEST_IFlagLatency", "TEST_NmiAndIrq", "IFlagLatency", "NmiAndIrq",
        "DMC IRQ", "dmc irq", "CLI", "set_test 1"
    )):
        if "IFlag" in l or "NmiAnd" in l or "DMC" in l or "TEST_I" in l or "TEST_N" in l:
            print(f"{i}:{l[:150]}")
