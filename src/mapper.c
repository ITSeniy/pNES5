#include "mapper.h"

static int bank_mod(int bank, int count) {
    if (count <= 0) return 0;
    bank %= count;
    return bank < 0 ? bank + count : bank;
}

int mapper_supported(int mapper) {
    switch (mapper) {
    case 0: case 1: case 2: case 3: case 4: case 7:
    case 9: case 10: case 11: case 13: case 34: case 66:
    case 69: case 70: case 71: case 78: case 79: case 87:
    case 93: case 94: case 113: case 140: case 152:
    case 180: case 185: case 206:
        return 1;
    default:
        return 0;
    }
}

static int prg_8k_count(struct NES *nes) {
    return nes->prg_size > 0 ? (nes->prg_size / 0x2000) : 1;
}

static int prg_16k_count(struct NES *nes) {
    return nes->prg_size > 0 ? (nes->prg_size / 0x4000) : 1;
}

static int prg_32k_count(struct NES *nes) {
    return nes->prg_size > 0 ? (nes->prg_size / 0x8000) : 1;
}

static int chr_1k_count(struct NES *nes) {
    return nes->chr_size > 0 ? (nes->chr_size / 0x400) : 1;
}

static int chr_4k_count(struct NES *nes) {
    return nes->chr_size > 0 ? (nes->chr_size / 0x1000) : 1;
}

static int chr_8k_count(struct NES *nes) {
    return nes->chr_size > 0 ? (nes->chr_size / 0x2000) : 1;
}

static u8 prg_byte(struct NES *nes, u32 offset) {
    if (nes->prg_size <= 0 || !nes->prg) return 0;
    return nes->prg[offset % (u32)nes->prg_size];
}

static u8 chr_byte(struct NES *nes, u32 offset) {
    if (nes->chr_size <= 0 || !nes->chr) return 0;
    return nes->chr[offset % (u32)nes->chr_size];
}

static void chr_store(struct NES *nes, u32 offset, u8 val) {
    if (!nes->chr_is_ram || !nes->chr || nes->chr_size <= 0) return;
    nes->chr[offset % (u32)nes->chr_size] = val;
}

/* ---- CHR bank resolve (applies even when CHR is RAM) ---- */

