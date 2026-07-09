prg = open("roms/AccuracyCoin.nes", "rb").read()[16 : 16 + 32768]
print("len", len(prg))
for i in range(len(prg) - 64):
    if prg[i : i + 32] == bytes(32) and prg[i + 32 : i + 64] == bytes([0xFF] * 32):
        print("00*32 FF*32 at prg", hex(i), "cpu", hex(0x8000 + i))
found32 = False
for i in range(len(prg) - 32):
    if prg[i : i + 32] == bytes(32):
        print("32 zeros at", hex(i), "cpu", hex(0x8000 + i))
        found32 = True
        if not found32:
            break
# show FFC0 again
off = 0x7FC0
print("FFC0 data:", prg[off : off + 64].hex())
# C000 silent
print("C000 data:", prg[0x4000 : 0x4000 + 40].hex())
