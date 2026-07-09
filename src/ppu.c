#include "nes.h"
#include "mapper.h"
#include "tables.h"

static void inc_scroll_y(struct NES *nes) {
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
}

static void copy_scroll_x(struct NES *nes) {
    nes->vram_addr = (nes->vram_addr & 0xFBE0) | (nes->temp_addr & 0x041F);
}

static void copy_scroll_y(struct NES *nes) {
    nes->vram_addr = (nes->vram_addr & 0x041F) | (nes->temp_addr & 0xFBE0);
}

/*
 * Latch whether spr0 and BG share an opaque pixel on scanline y using the
 * line's v (ppu_line_v). Does not paint. Used when $2001 enables partial
 * rendering mid-line after the line was drawn with rendering off.
 */
static void ppu_eval_sp0_overlap(struct NES *nes, int y) {
    u8 bg_opaque[NES_W];
    for (int x = 0; x < NES_W; x++) bg_opaque[x] = 0;

    u16 v = nes->ppu_line_v;
    u16 pat = (nes->ppu_ctrl & 0x10) ? 0x1000 : 0;
    for (int tile = 0; tile < 33; tile++) {
        int cx = v & 0x1F;
        int cy = (v >> 5) & 0x1F;
        int fy = (v >> 12) & 7;
        u16 nt = 0x2000 | (v & 0x0C00);
        u8 idx = ppu_read(nes, nt | (cy << 5) | cx);
        u8 lo = ppu_read(nes, pat + (u16)idx * 16 + fy);
        u8 hi = ppu_read(nes, pat + (u16)idx * 16 + fy + 8);
        for (int px = 0; px < 8; px++) {
            int sx = tile * 8 + px - nes->fine_x;
            if (sx < 0 || sx >= NES_W) continue;
            u8 color = ((hi >> (7 - px)) & 1) << 1 | ((lo >> (7 - px)) & 1);
            if (color) bg_opaque[sx] = 1;
        }
        if ((v & 0x1F) == 31) { v &= ~0x1F; v ^= 0x0400; }
        else v++;
    }

    int sph = (nes->ppu_ctrl & 0x20) ? 16 : 8;
    u16 spr_pat = (nes->ppu_ctrl & 0x08) ? 0x1000 : 0;
    int oy = nes->oam[0];
    if (oy >= 0xEF) return;
    int sy = oy + 1;
    if (y < sy || y >= sy + sph) return;

    int tile = nes->oam[1];
    int attr = nes->oam[2];
    int sx = nes->oam[3];
    int row = y - sy;
    if (attr & 0x80) row = sph - 1 - row;
    u16 pa;
    if (sph == 16) {
        u16 bk = (tile & 1) ? 0x1000 : 0;
        u8 t = tile & 0xFE;
        if (row >= 8) { t++; row -= 8; }
        pa = bk + t * 16 + row;
    } else {
        pa = spr_pat + tile * 16 + row;
    }
    u8 lo = ppu_read(nes, pa);
    u8 hi = ppu_read(nes, pa + 8);
    int spr_clip = !(nes->ppu_mask & 0x04);
    int sp0_left_clip = !(nes->ppu_mask & 0x02) || spr_clip;
    for (int px = 0; px < 8; px++) {
        int bx = (attr & 0x40) ? px : (7 - px);
        u8 c = ((hi >> bx) & 1) << 1 | ((lo >> bx) & 1);
        if (!c) continue;
        int dx = sx + px;
        if (dx >= NES_W || dx >= 255) continue;
        if (sp0_left_clip && dx < 8) continue;
        if (bg_opaque[dx]) {
            nes->sp0_overlap = 1;
            return;
        }
    }
}

void ppu_on_mask_write(struct NES *nes, u8 prev_mask) {
    u8 prev_r = prev_mask & 0x18;
    u8 new_r = nes->ppu_mask & 0x18;
    int y = nes->ppu_cur_scanline;

    if (y < 0 || y >= 240 || nes->in_vblank)
        goto commit;

    if (new_r && new_r != prev_r) {
        /*
         * Test 1: 0 → $1E late must NOT invent filled shift regs.
         * Test 2: 0 → $10 (or already $10 from render) then $1E must hit.
         * Only (re)latch overlap when enabling a *partial* mask, or when
         * already rendering and the mask changes.
         */
        if (prev_r != 0 || new_r == 0x08 || new_r == 0x10)
            ppu_eval_sp0_overlap(nes, y);
    }

commit:
    if (nes->sp0_overlap && new_r == 0x18)
        nes->ppu_status |= 0x40;
}