static u32 chr_map(struct NES *nes, u16 addr) {
    addr &= 0x1FFF;
    switch (nes->mapper) {
    case 1: {
        int bank;
        if (nes->mmc1_ctrl & 0x10) {
            bank = (addr < 0x1000) ? nes->mmc1_chr0 : nes->mmc1_chr1;
            bank = bank_mod(bank, chr_4k_count(nes));
            return (u32)bank * 0x1000 + (addr & 0xFFF);
        }
        bank = bank_mod(nes->mmc1_chr0 >> 1, chr_8k_count(nes));
        return (u32)bank * 0x2000 + addr;
    }
    case 3: case 11: case 66: case 70: case 78: case 87:
    case 140: case 152: case 185: {
        int bank = bank_mod(nes->chr_bank, chr_8k_count(nes));
        return (u32)bank * 0x2000 + addr;
    }
    case 13: {
        /* CPROM: fixed 4KB at $0000, switchable 4KB at $1000 */
        if (addr < 0x1000)
            return addr;
        {
            int bank = bank_mod(nes->chr_bank, chr_4k_count(nes));
            return (u32)bank * 0x1000 + (addr & 0xFFF);
        }
    }
    case 34: {
        /* NINA-001 uses 4KB CHR banks via mmc1_chr0/1; BNROM has fixed 8KB. */
        if (nes->chr_size > 0x2000 || nes->mmc1_chr0 || nes->mmc1_chr1) {
            int bank = (addr < 0x1000) ? nes->mmc1_chr0 : nes->mmc1_chr1;
            bank = bank_mod(bank, chr_4k_count(nes));
            return (u32)bank * 0x1000 + (addr & 0xFFF);
        }
        return addr;
    }
    case 79: case 113: {
        int bank = bank_mod(nes->chr_bank, chr_8k_count(nes));
        return (u32)bank * 0x2000 + addr;
    }
    case 4: case 206: {
        int bank;
        int mask = (nes->mapper == 206) ? 0x3F : 0xFF;
        if (nes->mmc3_chr_mode == 0) {
            if      (addr < 0x0400) bank = nes->mmc3_regs[0] & 0xFE;
            else if (addr < 0x0800) bank = nes->mmc3_regs[0] | 1;
            else if (addr < 0x0C00) bank = nes->mmc3_regs[1] & 0xFE;
            else if (addr < 0x1000) bank = nes->mmc3_regs[1] | 1;
            else if (addr < 0x1400) bank = nes->mmc3_regs[2];
            else if (addr < 0x1800) bank = nes->mmc3_regs[3];
            else if (addr < 0x1C00) bank = nes->mmc3_regs[4];
            else                  bank = nes->mmc3_regs[5];
        } else {
            if      (addr < 0x0400) bank = nes->mmc3_regs[2];
            else if (addr < 0x0800) bank = nes->mmc3_regs[3];
            else if (addr < 0x0C00) bank = nes->mmc3_regs[4];
            else if (addr < 0x1000) bank = nes->mmc3_regs[5];
            else if (addr < 0x1400) bank = nes->mmc3_regs[0] & 0xFE;
            else if (addr < 0x1800) bank = nes->mmc3_regs[0] | 1;
            else if (addr < 0x1C00) bank = nes->mmc3_regs[1] & 0xFE;
            else                  bank = nes->mmc3_regs[1] | 1;
        }
        bank = bank_mod(bank & mask, chr_1k_count(nes));
        return (u32)bank * 0x400 + (addr & 0x3FF);
    }
    case 9: case 10: {
        int bank = (addr < 0x1000)
            ? nes->mmc2_chr_lo[nes->mmc2_latch0]
            : nes->mmc2_chr_hi[nes->mmc2_latch1];
        bank = bank_mod(bank, chr_4k_count(nes));
        return (u32)bank * 0x1000 + (addr & 0xFFF);
    }
    case 69: {
        int bank = bank_mod(nes->fme7_chr[(addr >> 10) & 7], chr_1k_count(nes));
        return (u32)bank * 0x400 + (addr & 0x3FF);
    }
    default:
        return addr;
    }
}

u8 mapper_chr_read(struct NES *nes, u16 addr) {
    addr &= 0x1FFF;
    if (!nes->chr_enable)
        return 0xFF;
    u8 val = chr_byte(nes, chr_map(nes, addr));

    /* MMC2/MMC4 latches fire after the read. */
    if (nes->mapper == 9 || nes->mapper == 10) {
        if (addr == 0x0FD8) nes->mmc2_latch0 = 0;
        else if (addr == 0x0FE8) nes->mmc2_latch0 = 1;
        else if (addr >= 0x1FD8 && addr <= 0x1FDF) nes->mmc2_latch1 = 0;
        else if (addr >= 0x1FE8 && addr <= 0x1FEF) nes->mmc2_latch1 = 1;
    }
    return val;
}

void mapper_chr_write(struct NES *nes, u16 addr, u8 val) {
    if (!nes->chr_is_ram || !nes->chr_enable)
        return;
    chr_store(nes, chr_map(nes, addr & 0x1FFF), val);
}

/* ---- PRG map ---- */

