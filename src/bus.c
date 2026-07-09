#include "nes.h"
#include "mapper.h"
#include "tables.h"

static u16 mirror_nt(struct NES *nes, u16 addr) {
    addr &= 0x0FFF;
    switch (nes->mirror) {
    case 0: return ((addr / 0x400) & ~1) * 0x200 + (addr & 0x3FF);
    case 1: return addr & 0x7FF;
    case 2: return addr & 0x3FF;
    case 3: return (addr & 0x3FF) + 0x400;
    case 4: return addr & 0xFFF;
    }
    return addr & 0x7FF;
}

static void vram_step(struct NES *nes) {
    if ((nes->ppu_mask & 0x18) && !nes->in_vblank) {
        if ((nes->vram_addr & 0x1F) == 31) {
            nes->vram_addr &= ~0x1F;
            nes->vram_addr ^= 0x0400;
        } else nes->vram_addr++;
        u16 v = nes->vram_addr;
        if ((v & 0x7000) != 0x7000) { v += 0x1000; }
        else {
            v &= ~0x7000;
            int cy = (v & 0x03E0) >> 5;
            if (cy == 29) { cy = 0; v ^= 0x0800; }
            else if (cy == 31) cy = 0;
            else cy++;
            v = (v & ~0x03E0) | (cy << 5);
        }
        nes->vram_addr = v;
    } else {
        nes->vram_addr += (nes->ppu_ctrl & 4) ? 32 : 1;
    }
}

static void ppu_bus_set(struct NES *nes, u8 val) {
    nes->ppu_open_bus = val;
    nes->ppu_open_bus_decay_low = 60;
    nes->ppu_open_bus_decay_high = 60;
}

static void ppu_status_bus_set(struct NES *nes, u8 val) {
    nes->ppu_open_bus = val;
    nes->ppu_open_bus_decay_high = 60;
}

static u8 cpu_bus_put(struct NES *nes, u8 val) {
    /*
     * After a DMC get the sample sits on the external data bus. Holding it
     * across *code/RAM* operand fetches (ADH of LDA $4000) lets open-bus see
     * $00 even if halt was one cycle early. Must NOT suppress puts from PPU /
     * $4015–$4017 — that broke DMA+$4016 (controller bits / OE sequencing).
     */
    if (nes->dmc.bus_hold_ttl) {
        nes->dmc.bus_hold_ttl--;
        return nes->cpu_data_bus;
    }
    nes->cpu_data_bus = val;
    return val;
}

/* True when this put is from a side-effecting I/O read that must update the bus. */
static int is_io_side_effect_addr(u16 addr) {
    if (addr >= 0x2000 && addr < 0x4000)
        return 1;
    if (addr == 0x4015 || addr == 0x4016 || addr == 0x4017)
        return 1;
    return 0;
}

/*
 * Side-effecting CPU read without DMC service / timer tick.
 * DMC halt/dummy/alignment cycles re-issue this path on the halted address.
 */