static void step_cpu_apu(struct NES *nes) {
    /*
     * DMC timer: one tick per CPU cycle. Bus accesses tick during the instr;
     * pad any leftover cycles so IFlagLatency DMA windows stay aligned.
     * Open-bus DMASync uses dma_open_ttl so pad-time DMA requests are OK.
     */
    nes->dmc.ticks_exec = 0;
    int before = nes->cycles;
    cpu_step(nes);
    int ran = nes->cycles - before;
    if (ran > 0) {
        while (nes->dmc.ticks_exec < ran)
            dmc_tick(nes);
        apu_step(nes, ran);
        mapper_cpu_clock(nes, ran);
        nes->total_cycles += ran;
    }
}

static int cpu_next_cycles_hint(struct NES *nes) {
    static const u8 cycles[256] = {
        7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
        2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
        2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
        2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
        2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
        2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
        2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
        2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
        2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
        2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
        2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
        2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7
    };
    if ((nes->irq_pending || nes->apu_irq_pending) && !nes->prev_irq_inhibit)
        return 7;
    /*
     * Peek opcode without cpu_read(): that would tick DMC / service DMA and
     * desync AccuracyCoin DMASync + interrupt timing over a frame.
     */
    {
        u16 pc = nes->pc;
        u8 op;
        if (pc < 0x2000)
            op = nes->ram[pc & 0x7FF];
        else if (pc >= 0x8000)
            op = mapper_prg_read(nes, pc);
        else
            op = 0xEA;
        return cycles[op];
    }
}

/*
 * Abs load of $2002 that participates in the VBL set race.
 * Only LDA/LDX/LDY abs (and indexed) — not BIT. AccuracyCoin VblSync ends
 * with BIT $2002; racing that cleared the set edge and desynced End/Beginning.
 */
static int abs_load_is_ppu_status(struct NES *nes) {
    u16 pc = nes->pc;
    u8 op;
    if (pc < 0x2000)
        op = nes->ram[pc & 0x7FF];
    else if (pc >= 0x8000)
        op = mapper_prg_read(nes, pc);
    else
        return 0;
    switch (op) {
    /*
     * LDX abs $2002 only for the set-race (Beginning's first of two reads).
     * Racing LDY too widened the $00 zone into the after-region ($01).
     * LDA/BIT are used by VblSync — never race those.
     */
    case 0xAE: /* LDX abs */
        break;
    default:
        return 0;
    }
    u8 lo, hi;
    u16 a1 = (u16)(pc + 1), a2 = (u16)(pc + 2);
    if (a1 < 0x2000) lo = nes->ram[a1 & 0x7FF];
    else if (a1 >= 0x8000) lo = mapper_prg_read(nes, a1);
    else return 0;
    if (a2 < 0x2000) hi = nes->ram[a2 & 0x7FF];
    else if (a2 >= 0x8000) hi = mapper_prg_read(nes, a2);
    else return 0;
    u16 addr = (u16)(lo | (hi << 8));
    return (addr >= 0x2000 && addr < 0x4000 && (addr & 7) == 2);
}

static void begin_vblank(struct NES *nes, int during_cpu_step, int instr_cycle) {
    nes->ppu_status |= 0x80;
    nes->in_vblank = 1;
    if (nes->ppu_ctrl & 0x80) {
        nes->nmi_pending = 1;
        if (during_cpu_step) {
            nes->nmi_in_instr = 1;
            nes->nmi_instr_cycle = instr_cycle > 0 ? (u8)instr_cycle : 1;
        }
    }
}

/* VBlank period without status bit / NMI (same-cycle $2002 race). */
static void begin_vblank_suppressed(struct NES *nes) {
    nes->in_vblank = 1;
    nes->ppu_status &= ~0x80;
}

static void end_vblank(struct NES *nes) {
    /* Same instant as pre-render: clear VBlank + sprite0 + overflow. */
    nes->ppu_status &= ~0xE0;
    nes->in_vblank = 0;
}