u8 mapper_prg_read(struct NES *nes, u16 addr) {
    u32 off = (u32)(addr - 0x8000);
    switch (nes->mapper) {
    case 1: {
        int mode = (nes->mmc1_ctrl >> 2) & 3;
        int bank = nes->mmc1_prg & 0x0F;
        int banks16 = prg_16k_count(nes);
        if (mode <= 1) {
            bank = bank_mod(bank >> 1, prg_32k_count(nes));
            return prg_byte(nes, (u32)bank * 0x8000 + off);
        }
        if (mode == 2) {
            if (addr < 0xC000) return prg_byte(nes, off);
            bank = bank_mod(bank, banks16);
            return prg_byte(nes, (u32)bank * 0x4000 + (addr - 0xC000));
        }
        /* mode 3: switch $8000, fix last at $C000 */
        if (addr < 0xC000) {
            bank = bank_mod(bank, banks16);
            return prg_byte(nes, (u32)bank * 0x4000 + off);
        }
        return prg_byte(nes, (u32)(banks16 - 1) * 0x4000 + (addr - 0xC000));
    }
    case 2: case 70: case 71: case 78: case 93: case 94: case 152: {
        int banks16 = prg_16k_count(nes);
        if (addr < 0xC000) {
            int bank = bank_mod(nes->prg_bank, banks16);
            return prg_byte(nes, (u32)bank * 0x4000 + off);
        }
        return prg_byte(nes, (u32)(banks16 - 1) * 0x4000 + (addr - 0xC000));
    }
    case 4: {
        int n8 = prg_8k_count(nes);
        int b;
        if (nes->mmc3_prg_mode == 0) {
            if      (addr < 0xA000) b = nes->mmc3_regs[6];
            else if (addr < 0xC000) b = nes->mmc3_regs[7];
            else if (addr < 0xE000) b = n8 - 2;
            else                  b = n8 - 1;
        } else {
            if      (addr < 0xA000) b = n8 - 2;
            else if (addr < 0xC000) b = nes->mmc3_regs[7];
            else if (addr < 0xE000) b = nes->mmc3_regs[6];
            else                  b = n8 - 1;
        }
        b = bank_mod(b, n8);
        return prg_byte(nes, (u32)b * 0x2000 + (addr & 0x1FFF));
    }
    case 7: case 11: case 13: case 34: case 66: case 79:
    case 113: case 140: {
        int bank = bank_mod(nes->prg_bank, prg_32k_count(nes));
        return prg_byte(nes, (u32)bank * 0x8000 + off);
    }
    case 9: {
        /* MMC2: 8KB switch at $8000, fixed last three 8KB */
        if (addr < 0xA000) {
            int bank = bank_mod(nes->prg_bank, prg_8k_count(nes));
            return prg_byte(nes, (u32)bank * 0x2000 + off);
        }
        return prg_byte(nes, (u32)nes->prg_size - 0x6000 + (addr - 0xA000));
    }
    case 10: {
        int banks16 = prg_16k_count(nes);
        if (addr < 0xC000) {
            int bank = bank_mod(nes->prg_bank, banks16);
            return prg_byte(nes, (u32)bank * 0x4000 + off);
        }
        return prg_byte(nes, (u32)(banks16 - 1) * 0x4000 + (addr - 0xC000));
    }
    case 69: {
        /* $8000/$A000/$C000 switched; $E000 fixed last 8KB */
        int n8 = prg_8k_count(nes);
        int slot, bank;
        if (addr >= 0xE000) {
            bank = n8 - 1;
        } else {
            slot = ((addr - 0x8000) >> 13) + 1; /* 1,2,3 */
            bank = bank_mod(nes->fme7_prg[slot] & 0x3F, n8);
        }
        return prg_byte(nes, (u32)bank * 0x2000 + (addr & 0x1FFF));
    }
    case 180: {
        int banks16 = prg_16k_count(nes);
        if (addr < 0xC000) return prg_byte(nes, off);
        {
            int bank = bank_mod(nes->prg_bank, banks16);
            return prg_byte(nes, (u32)bank * 0x4000 + (addr - 0xC000));
        }
    }
    case 206: {
        int n8 = prg_8k_count(nes);
        int b;
        if      (addr < 0xA000) b = nes->mmc3_regs[6] & 0x0F;
        else if (addr < 0xC000) b = nes->mmc3_regs[7] & 0x0F;
        else if (addr < 0xE000) b = n8 - 2;
        else                  b = n8 - 1;
        b = bank_mod(b, n8);
        return prg_byte(nes, (u32)b * 0x2000 + (addr & 0x1FFF));
    }
    default:
        if (nes->prg_size == 0x4000)
            off &= 0x3FFF;
        return prg_byte(nes, off);
    }
}

/* PRG-RAM at $6000-$7FFF: open bus (0) when chip disables it. */
static int prg_ram_readable(struct NES *nes) {
    switch (nes->mapper) {
    case 1:
        /* MMC1 $E000 bit4 = 1 disables PRG-RAM */
        return !(nes->mmc1_prg & 0x10);
    case 4:
        return nes->mmc3_wram_enable;
    case 69:
        return (nes->fme7_ram_mode & 0xC0) == 0xC0;
    default:
        return 1;
    }
}

