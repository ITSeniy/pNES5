#ifndef NES_STATE_H
#define NES_STATE_H

#include "core.h"

#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_B 0x10
#define F_U 0x20
#define F_V 0x40
#define F_N 0x80

#define SET_ZN(v) do { \
    nes->flags = (nes->flags & ~(F_Z|F_N)) \
               | ((v)==0 ? F_Z : 0) \
               | ((v)&0x80 ? F_N : 0); \
} while(0)

#define MAX_ROMS 4096
#define MAX_NAME 28

struct rom_entry {
    char filename[48];
    char display[MAX_NAME];
};

struct pulse_ch {
    u8  duty, halt, const_vol, vol_period;
    u8  sweep_en, sweep_neg, sweep_shift, sweep_period;
    u16 timer;
    u8  length, env_vol, env_counter, env_start;
    u16 timer_count;
    u8  duty_pos, sweep_reload, sweep_counter, enabled;
};

struct triangle_ch {
    u8  linear_load, control;
    u16 timer;
    u8  length, linear_counter, linear_reload;
    u16 timer_count;
    u8  step, enabled;
};

struct noise_ch {
    u8  halt, const_vol, vol_period, mode, period_idx;
    u8  length, env_vol, env_counter, env_start;
    u16 timer_count, shift_reg;
    u8  enabled;
};

struct dmc_ch {
    u8  irq_enable, loop, period_idx, enabled;
    u8  output_level, shift_reg, bits_left, silence;
    u8  sample_buf, sample_buf_full, irq_flag;
    u16 timer_count, sample_addr, sample_len;
    u16 cur_addr, bytes_left;
    /* Memory reader unit: pending DMA after sample buffer empties. */
    u8  dma_pending;
    u8  dma_reentry; /* guard re-entrancy while performing DMA get */
    u8  dma_is_load; /* 1 = load after $4015 (3 cyc), 0 = reload (4 cyc) */
    u8  dma_halt_delay; /* CPU reads to skip before load DMA may halt */
    u8  dma_abort;   /* next service is 1-cycle aborted DMA (explicit/implicit stop) */
    u8  dma_reload_race; /* load while reload pending → abort after load */
    u8  stream_defer; /* unused (reload DMA is ASAP); kept for savestate size */
    u8  bus_hold_ttl; /* keep DMA sample on data bus across code/RAM puts */
    u8  joy_oe_hold;  /* post-DMA completing read shares $4016/$4017 OE */
    u8  oam_dma_active; /* DMC during OAM uses shortened steal */
    u16 halt_addr;   /* CPU address delayed by RDY (dummy-read target) */
    /*
     * When DMC DMA pulls RDY ~2 cycles before an SH* write, hardware
     * stops ANDing with (H+1). Window is cycle-based (AccuracyCoin times
     * DMA tens of cycles before SH* via DMASync + clockslides).
     */
    u8  sh_h_suppress;
    s32 sh_suppress_until; /* total_cycles deadline; 0 = inactive */
    /* Ticks executed during the current cpu_step (for cycle padding). */
    s32 ticks_exec;
};

struct NES {
    u16 pc;
    u8  a, x, y, sp, flags;
    s32 cycles;
    s32 total_cycles;
    u8  nmi_pending, nmi_delay, prev_nmi_line, nmi_in_instr, nmi_instr_cycle;

    u8  ppu_ctrl, ppu_mask, ppu_status, oam_addr;
    u8  in_vblank;
    u8  odd_frame;
    u16 vram_addr, temp_addr;
    u8  fine_x, write_toggle, read_buf, ppu_open_bus;
    u8  ppu_open_bus_decay_low, ppu_open_bus_decay_high;
    /*
     * Opaque BG+spr0 overlap found while rendering this scanline (shift regs
     * fill even if only one of BG/sprites is shown). Committed to $2002.6 when
     * both enable bits are on — including mid-line $2001 writes (AccuracyCoin
     * Rendering Flag Behavior). Lives in NES (heap), never shellcode static.
     */
    u8  sp0_overlap;
    /*
     * Per-line BG opacity for spr0. Must be on the heap NES object: shellcode
     * is mapped RX (W^X), so file-scope statics fault; the exploit stack is
     * also too tight for 256-byte temps in render paths.
     */
    u8  bg_opaque[NES_W];
    /* Visible scanline index while its CPU runs, else -1. */
    s16 ppu_cur_scanline;
    /* v used for the current visible line (before inc_scroll_y). */
    u16 ppu_line_v;

    u8  ram[0x800];
    u8  sram[0x2000];
    u8  vram[0x1000];
    u8  palette[0x20];
    u8  oam[256];
    u8  *prg, *chr;
    s32 prg_size, chr_size;
    s32 chr_is_ram;
    s32 mirror;
    s32 mapper;
    s32 prg_bank, chr_bank;
    s32 prg_banks, chr_banks;
    u8  has_battery, sram_dirty;

    u8  mmc1_shift, mmc1_count;
    u8  mmc1_ctrl, mmc1_chr0, mmc1_chr1, mmc1_prg;
    s32 mmc1_last_write_cycle;

    u8  mmc3_select, mmc3_regs[8];
    u8  mmc3_irq_latch, mmc3_irq_count;
    u8  mmc3_irq_enable, mmc3_irq_reload;
    u8  mmc3_prg_mode, mmc3_chr_mode, mmc3_a12;
    u8  mmc3_wram_enable, mmc3_wram_protect;

    u8  mmc2_chr_lo[2], mmc2_chr_hi[2];
    u8  mmc2_latch0, mmc2_latch1;

