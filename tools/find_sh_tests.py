from pathlib import Path
p = Path(__file__).with_name("AccuracyCoin.asm")
text = p.read_text(errors="replace")
for i, line in enumerate(text.splitlines(), 1):
    if (
        "TEST_SH" in line
        or "TEST_LAE" in line
        or "TEST_SHA" in line
        or "UnOp_Cycle" in line
        or ("Behavior" in line and "SH" in line)
        or "RDY" in line
        or "CycleDelay" in line
        or "RunUnofficial" in line
        or "ExecuteUnofficial" in line
        or "opcode = $" in line
    ):
        print(f"{i}:{line[:160]}")