static int prg_ram_writable(struct NES *nes) {
    switch (nes->mapper) {
    case 1:
        return !(nes->mmc1_prg & 0x10);
    case 4:
        return nes->mmc3_wram_enable && !nes->mmc3_wram_protect;
    case 69:
        return (nes->fme7_ram_mode & 0xC0) == 0xC0;
    default:
        return 1;
    }
}

/* $6000-$7FFF cart area */
u8 mapper_cpu_read(struct NES *nes, u16 addr) {
    if (addr >= 0x8000)
        return mapper_prg_read(nes, addr);

    if (addr >= 0x6000 && addr < 0x8000) {
        if (nes->mapper == 69) {
            u8 mode = nes->fme7_ram_mode;
            if (mode & 0x40) {
                if (!(mode & 0x80))
                    return 0;
                return nes->sram[addr - 0x6000];
            }
            /* PRG ROM banked at $6000 */
            int bank = bank_mod(nes->fme7_prg[0] & 0x3F, prg_8k_count(nes));
            return prg_byte(nes, (u32)bank * 0x2000 + (addr & 0x1FFF));
        }
        if (!prg_ram_readable(nes))
            return 0;
        return nes->sram[addr - 0x6000];
    }
    return 0;
}

static void mmc3_clock(struct NES *nes) {
    if (nes->mmc3_irq_count == 0 || nes->mmc3_irq_reload) {
        nes->mmc3_irq_count = nes->mmc3_irq_latch;
        nes->mmc3_irq_reload = 0;
    } else {
        nes->mmc3_irq_count--;
    }
    if (nes->mmc3_irq_count == 0 && nes->mmc3_irq_enable)
        nes->irq_pending = 1;
}

void mapper_notify_a12(struct NES *nes, u16 ppu_addr) {
    if (nes->mapper != 4) return;
    u8 a12 = (ppu_addr & 0x1000) ? 1 : 0;
    if (a12 && !nes->mmc3_a12)
        mmc3_clock(nes);
    nes->mmc3_a12 = a12;
}

void mapper_scanline_clock(struct NES *nes) {
    if (nes->mapper != 4) return;
    if (!(nes->ppu_mask & 0x18)) return;
    mmc3_clock(nes);
}

void mapper_cpu_clock(struct NES *nes, int cycles) {
    if (nes->mapper != 69 || cycles <= 0) return;
    /* Counter runs when bit 7 of IRQ control is set. */
    if (!(nes->fme7_irq_ctrl & 0x80)) return;
    while (cycles--) {
        if (nes->fme7_irq_counter == 0) {
            nes->fme7_irq_counter = 0xFFFF;
            if (nes->fme7_irq_ctrl & 0x01)
                nes->irq_pending = 1;
        } else {
            nes->fme7_irq_counter--;
            if (nes->fme7_irq_counter == 0 && (nes->fme7_irq_ctrl & 0x01))
                nes->irq_pending = 1;
        }
    }
}