u8 cpu_read_nodma(struct NES *nes, u16 addr) {
    u8 result = 0;
    int skip_bus_update = 0;

    /* I/O side effects always win over post-DMA sample hold. */
    if (is_io_side_effect_addr(addr))
        nes->dmc.bus_hold_ttl = 0;

    if (addr < 0x2000) {
        result = nes->ram[addr & 0x7FF];
        nes->joy_oe_addr = 0;
    } else if (addr < 0x4000) {
        nes->joy_oe_addr = 0;
        switch (addr & 7) {
        case 0: case 1: case 3: case 5: case 6:
            result = nes->ppu_open_bus;
            break;
        case 2: {
            result = (nes->ppu_status & 0xE0) | (nes->ppu_open_bus & 0x1F);
            ppu_status_bus_set(nes, result);
            nes->ppu_status &= ~0x80;
            nes->nmi_pending = 0;
            nes->nmi_delay = 0;
            nes->prev_nmi_line = 0;
            nes->write_toggle = 0;
            break;
        }
        case 4: {
            u8 r = nes->oam[nes->oam_addr];
            if ((nes->oam_addr & 3) == 2)
                r &= 0xE3;
            ppu_bus_set(nes, r);
            result = nes->ppu_open_bus;
            break;
        }
        case 7: {
            /*
             * $2007: non-palette reads return the previous fill (1-byte buffer).
             * Palette ($3F00–$3FFF) returns immediately; the buffer is filled from
             * the nametable under that address (bit 12 clear → $2Fxx), AccuracyCoin
             * PPU Read Buffer test 7 / Palette RAM Quirks prerequisite.
             */
            {
                u16 a = nes->vram_addr & 0x3FFF;
                mapper_notify_a12(nes, a);
                if (a >= 0x3F00) {
                    u8 pal = ppu_read(nes, a);
                    if (nes->ppu_mask & 0x01)
                        pal &= 0x30; /* greyscale: force lower 4 bits clear */
                    result = (u8)((pal & 0x3F) | (nes->ppu_open_bus & 0xC0));
                    nes->read_buf = ppu_read(nes, (u16)(a & 0x2FFF));
                } else {
                    result = nes->read_buf;
                    nes->read_buf = ppu_read(nes, a);
                }
                ppu_bus_set(nes, result);
                vram_step(nes);
                mapper_notify_a12(nes, nes->vram_addr);
            }
            break;
        }
        }
    } else if (addr >= 0x6000 && addr < 0x8000) {
        nes->joy_oe_addr = 0;
        result = mapper_cpu_read(nes, addr);
    } else if (addr == 0x4015) {
        nes->joy_oe_addr = 0;
        result = 0;
        if (nes->pulse[0].length > 0) result |= 1;
        if (nes->pulse[1].length > 0) result |= 2;
        if (nes->tri.length > 0) result |= 4;
        if (nes->noise.length > 0) result |= 8;
        if (nes->dmc.bytes_left > 0) result |= 0x10;
        if (nes->frame_irq_flag) result |= 0x40;
        if (nes->dmc.irq_flag) result |= 0x80;
        /*
         * Frame IRQ (bit 6) is not cleared on this cycle if it is a put cycle;
         * it clears on the next put→get (AccuracyCoin Frame Counter IRQ 6–7).
         * Reading still samples the current flag value above.
         */
        nes->frame_irq_clear_pending = 1;
        nes->apu_irq_pending = (nes->frame_irq_flag || nes->dmc.irq_flag) ? 1 : 0;
        /* Internal $4015 does not drive the external data bus. */
        result = (result & (u8)~0x20) | (nes->cpu_data_bus & 0x20);
        skip_bus_update = 1;
    } else if (addr == 0x4016) {
        /*
         * During DMC halt/dummy cycles (dma_reentry), re-reads of $4016 form
         * one contiguous OE: NES/AV Famicom clock once; subsequent dummies
         * return the same bit (AccuracyCoin DMA+$4016). The CPU's completing
         * data cycle after DMA is still that same OE (joy_oe_hold) — an extra
         * clock here broke DMA+$4016 (shift advanced twice per LDA).
         * Outside DMA, every access clocks (LDA $4016 separated by opcode fetches).
         */
        u8 bit;
        if (nes->pad_strobe)
            nes->pad_shift = nes->pad_state;
        if ((nes->dmc.dma_reentry || nes->dmc.joy_oe_hold) && nes->joy_oe_addr == addr) {
            bit = nes->joy_last_bit;
            nes->dmc.joy_oe_hold = 0;
        } else {
            bit = (u8)(nes->pad_shift & 1);
            if (!nes->pad_strobe)
                nes->pad_shift = (u8)((nes->pad_shift >> 1) | 0x80);
            nes->joy_last_bit = bit;
            nes->joy_oe_addr = addr;
        }
        result = (u8)(bit | (nes->cpu_data_bus & 0xFE));
    } else if (addr == 0x4017) {
        if (nes->dmc.joy_oe_hold && nes->joy_oe_addr == addr)
            nes->dmc.joy_oe_hold = 0;
        nes->joy_oe_addr = addr;
        result = (u8)(nes->cpu_data_bus & 0xFE);
    } else if (addr >= 0x8000) {
        nes->joy_oe_addr = 0;
        result = mapper_prg_read(nes, addr);
    } else {
        /* $4000–$4014, $4018–$5FFF open bus */
        nes->joy_oe_addr = 0;
        result = nes->cpu_data_bus;
        skip_bus_update = 1;
    }

    if (!skip_bus_update)
        cpu_bus_put(nes, result);
    return result;
}

