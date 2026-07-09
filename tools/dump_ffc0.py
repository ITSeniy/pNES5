import sys

path = "roms/AccuracyCoin.nes"
with open(path, "rb") as f:
    hdr = f.read(16)
    prg_size = hdr[4] * 16384
    if (hdr[7] & 0x0C) == 0x08:
        prg_size = ((hdr[9] & 0x0F) << 8 | hdr[4]) * 16384
    prg = f.read(prg_size)

mapper = (hdr[7] & 0xF0) | (hdr[6] >> 4)
print("mapper", mapper, "prg", prg_size, "banks", prg_size // 0x4000)

# AccuracyCoin often uses last bank at C000-FFFF; dump each 16k bank tail
for bi in range(prg_size // 0x4000):
    bank = prg[bi * 0x4000 : (bi + 1) * 0x4000]
    region = bank[0x3FC0:0x4000]
    nonzero = sum(1 for b in region if b)
    if nonzero or bi >= (prg_size // 0x4000) - 2:
        print("bank", bi, "FFC0 unique", sorted(set(region)), "nonzero", nonzero)
        print("  FFC0", region[0:0x20].hex())
        print("  FFE0", region[0x20:0x40].hex())
