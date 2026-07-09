from pathlib import Path
t = Path(__file__).with_name("AccuracyCoin.asm").read_text(errors="replace").splitlines()
for i, l in enumerate(t, 1):
    if any(k in l for k in ("TEST_UnOp_RamFunc", "CycleDelayPostDMA", "DMASync_50Minus", "UnOpTest_Opcode")):
        print(f"{i}:{l[:160]}")