/* Legacy no-op — dummies go through cpu_read_nodma now. */
void cpu_dma_repeat_read(struct NES *nes) {
    (void)nes;
}

u8 cpu_read(struct NES *nes, u16 addr) {
    /*
     * DMC DMA may only halt on a read cycle (RDY).
     * Load: dma_halt_delay then halt ASAP (AccuracyCoin DMA+$2002).
     * Reload / abort: halt on the next read (hardware RDY).
     *
     * Stream-defer was tried (prefer ABS data for $2007 dummies) but any
     * budget that bridges BNE+LDA also lets an early pending get land in a
     * Clockslide / epilogue LDA zp, breaking DMASync residual (open bus).
     * bus_hold_ttl covers open-bus when halt is 1–2 cycles early on ADL/ADH.
     * Side-effect ports ($2007/$4015/$4016) still need the get on the data
     * address — that requires residual phase, not stream defer.
     */
    apu_on_cpu_cycle_begin(nes);

    if (nes->dmc.dma_pending && !nes->dmc.dma_reentry) {
        if (nes->dmc.dma_halt_delay > 0 && !nes->dmc.dma_abort) {
            nes->dmc.dma_halt_delay--;
        } else {
            nes->dmc.stream_defer = 0;
            nes->dmc.halt_addr = addr;
            dmc_service_dma(nes);
        }
    }

    u8 result = cpu_read_nodma(nes, addr);
    dmc_tick(nes);
    return result;
}

u8 ppu_read(struct NES *nes, u16 addr) {
    addr &= 0x3FFF;

    if (addr < 0x2000)
        return mapper_chr_read(nes, addr);

    if (addr < 0x3F00)
        return nes->vram[mirror_nt(nes, addr - 0x2000)];

    u8 idx = addr & 0x1F;
    if ((idx & 0x13) == 0x10) idx &= 0x0F;
    return nes->palette[idx];
}

void ppu_write(struct NES *nes, u16 addr, u8 val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        mapper_chr_write(nes, addr, val);
    } else if (addr < 0x3F00) {
        nes->vram[mirror_nt(nes, addr - 0x2000)] = val;
    } else {
        u8 idx = addr & 0x1F;
        if ((idx & 0x13) == 0x10) idx &= 0x0F;
        /* Palette entries are 6-bit; greyscale does not alter write path
         * (AccuracyCoin Palette RAM Quirks tests 5–7: mask on read only). */
        nes->palette[idx] = val & 0x3F;
    }
}

