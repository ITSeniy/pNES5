from pathlib import Path
t = Path(__file__).with_name("AccuracyCoin.asm").read_text(errors="replace").splitlines()
for i, l in enumerate(t, 1):
    if any(k in l for k in (
        "DMASync", "PostDMACycles", "CycleDelayPostDMA", "TEST_UnOp_Setup",
        "TEST_RunTest_Addr", "UnOp_Exec", "ExecuteOpcode", "RunTest"
    )):
        print(f"{i}:{l[:160]}")