void mapper_init(struct NES *nes) {
    nes->prg_bank = 0;
    nes->chr_bank = 0;
    nes->chr_enable = 1;
    nes->mmc1_shift = 0;
    nes->mmc1_count = 0;
    nes->mmc1_chr0 = 0;
    nes->mmc1_chr1 = 0;
    nes->mmc1_prg = 0;
    nes->mmc1_last_write_cycle = -2;
    nes->mmc3_select = 0;
    nes->mmc3_prg_mode = 0;
    nes->mmc3_chr_mode = 0;
    nes->mmc3_irq_latch = 0;
    nes->mmc3_irq_count = 0;
    nes->mmc3_irq_enable = 0;
    nes->mmc3_irq_reload = 0;
    nes->mmc3_a12 = 0;
    nes->mmc3_wram_enable = 1;
    nes->mmc3_wram_protect = 0;
    nes->mmc2_latch0 = 0;
    nes->mmc2_latch1 = 0;
    nes->fme7_cmd = 0;
    nes->fme7_irq_ctrl = 0;
    nes->fme7_irq_counter = 0;
    nes->fme7_ram_mode = 0xC0; /* RAM enabled by default for battery games */

    for (int i = 0; i < 8; i++)
        nes->mmc3_regs[i] = 0;
    nes->mmc2_chr_lo[0] = nes->mmc2_chr_lo[1] = 0;
    nes->mmc2_chr_hi[0] = nes->mmc2_chr_hi[1] = 0;
    for (int i = 0; i < 8; i++)
        nes->fme7_chr[i] = (u8)i;
    for (int i = 0; i < 4; i++)
        nes->fme7_prg[i] = 0;

    switch (nes->mapper) {
    case 1:
        /* Fix last bank at $C000 (mode 3). */
        nes->mmc1_ctrl = 0x0C;
        break;
    case 4: case 206: {
        int n8 = prg_8k_count(nes);
        nes->mmc3_regs[6] = 0;
        nes->mmc3_regs[7] = 1;
        if (n8 >= 2) {
            /* Keep fixed banks consistent with power-on. */
        }
        (void)n8;
        break;
    }
    case 9: case 10:
        nes->mmc2_chr_lo[0] = 0;
        nes->mmc2_chr_lo[1] = 0;
        nes->mmc2_chr_hi[0] = 0;
        nes->mmc2_chr_hi[1] = 0;
        break;
    case 69: {
        int n8 = prg_8k_count(nes);
        nes->fme7_prg[0] = 0;
        nes->fme7_prg[1] = 0;
        nes->fme7_prg[2] = 0;
        nes->fme7_prg[3] = (u8)((n8 > 1) ? (n8 - 2) : 0);
        nes->fme7_ram_mode = 0xC0;
        break;
    }
    case 185:
        /* CHR starts disabled until a valid enable write. */
        nes->chr_enable = 0;
        break;
    default:
        break;
    }
}