void cpu_write(struct NES *nes, u16 addr, u8 val) {
    /*
     * DMA cannot halt on a write cycle — never service dma_pending here.
     * Pending gets wait until the next CPU read.
     */
    apu_on_cpu_cycle_begin(nes);

    if (addr == 0x4010 || addr == 0x4015 || addr == 0x4017) {
        nes->cpu_data_bus = val;
        nes->joy_oe_addr = 0;
        apu_write_reg(nes, addr, val);
        /* Keep write data on the bus after any load-DMA side effects. */
        nes->cpu_data_bus = val;
        dmc_tick(nes);
        return;
    }

    if (addr == 0x4016) {
        /*
         * OUT0 latched before put-cycle apply in dmc_tick (Controller Strobing).
         */
        nes->cpu_data_bus = val;
        nes->joy_oe_addr = 0;
        nes->pad_out0 = val & 1;
        dmc_tick(nes);
        return;
    }

    nes->cpu_data_bus = val;
    nes->joy_oe_addr = 0;
    dmc_tick(nes);

    if (addr < 0x2000) { nes->ram[addr & 0x7FF] = val; return; }

    if (addr < 0x4000) {
        ppu_bus_set(nes, val);
        switch (addr & 7) {
        case 0: {
            u8 prev = nes->ppu_ctrl;
            nes->ppu_ctrl = val;
            nes->temp_addr = (nes->temp_addr & 0xF3FF) | ((val & 3) << 10);
            if (!(prev & 0x80) && (val & 0x80) && (nes->ppu_status & 0x80)) {
                nes->nmi_pending = 1;
                nes->nmi_delay = 1;
            }
            break;
        }
        case 1: {
            u8 prev_mask = nes->ppu_mask;
            nes->ppu_mask = val;
            ppu_on_mask_write(nes, prev_mask);
            break;
        }
        case 3: nes->oam_addr = val; break;
        case 4: nes->oam[nes->oam_addr++] = val; break;
        case 5:
            if (!nes->write_toggle) {
                nes->fine_x = val & 7;
                nes->temp_addr = (nes->temp_addr & 0xFFE0) | (val >> 3);
            } else {
                nes->temp_addr = (nes->temp_addr & 0x0C1F) | ((val & 7) << 12) | ((val >> 3) << 5);
            }
            nes->write_toggle ^= 1;
            break;
        case 6:
            if (!nes->write_toggle) {
                nes->temp_addr = (nes->temp_addr & 0x00FF) | ((val & 0x3F) << 8);
                mapper_notify_a12(nes, nes->temp_addr);
            } else {
                nes->temp_addr = (nes->temp_addr & 0xFF00) | val;
                nes->vram_addr = nes->temp_addr;
                mapper_notify_a12(nes, nes->vram_addr);
            }
            nes->write_toggle ^= 1;
            break;
        case 7:
            ppu_write(nes, nes->vram_addr, val);
            vram_step(nes);
            mapper_notify_a12(nes, nes->vram_addr);
            break;
        }
        return;
    }

    if (addr == 0x4014) {
        /*
         * OAM DMA: halt + optional align + 256 get/put pairs (513/514).
         * DMC may steal ~2 cycles mid-transfer (see dmc_service_dma).
         * Alignment uses the current bus-cycle parity so the next CPU opcode
         * starts on a get cycle (AccuracyCoin APU get/put sync).
         */
        u16 base = (u16)val << 8;
        int odd = (nes->total_cycles + nes->dmc.ticks_exec) & 1;
        nes->dmc.oam_dma_active = 1;
        nes->cycles += 1;
        dmc_tick(nes);
        if (odd) {
            nes->cycles += 1;
            dmc_tick(nes);
        }
        for (int i = 0; i < 256; i++) {
            if (nes->dmc.dma_pending && !nes->dmc.dma_reentry
                && nes->dmc.dma_halt_delay == 0) {
                nes->dmc.halt_addr = 0xFFFF;
                dmc_service_dma(nes);
            }
            apu_on_cpu_cycle_begin(nes);
            u8 b = cpu_read_nodma(nes, (u16)(base + i));
            nes->oam[(nes->oam_addr + i) & 0xFF] = b;
            nes->cycles += 2;
            dmc_tick(nes);
            dmc_tick(nes);
            /* Count down load delay across OAM get cycles */
            if (nes->dmc.dma_halt_delay > 0)
                nes->dmc.dma_halt_delay--;
        }
        nes->dmc.oam_dma_active = 0;
        return;
    }

    if (addr >= 0x4000 && addr <= 0x4013) {
        apu_write_reg(nes, addr, val);
        return;
    }

    if (addr >= 0x4100)
        mapper_cpu_write(nes, addr, val);
}