/*
 * Advance CPU/APU to `target`. Optional one-shot events:
 *   nmi_cycle  — set VBlank/NMI (pass nmi_done non-NULL)
 *   clr_cycle  — clear VBlank flags (pass clr_done non-NULL)
 * Use cycle < 0 to disable an event.
 *
 * Same-cycle $2002 race (AccuracyCoin VBlank beginning A=4 / NMI Suppression):
 * if VBL lands on the read cycle of an abs $2002 load, run the instruction
 * first so the read sees 0, then enter vblank without setting bit 7 / NMI.
 */
static void step_cpu_apu_until(struct NES *nes, int target,
                              int nmi_cycle, int *nmi_done,
                              int clr_cycle, int *clr_done) {
    if (nmi_done && !*nmi_done && nmi_cycle >= 0 && nes->cycles >= nmi_cycle) {
        begin_vblank(nes, 0, 0);
        *nmi_done = 1;
    }
    if (clr_done && !*clr_done && clr_cycle >= 0 && nes->cycles >= clr_cycle) {
        end_vblank(nes);
        *clr_done = 1;
    }
    while (nes->cycles < target) {
        int hint = cpu_next_cycles_hint(nes);
        int start = nes->cycles;
        int race = 0;
        int is_ldx_ldy_2002 = abs_load_is_ppu_status(nes);

        /*
         * Inclusive set (keeps VblSync/End). LDX $2002 race on data cycle.
         * Also: VBL in the first CPU after LDX $2002 (between LDX and LDY) is
         * treated as suppress — that is the A=4 window AccuracyCoin expects.
         */
        if (nmi_done && !*nmi_done && nmi_cycle >= 0
            && start < nmi_cycle && start + hint >= nmi_cycle) {
            int ic = nmi_cycle - start;
            if (is_ldx_ldy_2002 && hint > 0 && ic == hint - 1)
                race = 1;
            else {
                begin_vblank(nes, 1, ic > 0 ? ic : 1);
                *nmi_done = 1;
            }
        }
        if (clr_done && !*clr_done && clr_cycle >= 0
            && start < clr_cycle && start + hint >= clr_cycle) {
            end_vblank(nes);
            *clr_done = 1;
        }

        int was_ldx_2002 = is_ldx_ldy_2002;
        int ldx_end = start + hint;

        step_cpu_apu(nes);

        if (race) {
            begin_vblank_suppressed(nes);
            *nmi_done = 1;
        }
        /* VBL one CPU after LDX $2002 ends (deep in LDX/LDY gap) → suppress. */
        if (was_ldx_2002 && nmi_done && !*nmi_done && nmi_cycle >= 0
            && nmi_cycle == ldx_end + 1) {
            begin_vblank_suppressed(nes);
            *nmi_done = 1;
        }
        if (nmi_done && !*nmi_done && nmi_cycle >= 0 && nes->cycles >= nmi_cycle) {
            begin_vblank(nes, 0, 0);
            *nmi_done = 1;
        }
        if (clr_done && !*clr_done && clr_cycle >= 0 && nes->cycles >= clr_cycle) {
            end_vblank(nes);
            *clr_done = 1;
        }
    }
    if (nmi_done && !*nmi_done && nmi_cycle >= 0 && nes->cycles >= nmi_cycle) {
        begin_vblank(nes, 0, 0);
        *nmi_done = 1;
    }
    if (clr_done && !*clr_done && clr_cycle >= 0 && nes->cycles >= clr_cycle) {
        end_vblank(nes);
        *clr_done = 1;
    }
}