void mapper_cpu_write(struct NES *nes, u16 addr, u8 val) {
    /* $6000-$7FFF special cases before generic SRAM */
    if (addr >= 0x6000 && addr < 0x8000) {
        if (nes->mapper == 140) {
            nes->prg_bank = bank_mod((val >> 4) & 3, prg_32k_count(nes));
            nes->chr_bank = bank_mod(val & 0x0F, chr_8k_count(nes));
            return;
        }
        /* NINA-001 (mapper 34 variant): regs at $7FFD-$7FFF */
        if (nes->mapper == 34 && addr >= 0x7FFD) {
            if (addr == 0x7FFD)
                nes->prg_bank = bank_mod(val & 1, prg_32k_count(nes));
            else if (addr == 0x7FFE)
                nes->mmc1_chr0 = val & 0x0F; /* 4KB lo — reused storage */
            else
                nes->mmc1_chr1 = val & 0x0F; /* 4KB hi */
            return;
        }
        if (!prg_ram_writable(nes))
            return;
        nes->sram[addr - 0x6000] = val;
        if (nes->has_battery) nes->sram_dirty = 1;
        return;
    }

    /* NINA-03/06 (79) and mapper 113 register range $4100-$5FFF */
    if (addr >= 0x4100 && addr < 0x6000) {
        if (nes->mapper == 79) {
            nes->prg_bank = bank_mod((val >> 3) & 1, prg_32k_count(nes));
            nes->chr_bank = bank_mod(val & 7, chr_8k_count(nes));
            return;
        }
        if (nes->mapper == 113) {
            nes->prg_bank = bank_mod((val >> 3) & 7, prg_32k_count(nes));
            nes->chr_bank = bank_mod((val & 7) | ((val >> 3) & 8), chr_8k_count(nes));
            nes->mirror = (val & 0x80) ? 1 : 0; /* 0=H, 1=V */
            return;
        }
    }

    if (addr < 0x8000)
        return;

    switch (nes->mapper) {
    case 1: {
        /* Ignore consecutive-cycle writes (MMC1 shift register quirk). */
        if (nes->total_cycles >= 0
            && nes->mmc1_last_write_cycle >= 0
            && (nes->total_cycles - nes->mmc1_last_write_cycle) <= 1) {
            nes->mmc1_last_write_cycle = nes->total_cycles;
            break;
        }
        nes->mmc1_last_write_cycle = nes->total_cycles;
        if (val & 0x80) {
            nes->mmc1_shift = 0;
            nes->mmc1_count = 0;
            nes->mmc1_ctrl |= 0x0C;
        } else {
            nes->mmc1_shift |= (u8)((val & 1) << nes->mmc1_count);
            if (++nes->mmc1_count == 5) {
                int reg = (addr >> 13) & 3;
                switch (reg) {
                case 0:
                    nes->mmc1_ctrl = nes->mmc1_shift;
                    switch (nes->mmc1_ctrl & 3) {
                    case 0: nes->mirror = 2; break;
                    case 1: nes->mirror = 3; break;
                    case 2: nes->mirror = 1; break;
                    case 3: nes->mirror = 0; break;
                    }
                    break;
                case 1: nes->mmc1_chr0 = nes->mmc1_shift; break;
                case 2: nes->mmc1_chr1 = nes->mmc1_shift; break;
                case 3: nes->mmc1_prg = nes->mmc1_shift; break;
                }
                nes->mmc1_shift = 0;
                nes->mmc1_count = 0;
            }
        }
        break;
    }
    case 2:
        nes->prg_bank = bank_mod(val, prg_16k_count(nes));
        break;
    case 3:
        nes->chr_bank = bank_mod(val, chr_8k_count(nes));
        break;
    case 4:
        if (addr < 0xA000) {
            if (addr & 1) {
                int sel = nes->mmc3_select & 7;
                nes->mmc3_regs[sel] = val;
                /* R0/R1 ignore LSB (2KB CHR banks) */
                if (sel <= 1)
                    nes->mmc3_regs[sel] &= 0xFE;
            } else {
                nes->mmc3_select = val;
                nes->mmc3_prg_mode = (val >> 6) & 1;
                nes->mmc3_chr_mode = (val >> 7) & 1;
            }
        } else if (addr < 0xC000) {
            if (addr & 1) {
                /* $A001: PRG-RAM enable (bit7), write-protect (bit6) */
                nes->mmc3_wram_enable = (val >> 7) & 1;
                nes->mmc3_wram_protect = (val >> 6) & 1;
            } else if (nes->mirror != 4) {
                /* Four-screen boards ignore MMC3 mirror control */
                nes->mirror = (val & 1) ? 0 : 1;
            }
        } else if (addr < 0xE000) {
            if (addr & 1) {
                nes->mmc3_irq_count = 0;
                nes->mmc3_irq_reload = 1;
            } else {
                nes->mmc3_irq_latch = val;
            }
        } else {
            nes->mmc3_irq_enable = (addr & 1) ? 1 : 0;
            if (!(addr & 1))
                nes->irq_pending = 0;
        }
        break;
    case 7:
        nes->prg_bank = bank_mod(val & 0x0F, prg_32k_count(nes));
        nes->mirror = (val & 0x10) ? 3 : 2;
        break;
    case 11:
        nes->prg_bank = bank_mod(val & 3, prg_32k_count(nes));
        nes->chr_bank = bank_mod(val >> 4, chr_8k_count(nes));
        break;
    case 9: case 10:
        if (addr < 0xB000)
            nes->prg_bank = val & 0x0F;
        else if (addr < 0xC000)
            nes->mmc2_chr_lo[0] = val & 0x1F;
        else if (addr < 0xD000)
            nes->mmc2_chr_lo[1] = val & 0x1F;
        else if (addr < 0xE000)
            nes->mmc2_chr_hi[0] = val & 0x1F;
        else if (addr < 0xF000)
            nes->mmc2_chr_hi[1] = val & 0x1F;
        else
            nes->mirror = (val & 1) ? 0 : 1;
        break;
    case 13:
        /* CPROM: select 4KB CHR-RAM bank for $1000-$1FFF */
        nes->chr_bank = bank_mod(val & 3, chr_4k_count(nes));
        break;
    case 34:
        nes->prg_bank = bank_mod(val, prg_32k_count(nes));
        break;
    case 79:
        /* Also accept $8000+ writes (some dumps / bus mirrors) */
        nes->prg_bank = bank_mod((val >> 3) & 1, prg_32k_count(nes));
        nes->chr_bank = bank_mod(val & 7, chr_8k_count(nes));
        break;
    case 113:
        nes->prg_bank = bank_mod((val >> 3) & 7, prg_32k_count(nes));
        nes->chr_bank = bank_mod((val & 7) | ((val >> 3) & 8), chr_8k_count(nes));
        nes->mirror = (val & 0x80) ? 1 : 0;
        break;
    case 70:
        nes->prg_bank = bank_mod((val >> 4) & 7, prg_16k_count(nes));
        nes->chr_bank = bank_mod(val & 0x0F, chr_8k_count(nes));
        break;
    case 71:
        if (addr >= 0x9000 && addr < 0xA000)
            nes->mirror = (val & 0x10) ? 3 : 2;
        else if (addr >= 0xC000)
            nes->prg_bank = bank_mod(val & 0x0F, prg_16k_count(nes));
        break;
    case 78:
        nes->prg_bank = bank_mod(val & 7, prg_16k_count(nes));
        nes->chr_bank = bank_mod(val >> 4, chr_8k_count(nes));
        nes->mirror = (val & 8) ? 3 : 2;
        break;
    case 66:
        nes->prg_bank = bank_mod((val >> 4) & 3, prg_32k_count(nes));
        nes->chr_bank = bank_mod(val & 3, chr_8k_count(nes));
        break;
    case 69:
        if (addr < 0xA000) {
            nes->fme7_cmd = val & 0x0F;
        } else if (addr < 0xC000) {
            u8 cmd = nes->fme7_cmd;
            if (cmd < 8) {
                nes->fme7_chr[cmd] = val;
            } else if (cmd < 12) {
                if (cmd == 8) {
                    nes->fme7_ram_mode = val;
                    nes->fme7_prg[0] = val & 0x3F;
                } else {
                    nes->fme7_prg[cmd - 8] = val & 0x3F;
                }
            } else if (cmd == 12) {
                switch (val & 3) {
                case 0: nes->mirror = 1; break;
                case 1: nes->mirror = 0; break;
                case 2: nes->mirror = 2; break;
                case 3: nes->mirror = 3; break;
                }
            } else if (cmd == 13) {
                nes->fme7_irq_ctrl = val;
                if (!(val & 0x01))
                    nes->irq_pending = 0;
            } else if (cmd == 14) {
                nes->fme7_irq_counter =
                    (nes->fme7_irq_counter & 0xFF00) | val;
            } else if (cmd == 15) {
                nes->fme7_irq_counter =
                    (nes->fme7_irq_counter & 0x00FF) | ((u16)val << 8);
            }
        }
        break;
    case 87:
        nes->chr_bank = bank_mod(((val >> 1) & 1) | ((val << 1) & 2),
                                chr_8k_count(nes));
        break;
    case 93:
        nes->prg_bank = bank_mod((val >> 4) & 7, prg_16k_count(nes));
        break;
    case 94:
        nes->prg_bank = bank_mod((val >> 2) & 7, prg_16k_count(nes));
        break;
    case 152:
        nes->prg_bank = bank_mod((val >> 4) & 7, prg_16k_count(nes));
        nes->chr_bank = bank_mod(val & 0x0F, chr_8k_count(nes));
        nes->mirror = (val & 0x80) ? 3 : 2;
        break;
    case 180:
        nes->prg_bank = bank_mod(val, prg_16k_count(nes));
        break;
    case 185:
        /*
         * CNROM + copy-protection: CHR is enabled only when the written
         * nibble is non-zero (common heuristic used by most emulators).
         */
        nes->chr_bank = 0;
        nes->chr_enable = ((val & 0x0F) != 0) ? 1 : 0;
        break;
    case 206:
        if (addr < 0xA000) {
            if (addr & 1) {
                int sel = nes->mmc3_select & 7;
                u8 v = val;
                if (sel <= 1) v &= 0xFE;
                if (sel >= 6) v &= 0x0F;
                nes->mmc3_regs[sel] = v;
            } else {
                nes->mmc3_select = val & 7;
            }
        }
        break;
    default:
        break;
    }
}