    /* FME-7: prg[0]=$6000, prg[1]=$8000, prg[2]=$A000, prg[3]=$C000; $E000 fixed */
    u8  fme7_cmd, fme7_prg[4], fme7_chr[8];
    u8  fme7_irq_ctrl;
    u16 fme7_irq_counter;
    u8  fme7_ram_mode; /* cmd $8 raw: bit6=RAM, bit7=enable */

    /* Mapper 185: CHR open-bus when disabled */
    u8  chr_enable;

    /*
     * External CPU data bus (open-bus for $4000–$4014 / $4018+).
     * Updated on every R/W except $4015 *reads* (AccuracyCoin Internal Data Bus).
     * DMC sample gets land here so LDA $4000 open-bus DMASync sees them.
     */
    u8  cpu_data_bus;
    /*
     * Internal data bus: updated on every R/W including $4015 reads.
     * DMC DMA does *not* write this (only the external bus). $4015 bit 5 is
     * open bus from the *internal* latch (Internal Data Bus tests 2–3).
     */
    u8  cpu_db_internal;

    u8  pad_state, pad_shift, pad_strobe;
    /* Internal OUT0 ($4016 bit0); external pin updates only on put cycles. */
    u8  pad_out0;
    /* Contiguous joypad OE coalescing (NES/AV Famicom, not RF Famicom). */
    u16 joy_oe_addr;
    u8  joy_last_bit;
    u8  dma_read_pending, dma_read_suppress; /* legacy; unused */
    u16 dma_read_addr;
    u8  irq_pending, prev_irq_inhibit, apu_irq_pending;

    struct pulse_ch    pulse[2];
    struct triangle_ch tri;
    struct noise_ch    noise;
    struct dmc_ch      dmc;
    u8  apu_status, frame_mode, frame_irq_inhibit;
    u8  frame_irq_flag, frame_reset_delay, frame_reset_mode;
    /* AccuracyCoin: $4015 clears frame IRQ only on put→get (next get cycle). */
    u8  frame_irq_clear_pending;
    /*
     * Step-4 asserts $4015.6 for 3 CPU cycles (2 if inhibit). While >0 the
     * flag is forced and clear_pending cannot retire (tests E–H, I–L).
     */
    u8  frame_irq_set_timer;
    /*
     * Cycles to wait after $4015.6 rises before pulling the IRQ line low.
     * Flag is readable immediately; IRQ is ~2 CPU cycles later (AccuracyCoin
     * Frame Counter IRQ N/O: 29834/29833 after $4017, blargg min 29833).
     */
    u8  frame_irq_line_delay;
    u8  frame_irq_line_prev;
    s32 frame_counter, sample_acc;
    s32 fc_step[2][6];

    s16 audio_buf[2048 * 2];
    s32 audio_pos;
    void *gadget;
    void *audio_out_fn;
    s32  audio_handle;
    s16  lpf_prev;
    s32  hpf_in, hpf_out;

    u8  screen[NES_W * NES_H];
    s32 rom_loaded;
    u8  is_pal;
    s32 cpu_freq, num_scanlines;
    s32 frame_cycle_rem, frame_cpu_overrun;
};

struct ext_args {
    s64 status;
    s64 step;
    u32 frame_count;
    u32 _pad;
    s32 log_fd;
    s32 pad_fd;
    u8  log_addr[16];
    u64 dbg[8];
};

/* bus.c */
u8   ppu_read(struct NES *nes, u16 addr);
void ppu_write(struct NES *nes, u16 addr, u8 val);
u8   cpu_read(struct NES *nes, u16 addr);
void cpu_write(struct NES *nes, u16 addr, u8 val);
u8   cpu_read_nodma(struct NES *nes, u16 addr);
void cpu_dma_repeat_read(struct NES *nes);

/* apu.c — DMC memory reader / timer (may stall CPU / affect SH*) */
void dmc_service_dma(struct NES *nes);
void dmc_tick(struct NES *nes);
/* Put/get half-cycle side effects before a CPU bus access (frame IRQ clear). */
void apu_on_cpu_cycle_begin(struct NES *nes);

/* cpu.c */
void cpu_step(struct NES *nes);
u16  cpu_read16(struct NES *nes, u16 addr);

/* ppu.c — returns 1 if opaque BG+sprite0 overlap on this scanline */
int  render_scanline(struct NES *nes, int y);
void run_frame(struct NES *nes);
/* After PPUMASK write: mid-line partial→full sprite0 (Rendering Flag Behavior). */
void ppu_on_mask_write(struct NES *nes, u8 prev_mask);

/* apu.c */
void apu_write_reg(struct NES *nes, u16 addr, u8 val);
void apu_step(struct NES *nes, int cycles);
void apu_flush(struct NES *nes);
void apu_prime(struct NES *nes, int buffers);

/* ppu.c */
void draw_char(u8 *scr, int x, int y, char ch, u8 color);
void draw_str(u8 *scr, int x, int y, const char *s, u8 color);
void draw_str_limit(u8 *scr, int x, int y, const char *s, int max_chars, u8 color);
void draw_centered(u8 *scr, int y, const char *s, u8 color);
void draw_hline(u8 *scr, int y, int x1, int x2, u8 color);
void draw_vline(u8 *scr, int x, int y1, int y2, u8 color);
void draw_rect(u8 *scr, int x, int y, int w, int h, u8 color);
void draw_box(u8 *scr, int x, int y, int w, int h, u8 color);
int  str_len(const char *s);
int  is_rom_file(const char *name);
void extract_rom_name(const char *fn, char *out, int max);
void scale_to_framebuf(u32 *fb, const u8 *nes_screen, u8 ppu_mask, int scale_mode);

#endif