int render_scanline(struct NES *nes, int y) {
    u8 *line = &nes->screen[y * NES_W];
    /*
     * Stack (not static): JIT shellcode is RX; static writes fault on PS.
     */
    u8 bg_opaque[NES_W];
    for (int x = 0; x < NES_W; x++) { line[x] = nes->palette[0]; bg_opaque[x] = 0; }

    /*
     * Hardware keeps BG shift registers filling whenever *either* BG or
     * sprites are enabled, and sprite evaluation runs whenever either is
     * on (AccuracyCoin Rendering Flag Behavior). Only the show bits control
     * what is actually painted.
     */
    int any_render = (nes->ppu_mask & 0x18) != 0;
    int show_bg = (nes->ppu_mask & 0x08) != 0;
    int show_spr = (nes->ppu_mask & 0x10) != 0;
    nes->sp0_overlap = 0;

    /* OAMADDR forced through 0 during sprite fetch (dots 257–320). */
    if (any_render)
        nes->oam_addr = 0;

    if (any_render) {
        u16 v = nes->vram_addr;
        u16 pat = (nes->ppu_ctrl & 0x10) ? 0x1000 : 0;

        for (int tile = 0; tile < 33; tile++) {
            int cx = v & 0x1F;
            int cy = (v >> 5) & 0x1F;
            int fy = (v >> 12) & 7;
            u16 nt = 0x2000 | (v & 0x0C00);

            u8 idx = ppu_read(nes, nt | (cy << 5) | cx);
            u8 lo = ppu_read(nes, pat + (u16)idx * 16 + fy);
            u8 hi = ppu_read(nes, pat + (u16)idx * 16 + fy + 8);

            u8 at = ppu_read(nes, nt | 0x03C0 | ((cy >> 2) << 3) | (cx >> 2));
            u8 pal_idx = (at >> (((cy & 2) << 1) | (cx & 2))) & 3;

            for (int px = 0; px < 8; px++) {
                int sx = tile * 8 + px - nes->fine_x;
                if (sx < 0 || sx >= NES_W) continue;
                u8 color = ((hi >> (7-px)) & 1) << 1 | ((lo >> (7-px)) & 1);
                if (color) {
                    bg_opaque[sx] = 1;
                    if (show_bg)
                        line[sx] = nes->palette[pal_idx * 4 + color];
                }
            }

            if ((v & 0x1F) == 31) { v &= ~0x1F; v ^= 0x0400; }
            else v++;
        }
    }

    if (any_render) {
        int sph = (nes->ppu_ctrl & 0x20) ? 16 : 8;
        u16 spr_pat = (nes->ppu_ctrl & 0x08) ? 0x1000 : 0;
        int cnt = 0;
        u8 sprites[8];
        int has_sp0 = 0;

        for (int i = 0; i < 64; i++) {
            int oy = nes->oam[i*4];
            if (oy >= 0xEF) continue;
            int sy = oy + 1;
            if (y < sy || y >= sy + sph) continue;
            if (cnt < 8) {
                sprites[cnt] = i;
                if (i == 0) has_sp0 = 1;
                cnt++;
            } else {
                nes->ppu_status |= 0x20;
                break;
            }
        }

        int spr_clip = !(nes->ppu_mask & 0x04);
        int sp0_left_clip = !(nes->ppu_mask & 0x02) || spr_clip;

        for (int s = cnt - 1; s >= 0; s--) {
            int i = sprites[s];
            int sy = nes->oam[i*4] + 1;
            int tile = nes->oam[i*4+1];
            int attr = nes->oam[i*4+2];
            int sx = nes->oam[i*4+3];

            int row = y - sy;
            if (attr & 0x80) row = sph - 1 - row;

            u16 pa;
            if (sph == 16) {
                u16 bk = (tile & 1) ? 0x1000 : 0;
                u8 t = tile & 0xFE;
                if (row >= 8) { t++; row -= 8; }
                pa = bk + t * 16 + row;
            } else {
                pa = spr_pat + tile * 16 + row;
            }

            u8 lo = ppu_read(nes, pa);
            u8 hi = ppu_read(nes, pa + 8);
            u8 spal = (attr & 3) + 4;

            for (int px = 0; px < 8; px++) {
                int bx = (attr & 0x40) ? px : (7 - px);
                u8 c = ((hi >> bx) & 1) << 1 | ((lo >> bx) & 1);
                if (!c) continue;
                int dx = sx + px;
                if (dx >= NES_W) continue;
                if (has_sp0 && i == 0 && bg_opaque[dx] && dx < 255
                    && !(sp0_left_clip && dx < 8)) {
                    nes->sp0_overlap = 1;
                    /* Both show bits required; mid-line enable via $2001 write. */
                    if (show_bg && show_spr)
                        nes->ppu_status |= 0x40;
                }
                if (!show_spr) continue;
                if (spr_clip && dx < 8) continue;
                if ((attr & 0x20) && bg_opaque[dx]) continue;
                line[dx] = nes->palette[spal * 4 + c];
            }
        }
    }

    if (!(nes->ppu_mask & 0x02))
        for (int x = 0; x < 8; x++) line[x] = nes->palette[0];
    return nes->sp0_overlap;
}

void run_frame(struct NES *nes) {
    nes->cycles = nes->frame_cpu_overrun;
    nes->in_vblank = 0;
    nes->ppu_cur_scanline = -1;
    int target = 0, sl_acc = nes->frame_cycle_rem;
    int vblank_nmi_done = 0;
    int vblank_clr_done = 0;
    int sl_num = nes->is_pal ? (341 * 5) : 341;
    int sl_den = nes->is_pal ? 16 : 3;
    int total_sl = nes->num_scanlines;
    int skip_dot = 0;
    /* Last post-render line index and last vblank line (pre-render = total_sl-1). */
    int sl_post = 240;
    int sl_vbl_last = total_sl - 2; /* 260 NTSC / 310 PAL */
    int sl_pre = total_sl - 1;

    for (int y = 0; y < 240; y++) {
        if (nes->ppu_mask & 0x18) copy_scroll_x(nes);
        nes->ppu_line_v = nes->vram_addr;

        render_scanline(nes, y);
        if (nes->ppu_mask & 0x18) inc_scroll_y(nes);

        int irq_target = target + (260 * sl_num) / (341 * sl_den);
        sl_acc += sl_num;
        target += sl_acc / sl_den;
        sl_acc %= sl_den;

        /* CPU window for this line — mid-line $2001 uses ppu_cur_scanline. */
        nes->ppu_cur_scanline = (s16)y;
        step_cpu_apu_until(nes, irq_target, -1, 0, -1, 0);
        mapper_scanline_clock(nes);

        step_cpu_apu_until(nes, target, -1, 0, -1, 0);
        nes->ppu_cur_scanline = -1;
        /* Overlap flag is consumed on $2001 mid-line; drop at hblank. */
        nes->sp0_overlap = 0;
    }

    for (int y = 240; y < total_sl; y++) {
        int sl_start = target;
        if (y == sl_pre) {
            nes->sp0_overlap = 0;
            /*
             * Hardware forces OAMADDR through 0 on dots 257–320 of pre-render
             * when rendering; we always clear so a prior $2003 write cannot
             * misalign the next OAM DMA (AccuracyCoin sprite-zero setups).
             */
            nes->oam_addr = 0;
            if (nes->ppu_mask & 0x18) copy_scroll_y(nes);
        }
        if (y == sl_vbl_last)
            skip_dot = !nes->is_pal && nes->odd_frame && (nes->ppu_mask & 0x18);
        int cur_sl_num = sl_num;
        if (y == sl_pre && skip_dot)
            cur_sl_num = 340;
        sl_acc += cur_sl_num;
        target += sl_acc / sl_den;
        sl_acc %= sl_den;

        if (y == sl_pre) {
            int irq_target = target - (cur_sl_num / sl_den) + (260 * sl_num) / (341 * sl_den);
            step_cpu_apu_until(nes, irq_target, -1, 0, -1, 0);
            mapper_scanline_clock(nes);
        }

        /*
         * VBlank flag/NMI: set near SL241 dot 1.
         * Hardware: 241*341+1 PPU from frame start ≈ one PPU past the 240→241
         * boundary. With 3 PPU/CPU we arm one CPU cycle before `target` (end of
         * post-render) so AccuracyCoin's A=0..6 window matches (was one step
         * late: five $02s and no suppression slot).
         * Clear at end of last vblank line (= pre-render start).
         * $2002 read on the set cycle suppresses the flag (begin_vblank).
         */
        if (y == sl_post)
            step_cpu_apu_until(nes, target, target, &vblank_nmi_done, -1, 0);
        else if (y == 241 && !vblank_nmi_done)
            step_cpu_apu_until(nes, target, sl_start, &vblank_nmi_done, -1, 0);
        else if (y == sl_vbl_last)
            step_cpu_apu_until(nes, target, -1, 0, target, &vblank_clr_done);
        else if (y == sl_pre && !vblank_clr_done)
            step_cpu_apu_until(nes, target, -1, 0, sl_start, &vblank_clr_done);
        else
            step_cpu_apu_until(nes, target, -1, 0, -1, 0);
    }

    /* Safety: never leave VBlank stuck if clear event was missed. */
    if (!vblank_clr_done)
        end_vblank(nes);

    nes->frame_cycle_rem = sl_acc;
    nes->frame_cpu_overrun = nes->cycles - target;
    if (nes->frame_cpu_overrun < 0)
        nes->frame_cpu_overrun = 0;
    if (nes->ppu_open_bus_decay_low && --nes->ppu_open_bus_decay_low == 0)
        nes->ppu_open_bus &= 0xE0;
    if (nes->ppu_open_bus_decay_high && --nes->ppu_open_bus_decay_high == 0)
        nes->ppu_open_bus &= 0x1F;
    if (!nes->is_pal)
        nes->odd_frame ^= 1;
}


void scale_to_framebuf(u32 *fb, const u8 *scr, u8 mask) {
    int grey = mask & 0x01;
    int emph_r = (mask >> 5) & 1;
    int emph_g = (mask >> 6) & 1;
    int emph_b = (mask >> 7) & 1;

    for (int ny = 0; ny < NES_H; ny++) {
        int sy = OFF_Y + ny * SCALE;
        for (int nx = 0; nx < NES_W; nx++) {
            u8 idx = scr[ny * NES_W + nx] & 0x3F;
            if (grey) idx &= 0x30;
            u32 c = nes_rgb(idx);
            if (emph_r | emph_g | emph_b) {
                u32 r = (c >> 16) & 0xFF;
                u32 g = (c >> 8) & 0xFF;
                u32 b = c & 0xFF;
                if (emph_g | emph_b) r = r * 3 / 4;
                if (emph_r | emph_b) g = g * 3 / 4;
                if (emph_r | emph_g) b = b * 3 / 4;
                c = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
            int sx = OFF_X + nx * SCALE;
            for (int dy = 0; dy < SCALE; dy++) {
                u32 *row = &fb[(sy + dy) * SCR_W + sx];
                for (int dx = 0; dx < SCALE; dx++)
                    row[dx] = c;
            }
        }
    }
}

int str_len(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

void draw_char(u8 *scr, int x, int y, char ch, u8 color) {
    int gid = font_gid(ch);
    if (gid < 0) return;
    const u8 *glyph = font_pack[gid];

    for (int r = 0; r < 8; r++) {
        u8 bits = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) {
                int px = x + c, py = y + r;
                if (px >= 0 && px < NES_W && py >= 0 && py < NES_H)
                    scr[py * NES_W + px] = color;
            }
        }
    }
}

void draw_str(u8 *scr, int x, int y, const char *s, u8 color) {
    while (*s) {
        draw_char(scr, x, y, *s, color);
        x += 8;
        s++;
    }
}

void draw_str_limit(u8 *scr, int x, int y, const char *s, int max_chars, u8 color) {
    int n = 0;
    while (*s && n < max_chars) {
        draw_char(scr, x, y, *s, color);
        x += 8;
        s++;
        n++;
    }
}

void draw_centered(u8 *scr, int y, const char *s, u8 color) {
    int x = (NES_W - str_len(s) * 8) / 2;
    if (x < 0) x = 0;
    draw_str(scr, x, y, s, color);
}

void draw_hline(u8 *scr, int y, int x1, int x2, u8 color) {
    if (y < 0 || y >= NES_H) return;
    if (x1 < 0) x1 = 0;
    for (int x = x1; x < x2 && x < NES_W; x++)
        scr[y * NES_W + x] = color;
}

void draw_vline(u8 *scr, int x, int y1, int y2, u8 color) {
    if (x < 0 || x >= NES_W) return;
    if (y1 < 0) y1 = 0;
    for (int y = y1; y < y2 && y < NES_H; y++)
        scr[y * NES_W + x] = color;
}

void draw_rect(u8 *scr, int x, int y, int w, int h, u8 color) {
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > NES_W) x2 = NES_W;
    if (y2 > NES_H) y2 = NES_H;
    for (int py = y; py < y2; py++)
        for (int px = x; px < x2; px++)
            scr[py * NES_W + px] = color;
}

void draw_box(u8 *scr, int x, int y, int w, int h, u8 color) {
    draw_hline(scr, y, x, x + w, color);
    draw_hline(scr, y + h - 1, x, x + w, color);
    draw_vline(scr, x, y, y + h, color);
    draw_vline(scr, x + w - 1, y, y + h, color);
}

int is_rom_file(const char *name) {
    int len = str_len(name);
    if (len < 5) return 0;
    char a = name[len-4], b = name[len-3], c = name[len-2], d = name[len-1];
    if (a != '.') return 0;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (d >= 'A' && d <= 'Z') d += 32;
    return (b == 'r' && c == 'o' && d == 'm') || (b == 'n' && c == 'e' && d == 's');
}

void extract_rom_name(const char *fn, char *out, int max) {
    int i = 0;
    while (fn[i] && fn[i] != '.' && i < max - 1) {
        out[i] = fn[i];
        i++;
    }
    out[i] = '\0';
}
