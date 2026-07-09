#include "nes.h"
#include "mapper.h"
#include "tables.h"
#include "ftp.h"

#define COL_BG       0x0F
#define COL_PANEL    0x01
#define COL_PANEL_2  0x02
#define COL_BRAND    0x27
#define COL_HEAD     0x30
#define COL_LINE     0x21
#define COL_SEL      0x2A
#define COL_NORM     0x30
#define COL_DIM      0x00
#define COL_CUR      0x27
#define COL_NUM      0x21
#define ROM_BUF_SIZE 0x400000
#define STATE_MAGIC 0x30545345u /* EST0 */
#define STATE_VERSION 9u
#define CMD_STATE_LOAD 0xFC
#define CMD_STATE_SAVE 0xFD
#define CMD_MENU 0xFE
#define CMD_EXIT 0xFF

static const char *RESP_204K = "HTTP/1.1 204\r\nConnection:keep-alive\r\nAccess-Control-Allow-Origin:*\r\n\r\n";
static const char *RESP_CORS = "HTTP/1.1 204\r\nAccess-Control-Allow-Origin:*\r\nAccess-Control-Allow-Methods:POST\r\nConnection:keep-alive\r\n\r\n";

static void init_ntsc(struct NES *nes);
static void init_pal(struct NES *nes);

static void udp_log(void *G, void *sendto, s32 fd, u8 *sa, const char *msg) {
    if (fd < 0 || !sendto) return;
    NC(G, sendto, (u64)fd, (u64)msg, (u64)str_len(msg), 0, (u64)sa, 16);
}

static void clear_fb(u32 *fb) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000;
}

static int next_fb(int active) {
    active++;
    return active >= FB_COUNT ? 0 : active;
}

static int draw_uint(u8 *scr, int x, int y, int value, u8 color) {
    char tmp[10];
    int n = 0;
    if (value <= 0) {
        draw_char(scr, x, y, '0', color);
        return x + 8;
    }
    while (value > 0 && n < 10) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (int i = n - 1; i >= 0; i--) {
        draw_char(scr, x, y, tmp[i], color);
        x += 8;
    }
    return x;
}

static void draw_menu_shell(u8 *scr, const char *title) {
    for (int i = 0; i < NES_W * NES_H; i++) scr[i] = COL_BG;
    draw_rect(scr, 0, 0, NES_W, 36, COL_PANEL);
    draw_rect(scr, 0, 204, NES_W, 36, COL_PANEL);
    draw_box(scr, 4, 4, NES_W - 8, NES_H - 8, COL_PANEL_2);
    draw_hline(scr, 35, 8, NES_W - 8, COL_LINE);
    draw_hline(scr, 204, 8, NES_W - 8, COL_LINE);
    draw_centered(scr, 8, "EMUC0RE NES", COL_BRAND);
    draw_centered(scr, 21, title, COL_HEAD);
}



static int read_full(void *G, void *kread, s32 fd, u8 *dst, s32 len) {
    s32 total = 0;
    while (total < len) {
        s32 got = (s32)NC(G, kread, (u64)fd, (u64)(dst + total), (u64)(len - total), 0, 0, 0);
        if (got <= 0) break;
        total += got;
    }
    return total == len;
}

static int write_full(void *G, void *kwrite, s32 fd, const u8 *src, s32 len) {
    s32 total = 0;
    while (total < len) {
        s32 wrote = (s32)NC(G, kwrite, (u64)fd, (u64)(src + total), (u64)(len - total), 0, 0, 0);
        if (wrote <= 0) break;
        total += wrote;
    }
    return total == len;
}

static void build_ext_path(const char *rom_path, char *out, int max, const char *ext) {
    int i = 0, dot = -1;
    while (rom_path[i] && i < max - 5) {
        out[i] = rom_path[i];
        if (rom_path[i] == '.') dot = i;
        i++;
    }
    if (dot < 0) dot = i;
    out[dot++] = '.';
    for (int e = 0; ext[e] && dot < max - 1; e++)
        out[dot++] = ext[e];
    out[dot] = 0;
}

static void load_sram(struct NES *nes, void *G, void *kopen, void *kread,
                      void *kclose, const char *save_path) {
    if (!kopen || !kread || !kclose) return;
    s32 fd = (s32)NC(G, kopen, (u64)save_path, 0, 0, 0, 0, 0);
    if (fd < 0) return;
    read_full(G, kread, fd, nes->sram, 0x2000);
    NC(G, kclose, (u64)fd, 0, 0, 0, 0, 0);
    nes->sram_dirty = 0;
}

struct nes_state_core {
    u32 magic, version, size;
    s32 prg_size, chr_size, chr_is_ram, mirror, mapper;
    s32 prg_bank, chr_bank, prg_banks, chr_banks;
    u8  has_battery, is_pal;

    u16 pc;
    u8  a, x, y, sp, flags;
    s32 cycles, total_cycles;
    u8  nmi_pending, nmi_delay, prev_nmi_line, nmi_in_instr, nmi_instr_cycle;

    u8  ppu_ctrl, ppu_mask, ppu_status, oam_addr;
    u8  in_vblank, odd_frame;
    u16 vram_addr, temp_addr;
    u8  fine_x, write_toggle, read_buf, ppu_open_bus;
    u8  ppu_open_bus_decay_low, ppu_open_bus_decay_high;

    u8  ram[0x800];
    u8  sram[0x2000];
    u8  vram[0x1000];
    u8  palette[0x20];
    u8  oam[256];
    u8  chr_ram[0x2000];

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
    u8  fme7_cmd, fme7_prg[4], fme7_chr[8];
    u8  fme7_irq_ctrl;
    u16 fme7_irq_counter;
    u8  fme7_ram_mode;
    u8  chr_enable;

    u8  cpu_data_bus;
    u8  pad_state, pad_shift, pad_strobe, pad_out0;
    u8  irq_pending, prev_irq_inhibit, apu_irq_pending;
    struct pulse_ch    pulse[2];
    struct triangle_ch tri;
    struct noise_ch    noise;
    struct dmc_ch      dmc;
    u8  apu_status, frame_mode, frame_irq_inhibit;
    u8  frame_irq_flag, frame_reset_delay, frame_reset_mode;
    u8  frame_irq_clear_pending;
    s32 frame_counter, sample_acc;
    s16 lpf_prev;
    s32 hpf_in, hpf_out;
    s32 cpu_freq, num_scanlines;
    s32 frame_cycle_rem, frame_cpu_overrun;
};

static void capture_state(struct NES *nes, struct nes_state_core *st) {
    u8 *p = (u8 *)st;
    for (u32 i = 0; i < sizeof(*st); i++) p[i] = 0;

    st->magic = STATE_MAGIC;
    st->version = STATE_VERSION;
    st->size = sizeof(*st);
    st->prg_size = nes->prg_size; st->chr_size = nes->chr_size;
    st->chr_is_ram = nes->chr_is_ram; st->mirror = nes->mirror; st->mapper = nes->mapper;
    st->prg_bank = nes->prg_bank; st->chr_bank = nes->chr_bank;
    st->prg_banks = nes->prg_banks; st->chr_banks = nes->chr_banks;
    st->has_battery = nes->has_battery; st->is_pal = nes->is_pal;

    st->pc = nes->pc; st->a = nes->a; st->x = nes->x; st->y = nes->y;
    st->sp = nes->sp; st->flags = nes->flags;
    st->cycles = nes->cycles; st->total_cycles = nes->total_cycles;
    st->nmi_pending = nes->nmi_pending; st->nmi_delay = nes->nmi_delay;
    st->prev_nmi_line = nes->prev_nmi_line; st->nmi_in_instr = nes->nmi_in_instr;
    st->nmi_instr_cycle = nes->nmi_instr_cycle;

    st->ppu_ctrl = nes->ppu_ctrl; st->ppu_mask = nes->ppu_mask;
    st->ppu_status = nes->ppu_status; st->oam_addr = nes->oam_addr;
    st->in_vblank = nes->in_vblank; st->odd_frame = nes->odd_frame;
    st->vram_addr = nes->vram_addr; st->temp_addr = nes->temp_addr;
    st->fine_x = nes->fine_x; st->write_toggle = nes->write_toggle;
    st->read_buf = nes->read_buf; st->ppu_open_bus = nes->ppu_open_bus;
    st->ppu_open_bus_decay_low = nes->ppu_open_bus_decay_low;
    st->ppu_open_bus_decay_high = nes->ppu_open_bus_decay_high;

    for (int i = 0; i < 0x800; i++) st->ram[i] = nes->ram[i];
    for (int i = 0; i < 0x2000; i++) st->sram[i] = nes->sram[i];
    for (int i = 0; i < 0x1000; i++) st->vram[i] = nes->vram[i];
    for (int i = 0; i < 0x20; i++) st->palette[i] = nes->palette[i];
    for (int i = 0; i < 256; i++) st->oam[i] = nes->oam[i];
    if (nes->chr_is_ram && nes->chr)
        for (int i = 0; i < 0x2000; i++) st->chr_ram[i] = nes->chr[i];

    st->mmc1_shift = nes->mmc1_shift; st->mmc1_count = nes->mmc1_count;
    st->mmc1_ctrl = nes->mmc1_ctrl; st->mmc1_chr0 = nes->mmc1_chr0;
    st->mmc1_chr1 = nes->mmc1_chr1; st->mmc1_prg = nes->mmc1_prg;
    st->mmc1_last_write_cycle = nes->mmc1_last_write_cycle;
    st->mmc3_select = nes->mmc3_select;
    for (int i = 0; i < 8; i++) st->mmc3_regs[i] = nes->mmc3_regs[i];
    st->mmc3_irq_latch = nes->mmc3_irq_latch; st->mmc3_irq_count = nes->mmc3_irq_count;
    st->mmc3_irq_enable = nes->mmc3_irq_enable; st->mmc3_irq_reload = nes->mmc3_irq_reload;
    st->mmc3_prg_mode = nes->mmc3_prg_mode; st->mmc3_chr_mode = nes->mmc3_chr_mode;
    st->mmc3_a12 = nes->mmc3_a12;
    st->mmc3_wram_enable = nes->mmc3_wram_enable;
    st->mmc3_wram_protect = nes->mmc3_wram_protect;
    for (int i = 0; i < 2; i++) {
        st->mmc2_chr_lo[i] = nes->mmc2_chr_lo[i];
        st->mmc2_chr_hi[i] = nes->mmc2_chr_hi[i];
    }
    st->mmc2_latch0 = nes->mmc2_latch0; st->mmc2_latch1 = nes->mmc2_latch1;
    st->fme7_cmd = nes->fme7_cmd;
    for (int i = 0; i < 4; i++) st->fme7_prg[i] = nes->fme7_prg[i];
    for (int i = 0; i < 8; i++) st->fme7_chr[i] = nes->fme7_chr[i];
    st->fme7_irq_ctrl = nes->fme7_irq_ctrl;
    st->fme7_irq_counter = nes->fme7_irq_counter;
    st->fme7_ram_mode = nes->fme7_ram_mode;
    st->chr_enable = nes->chr_enable;

    st->cpu_data_bus = nes->cpu_data_bus;
    st->pad_state = nes->pad_state; st->pad_shift = nes->pad_shift;
    st->pad_strobe = nes->pad_strobe; st->pad_out0 = nes->pad_out0;
    st->irq_pending = nes->irq_pending;
    st->prev_irq_inhibit = nes->prev_irq_inhibit; st->apu_irq_pending = nes->apu_irq_pending;
    st->pulse[0] = nes->pulse[0]; st->pulse[1] = nes->pulse[1];
    st->tri = nes->tri; st->noise = nes->noise; st->dmc = nes->dmc;
    st->apu_status = nes->apu_status; st->frame_mode = nes->frame_mode;
    st->frame_irq_inhibit = nes->frame_irq_inhibit; st->frame_irq_flag = nes->frame_irq_flag;
    st->frame_reset_delay = nes->frame_reset_delay; st->frame_reset_mode = nes->frame_reset_mode;
    st->frame_irq_clear_pending = nes->frame_irq_clear_pending;
    st->frame_counter = nes->frame_counter; st->sample_acc = nes->sample_acc;
    st->lpf_prev = nes->lpf_prev; st->hpf_in = nes->hpf_in; st->hpf_out = nes->hpf_out;
    st->cpu_freq = nes->cpu_freq; st->num_scanlines = nes->num_scanlines;
    st->frame_cycle_rem = nes->frame_cycle_rem; st->frame_cpu_overrun = nes->frame_cpu_overrun;
}

static int restore_state(struct NES *nes, const struct nes_state_core *st) {
    if (st->magic != STATE_MAGIC || st->version != STATE_VERSION || st->size != sizeof(*st))
        return 0;
    if (st->mapper != nes->mapper || st->prg_size != nes->prg_size
        || st->chr_size != nes->chr_size || st->chr_is_ram != nes->chr_is_ram)
        return 0;

    st->is_pal ? init_pal(nes) : init_ntsc(nes);

    nes->mirror = st->mirror; nes->prg_bank = st->prg_bank; nes->chr_bank = st->chr_bank;
    nes->prg_banks = st->prg_banks; nes->chr_banks = st->chr_banks;
    nes->has_battery = st->has_battery;

    nes->pc = st->pc; nes->a = st->a; nes->x = st->x; nes->y = st->y;
    nes->sp = st->sp; nes->flags = st->flags;
    nes->cycles = st->cycles; nes->total_cycles = st->total_cycles;
    nes->nmi_pending = st->nmi_pending; nes->nmi_delay = st->nmi_delay;
    nes->prev_nmi_line = st->prev_nmi_line; nes->nmi_in_instr = st->nmi_in_instr;
    nes->nmi_instr_cycle = st->nmi_instr_cycle;

    nes->ppu_ctrl = st->ppu_ctrl; nes->ppu_mask = st->ppu_mask;
    nes->ppu_status = st->ppu_status; nes->oam_addr = st->oam_addr;
    nes->in_vblank = st->in_vblank; nes->odd_frame = st->odd_frame;
    nes->vram_addr = st->vram_addr; nes->temp_addr = st->temp_addr;
    nes->fine_x = st->fine_x; nes->write_toggle = st->write_toggle;
    nes->read_buf = st->read_buf; nes->ppu_open_bus = st->ppu_open_bus;
    nes->ppu_open_bus_decay_low = st->ppu_open_bus_decay_low;
    nes->ppu_open_bus_decay_high = st->ppu_open_bus_decay_high;

    for (int i = 0; i < 0x800; i++) nes->ram[i] = st->ram[i];
    for (int i = 0; i < 0x2000; i++) nes->sram[i] = st->sram[i];
    for (int i = 0; i < 0x1000; i++) nes->vram[i] = st->vram[i];
    for (int i = 0; i < 0x20; i++) nes->palette[i] = st->palette[i];
    for (int i = 0; i < 256; i++) nes->oam[i] = st->oam[i];
    if (nes->chr_is_ram && nes->chr)
        for (int i = 0; i < 0x2000; i++) nes->chr[i] = st->chr_ram[i];

    nes->mmc1_shift = st->mmc1_shift; nes->mmc1_count = st->mmc1_count;
    nes->mmc1_ctrl = st->mmc1_ctrl; nes->mmc1_chr0 = st->mmc1_chr0;
    nes->mmc1_chr1 = st->mmc1_chr1; nes->mmc1_prg = st->mmc1_prg;
    nes->mmc1_last_write_cycle = st->mmc1_last_write_cycle;
    nes->mmc3_select = st->mmc3_select;
    for (int i = 0; i < 8; i++) nes->mmc3_regs[i] = st->mmc3_regs[i];
    nes->mmc3_irq_latch = st->mmc3_irq_latch; nes->mmc3_irq_count = st->mmc3_irq_count;
    nes->mmc3_irq_enable = st->mmc3_irq_enable; nes->mmc3_irq_reload = st->mmc3_irq_reload;
    nes->mmc3_prg_mode = st->mmc3_prg_mode; nes->mmc3_chr_mode = st->mmc3_chr_mode;
    nes->mmc3_a12 = st->mmc3_a12;
    nes->mmc3_wram_enable = st->mmc3_wram_enable;
    nes->mmc3_wram_protect = st->mmc3_wram_protect;
    for (int i = 0; i < 2; i++) {
        nes->mmc2_chr_lo[i] = st->mmc2_chr_lo[i];
        nes->mmc2_chr_hi[i] = st->mmc2_chr_hi[i];
    }
    nes->mmc2_latch0 = st->mmc2_latch0; nes->mmc2_latch1 = st->mmc2_latch1;
    nes->fme7_cmd = st->fme7_cmd;
    for (int i = 0; i < 4; i++) nes->fme7_prg[i] = st->fme7_prg[i];
    for (int i = 0; i < 8; i++) nes->fme7_chr[i] = st->fme7_chr[i];
    nes->fme7_irq_ctrl = st->fme7_irq_ctrl;
    nes->fme7_irq_counter = st->fme7_irq_counter;
    nes->fme7_ram_mode = st->fme7_ram_mode;
    nes->chr_enable = st->chr_enable;

    nes->cpu_data_bus = st->cpu_data_bus;
    nes->pad_state = 0; nes->pad_shift = st->pad_shift; nes->pad_strobe = st->pad_strobe;
    nes->pad_out0 = st->pad_out0;
    nes->irq_pending = st->irq_pending; nes->prev_irq_inhibit = st->prev_irq_inhibit;
    nes->apu_irq_pending = st->apu_irq_pending;
    nes->pulse[0] = st->pulse[0]; nes->pulse[1] = st->pulse[1];
    nes->tri = st->tri; nes->noise = st->noise; nes->dmc = st->dmc;
    nes->apu_status = st->apu_status; nes->frame_mode = st->frame_mode;
    nes->frame_irq_inhibit = st->frame_irq_inhibit; nes->frame_irq_flag = st->frame_irq_flag;
    nes->frame_reset_delay = st->frame_reset_delay; nes->frame_reset_mode = st->frame_reset_mode;
    nes->frame_irq_clear_pending = st->frame_irq_clear_pending;
    nes->frame_counter = st->frame_counter; nes->sample_acc = st->sample_acc;
    nes->lpf_prev = st->lpf_prev; nes->hpf_in = st->hpf_in; nes->hpf_out = st->hpf_out;
    nes->frame_cycle_rem = st->frame_cycle_rem; nes->frame_cpu_overrun = st->frame_cpu_overrun;
    nes->audio_pos = 0;
    nes->sram_dirty = nes->has_battery ? 1 : nes->sram_dirty;
    return 1;
}

static int save_state(struct NES *nes, struct nes_state_core *st,
                      void *G, void *kopen, void *kwrite,
                      void *kclose, const char *path) {
    if (!st || !kopen || !kwrite || !kclose) return 0;
    s32 fd = (s32)NC(G, kopen, (u64)path, 0x0601, 0x1FF, 0, 0, 0);
    if (fd < 0) return 0;
    capture_state(nes, st);
    int ok = write_full(G, kwrite, fd, (const u8 *)st, sizeof(*st));
    NC(G, kclose, (u64)fd, 0, 0, 0, 0, 0);
    return ok;
}

static int load_state(struct NES *nes, struct nes_state_core *st,
                      void *G, void *kopen, void *kread,
                      void *kclose, const char *path) {
    if (!st || !kopen || !kread || !kclose) return 0;
    s32 fd = (s32)NC(G, kopen, (u64)path, 0, 0, 0, 0, 0);
    if (fd < 0) return 0;
    int ok = read_full(G, kread, fd, (u8 *)st, sizeof(*st));
    NC(G, kclose, (u64)fd, 0, 0, 0, 0, 0);
    return ok ? restore_state(nes, st) : 0;
}

static void save_sram(struct NES *nes, void *G, void *kopen, void *kwrite,
                      void *kclose, const char *save_path) {
    if (!nes->has_battery || !nes->sram_dirty || !kopen || !kwrite || !kclose) return;
    s32 fd = (s32)NC(G, kopen, (u64)save_path, 0x0601, 0x1FF, 0, 0, 0);
    if (fd < 0) return;
    NC(G, kwrite, (u64)fd, (u64)nes->sram, 0x2000, 0, 0, 0);
    NC(G, kclose, (u64)fd, 0, 0, 0, 0, 0);
    nes->sram_dirty = 0;
}

static void init_ntsc(struct NES *nes) {
    nes->is_pal = 0;
    nes->cpu_freq = 1789773;
    nes->num_scanlines = 262;
    nes->fc_step[0][0] = 7457;  nes->fc_step[0][1] = 14916;
    nes->fc_step[0][2] = 22371; nes->fc_step[0][3] = 29831;
    nes->fc_step[0][4] = 29832; nes->fc_step[0][5] = 29833;
    nes->fc_step[1][0] = 7457;  nes->fc_step[1][1] = 14916;
    nes->fc_step[1][2] = 22371; nes->fc_step[1][3] = 29829;
    nes->fc_step[1][4] = 37284; nes->fc_step[1][5] = 37285;
}

static void init_pal(struct NES *nes) {
    nes->is_pal = 1;
    nes->cpu_freq = 1662607;
    nes->num_scanlines = 312;
    nes->fc_step[0][0] = 8313;  nes->fc_step[0][1] = 16627;
    nes->fc_step[0][2] = 24939; nes->fc_step[0][3] = 33252;
    nes->fc_step[0][4] = 33253; nes->fc_step[0][5] = 33254;
    nes->fc_step[1][0] = 8313;  nes->fc_step[1][1] = 16627;
    nes->fc_step[1][2] = 24939; nes->fc_step[1][3] = 33253;
    nes->fc_step[1][4] = 41565; nes->fc_step[1][5] = 41566;
}

static int poll_ready(void *G, void *poll, s32 fd, s32 timeout_ms) {
    u8 pfd[8];
    *(s32*)pfd = fd;
    *(u16*)(pfd + 4) = 0x0001;
    *(u16*)(pfd + 6) = 0;
    return (s32)NC(G, poll, (u64)pfd, 1, (u64)timeout_ms, 0, 0, 0) > 0;
}

static int parse_pad_last(u8 *buf, s32 len) {
    int val = -1;
    for (s32 i = len - 2; i >= 1; i--) {
        if (buf[i] == '/' && buf[i+1] == 'b') {
            val = 0;
            for (s32 j = i + 2; j < len && j < i + 8; j++) {
                if (buf[j] >= '0' && buf[j] <= '9') val = val * 10 + (buf[j] - '0');
                else break;
            }
            break;
        }
    }
    return val;
}

static int count_posts(u8 *buf, s32 len) {
    int c = 0;
    for (s32 i = 0; i < len - 4; i++)
        if (buf[i] == 'P' && buf[i+1] == 'O' && buf[i+2] == 'S' && buf[i+3] == 'T') c++;
    return c;
}

static u8 web_handle(void *G, void *poll, void *accept, void *recv,
                     void *send, void *close, void *sso, s32 listen_fd,
                     s32 *keep_fd, u8 *page, u64 page_len, u8 *pad_out) {
    u8 got_input = 0;
    u8 req[512];

    if (*keep_fd >= 0) {
        for (int r = 0; r < 8; r++) {
            if (!poll_ready(G, poll, *keep_fd, 0)) break;
            s32 n = (s32)NC(G, recv, (u64)*keep_fd, (u64)req, 512, 0x80, 0, 0);
            if (n <= 0) { NC(G, close, (u64)*keep_fd, 0,0,0,0,0); *keep_fd = -1; break; }
            int v = parse_pad_last(req, n);
            if (v >= 0) {
                *pad_out = (u8)v;
                got_input = 1;
                int np = count_posts(req, n);
                for (int k = 0; k < np; k++)
                    NC(G, send, (u64)*keep_fd, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        if (!poll_ready(G, poll, listen_fd, 0)) break;

        u8 sa[16]; s32 sa_len = 16;
        s32 client = (s32)NC(G, accept, (u64)listen_fd, (u64)sa, (u64)&sa_len, 0, 0, 0);
        if (client < 0) break;

        if (sso) { s32 one = 1; NC(G, sso, (u64)client, 6, 1, (u64)&one, 4, 0); }

        if (!poll_ready(G, poll, client, 0)) {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
            continue;
        }

        s32 n = (s32)NC(G, recv, (u64)client, (u64)req, 512, 0x80, 0, 0);

        if (n > 7 && req[0] == 'P' && req[5] == '/' && req[6] == 'b') {
            int v = parse_pad_last(req, n);
            if (v >= 0) { *pad_out = (u8)v; got_input = 1; }
            NC(G, send, (u64)client, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            if (*keep_fd >= 0) NC(G, close, (u64)*keep_fd, 0,0,0,0,0);
            *keep_fd = client;
        } else if (n > 5 && req[0] == 'G' && req[4] == '/') {
            u64 off = 0;
            while (off < page_len) {
                u64 chunk = page_len - off;
                if (chunk > 2048) chunk = 2048;
                NC(G, send, (u64)client, (u64)(page + off), chunk, 0, 0, 0);
                off += chunk;
            }
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else if (n > 0 && req[0] == 'O') {
            NC(G, send, (u64)client, (u64)RESP_CORS, (u64)str_len(RESP_CORS), 0, 0, 0);
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        }
    }
    return got_input;
}

static void nes_reset(struct NES *nes, void *G, void *audio_fn, s32 audio_h) {
    u8 *p = (u8 *)nes;
    for (u32 i = 0; i < sizeof(struct NES); i++) p[i] = 0;
    init_ntsc(nes);
    nes->gadget = G;
    nes->audio_out_fn = audio_fn;
    nes->audio_handle = audio_h;
    nes->noise.shift_reg = 1;
}

static u8 ds_to_nes(u32 b) {
    u8 r = 0;
    if (b & 0x00004000) r |= 0x01;  /* CROSS    → A     */
    if (b & 0x00008000) r |= 0x02;  /* SQUARE   → B     */
    if (b & 0x00001000) r |= 0x04;  /* TRIANGLE → Sel   */
    if (b & 0x00002000) r |= 0x08;  /* CIRCLE   → Start */
    if (b & 0x00000008) r |= 0x08;  /* OPTIONS  → Start */
    if (b & 0x00000010) r |= 0x10;  /* UP              */
    if (b & 0x00000040) r |= 0x20;  /* DOWN            */
    if (b & 0x00000080) r |= 0x40;  /* LEFT            */
    if (b & 0x00000020) r |= 0x80;  /* RIGHT           */
    if (b & 0x00000100) r = CMD_STATE_SAVE; /* L2 -> Save state */
    if (b & 0x00000200) r = CMD_STATE_LOAD; /* R2 -> Load state */
    if (b & 0x00000400) r = CMD_MENU;       /* L1 -> Menu       */
    if (b & 0x00000800) r = CMD_EXIT;       /* R1 -> Exit       */
    return r;
}

static s32 read_native_pad(void *G, void *pad_read, s32 pad_h, u8 *pbuf) {
    if (pad_h < 0 || !pad_read) return -1;
    for (int i = 0; i < 128; i++) pbuf[i] = 0;
    s32 n = (s32)NC(G, pad_read, (u64)pad_h, (u64)pbuf, 1, 0, 0, 0);
    if (n <= 0 || (u32)n >= 0x80000000) return -1;
    u32 raw = *(u32 *)pbuf;
    if (raw & 0x80000000) return -1;
    return (s32)ds_to_nes(raw & 0x001FFFFF);
}

__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {
    void *G = (void *)(eboot_base + GADGET_OFFSET);
    void *D = (void *)dlsym_addr;
    ext->step = 1;

    void *usleep    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    void *cancel    = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    void *load_mod  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    void *alloc_dm  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    void *map_dm    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    void *dm_size   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    void *create_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    void *wait_eq   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    void *mmap      = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    void *munmap    = SYM(G, D, LIBKERNEL_HANDLE, "munmap");
    void *kopen     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    void *kread     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelRead");
    void *kwrite    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    void *kclose    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    void *kmkdir    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMkdir");
    void *delete_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");
    void *recvfrom  = SYM(G, D, LIBKERNEL_HANDLE, "recvfrom");
    void *sendto    = SYM(G, D, LIBKERNEL_HANDLE, "sendto");
    void *accept    = SYM(G, D, LIBKERNEL_HANDLE, "accept");
    void *poll      = SYM(G, D, LIBKERNEL_HANDLE, "poll");
    void *setsockopt_fn = SYM(G, D, LIBKERNEL_HANDLE, "setsockopt");
    void *getsockname_fn = SYM(G, D, LIBKERNEL_HANDLE, "getsockname");
    void *getdents  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetdents");
    if (!getdents)   getdents = SYM(G, D, LIBKERNEL_HANDLE, "getdents");

    s32 log_fd = ext->log_fd;
    u8 log_sa[16];
    for (int i = 0; i < 16; i++) log_sa[i] = ext->log_addr[i];

    s32 web_fd   = (s32)ext->dbg[0];
    u8 *web_page = (u8 *)ext->dbg[1];
    u64 web_len  = ext->dbg[2];
    s32 userId   = (s32)ext->dbg[3];
    /* FTP listen FDs are created in Lua (sceNet/BSD via LuaC0re), not libkernel socket(). */
    s32 ftp_fd   = (s32)ext->dbg[4];
    s32 ftp_data_fd = (s32)ext->dbg[5];

    if (!usleep || !load_mod) { ext->status = -1; ext->step = 2; return; }

    s32 vid_mod = (s32)NC(G, load_mod, (u64)"libSceVideoOut.sprx", 0,0,0,0,0);
    s32 aud_mod = (s32)NC(G, load_mod, (u64)"libSceAudioOut.sprx", 0,0,0,0,0);

    void *vid_open  = SYM(G, D, vid_mod, "sceVideoOutOpen");
    void *vid_close = SYM(G, D, vid_mod, "sceVideoOutClose");
    void *vid_reg   = SYM(G, D, vid_mod, "sceVideoOutRegisterBuffers");
    void *vid_flip  = SYM(G, D, vid_mod, "sceVideoOutSubmitFlip");
    void *vid_rate  = SYM(G, D, vid_mod, "sceVideoOutSetFlipRate");
    void *vid_evt   = SYM(G, D, vid_mod, "sceVideoOutAddFlipEvent");
    void *aud_open  = SYM(G, D, aud_mod, "sceAudioOutOpen");
    void *aud_out   = SYM(G, D, aud_mod, "sceAudioOutOutput");
    void *aud_close = SYM(G, D, aud_mod, "sceAudioOutClose");

    ext->step = 5;

    if (cancel) {
        u64 gs = *(u64 *)(eboot_base + EBOOT_GS_THREAD);
        if (gs) NC(G, cancel, gs, 0,0,0,0,0);
    }
    NC(G, usleep, 300000, 0,0,0,0,0);

    s32 emu_vid = *(s32 *)(eboot_base + EBOOT_VIDOUT);
    if (vid_close && emu_vid >= 0) NC(G, vid_close, (u64)emu_vid, 0,0,0,0,0);
    NC(G, usleep, 100000, 0,0,0,0,0);

    s32 video = (s32)NC(G, vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (video < 0) { ext->status = -10; ext->step = 11; return; }

    u64 eq = 0;
    if (create_eq) NC(G, create_eq, (u64)&eq, (u64)"nesq", 0,0,0,0);
    if (vid_evt && eq) NC(G, vid_evt, eq, (u64)video, 0,0,0,0);

    u64 mem_total = dm_size ? NC(G, dm_size, 0,0,0,0,0,0) : 0x300000000ULL;
    u64 phys = 0;
    NC(G, alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    void *vmem = 0;
    NC(G, map_dm, (u64)&vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!vmem) { ext->status = -21; ext->step = 22; return; }

    u8 attr[64];
    for (int i = 0; i < 64; i++) attr[i] = 0;
    *(u32*)(attr + 0)  = 0x80000000;
    *(u32*)(attr + 4)  = 1;
    *(u32*)(attr + 12) = SCR_W;
    *(u32*)(attr + 16) = SCR_H;
    *(u32*)(attr + 20) = SCR_W;

    void *fbs[FB_COUNT];
    fbs[0] = vmem;
    fbs[1] = (u8*)vmem + FB_ALIGNED;
    fbs[2] = (u8*)vmem + FB_ALIGNED * 2;

    if (NC(G, vid_reg, (u64)video, 0, (u64)fbs, FB_COUNT, (u64)attr, 0) != 0) {
        ext->status = -30; ext->step = 30; return;
    }
    if (vid_rate) NC(G, vid_rate, (u64)video, 0, 0,0,0,0);
    clear_fb((u32*)fbs[0]);
    clear_fb((u32*)fbs[1]);
    clear_fb((u32*)fbs[2]);

    struct NES *nes = (struct NES *)NC(G, mmap, 0,
        sizeof(struct NES) + 0x10000, 3, 0x1002, (u64)-1, 0);
    if ((s64)nes == -1) { ext->status = -40; ext->step = 16; return; }

    nes_reset(nes, G, 0, -1);

    u8 *rom_buf = (u8 *)NC(G, mmap, 0, ROM_BUF_SIZE, 3, 0x1002, (u64)-1, 0);
    if ((s64)rom_buf == -1) rom_buf = 0;
    u8 *chr_ram = (u8 *)NC(G, mmap, 0, 0x2000, 3, 0x1002, (u64)-1, 0);
    struct nes_state_core *state_buf = (struct nes_state_core *)NC(G, mmap, 0,
        sizeof(struct nes_state_core), 3, 0x1002, (u64)-1, 0);
    if ((s64)state_buf == -1) state_buf = 0;

    NC(G, load_mod, (u64)"libSceUserService.sprx", 0,0,0,0,0);
    if (aud_close)
        for (int h = 0; h < 8; h++) NC(G, aud_close, (u64)h, 0,0,0,0,0);

    s32 audio_h = -1;
    if (aud_open)
        audio_h = (s32)NC(G, aud_open, 0xFF, 0, 0, SAMPLES_PER_BUF, SAMPLE_RATE, AUDIO_S16_STEREO);

    s32 pad_mod = (s32)NC(G, load_mod, (u64)"libScePad.sprx", 0,0,0,0,0);
    void *pad_init_fn = SYM(G, D, pad_mod, "scePadInit");
    void *pad_geth    = SYM(G, D, pad_mod, "scePadGetHandle");
    void *pad_read    = SYM(G, D, pad_mod, "scePadRead");
    if (pad_init_fn) NC(G, pad_init_fn, 0,0,0,0,0,0);
    s32 pad_h = -1;
    if (pad_geth) pad_h = (s32)NC(G, pad_geth, (u64)userId, 0, 0, 0, 0, 0);
    u8 pad_buf[128];

    nes->gadget = G;
    nes->audio_out_fn = aud_out;
    nes->audio_handle = audio_h;
    nes->noise.shift_reg = 1;

    udp_log(G, sendto, log_fd, log_sa, "EgyDevTeam NES EMU V0.4 by egycnq\n");
    udp_log(G, sendto, log_fd, log_sa, pad_h >= 0 ? "Native pad OK\n" : "Native pad N/A\n");

    struct rom_entry *roms = (struct rom_entry *)NC(G, mmap, 0,
        sizeof(struct rom_entry) * MAX_ROMS, 3, 0x1002, (u64)-1, 0);
    int rom_count = 0;
    const char *rom_dir = ROM_DIR;

    if ((s64)roms != -1) {
        /* Show FTP waiting screen */
        u8 *scr = nes->screen;
        draw_menu_shell(scr, "NES EMULATOR");
        draw_box(scr, 28, 58, 200, 92, COL_LINE);
        draw_rect(scr, 30, 60, 196, 88, COL_PANEL);
        draw_centered(scr, 74, "ROM DROP READY", COL_SEL);
        draw_centered(scr, 96, "FTP PORT 1337", COL_NORM);
        draw_centered(scr, 118, "UPLOAD .NES OR .ROM", COL_DIM);
        draw_centered(scr, 212, "WAITING FOR LAUNCHER", COL_DIM);
        scale_to_framebuf((u32*)fbs[0], scr, 0);
        scale_to_framebuf((u32*)fbs[1], scr, 0);
        scale_to_framebuf((u32*)fbs[2], scr, 0);
        NC(G, vid_flip, (u64)video, 0, 1, 0, 0, 0);

        if (ftp_fd < 0 || ftp_data_fd < 0)
            udp_log(G, sendto, log_fd, log_sa, "FTP: Lua listen FDs missing — skip\n");
        else
            udp_log(G, sendto, log_fd, log_sa, "FTP: using Lua sockets\n");

        rom_count = ftp_serve(ftp_fd, ftp_data_fd,
                              G, D, load_mod, mmap, kopen, kwrite, kclose,
                              kmkdir, getdents, usleep,
                              recvfrom, sendto, accept,
                              getsockname_fn,
                              log_fd, log_sa, userId,
                              roms, MAX_ROMS);
        /*
         * ftp_serve no longer closes listen FDs — Lua closes them after
         * shellcode returns so a second inject can re-bind cleanly.
         */
        if (rom_count > 0) udp_log(G, sendto, log_fd, log_sa, "FTP ROMs loaded\n");
    }

    /* Scan filesystem for any additional ROMs not from FTP */
    if (kopen && getdents && (s64)roms != -1) {
        s32 dfd = (s32)NC(G, kopen, (u64)ROM_DIR, 0x20000, 0, 0, 0, 0);
        if (dfd < 0) {
            rom_dir = "/savedata0/";
            dfd = (s32)NC(G, kopen, (u64)"/savedata0/", 0x20000, 0, 0, 0, 0);
        }
        if (dfd >= 0) {
            u8 *dbuf = (u8 *)NC(G, mmap, 0, 0x2000, 3, 0x1002, (u64)-1, 0);
            if ((s64)dbuf != -1) {
                for (;;) {
                    s32 nread = (s32)NC(G, getdents, (u64)dfd, (u64)dbuf, 0x2000, 0, 0, 0);
                    if (nread <= 0) break;
                    int off = 0;
                    while (off < nread && rom_count < MAX_ROMS) {
                        u16 reclen = *(u16 *)(dbuf + off + 4);
                        u8 namlen  = *(u8 *)(dbuf + off + 7);
                        char *name = (char *)(dbuf + off + 8);
                        if (reclen == 0) break;
                        if (namlen > 0 && is_rom_file(name)) {
                            /* skip if already registered by FTP */
                            int dup = 0;
                            for (int j = 0; j < rom_count; j++) {
                                int match = 1;
                                for (int c = 0; c < 47; c++) {
                                    if (roms[j].filename[c] != name[c]) { match = 0; break; }
                                    if (!name[c]) break;
                                }
                                if (match) { dup = 1; break; }
                            }
                            if (!dup) {
                                int k = 0;
                                while (name[k] && k < 47) { roms[rom_count].filename[k] = name[k]; k++; }
                                roms[rom_count].filename[k] = '\0';
                                extract_rom_name(name, roms[rom_count].display, MAX_NAME);
                                rom_count++;
                            }
                        }
                        off += reclen;
                    }
                    if (rom_count >= MAX_ROMS) break;
                }
                if (munmap) NC(G, munmap, (u64)dbuf, 0x2000, 0,0,0,0);
            }
            NC(G, kclose, (u64)dfd, 0,0,0,0,0);
        }
    }

    if (rom_count == 0 && kopen && kclose) {
        s32 tfd = (s32)NC(G, kopen, (u64)"/savedata0/nes.rom", 0,0,0,0,0);
        if (tfd >= 0) {
            rom_dir = "/savedata0/";
            NC(G, kclose, (u64)tfd, 0,0,0,0,0);
            const char *fn = "nes.rom";
            int k = 0; while (fn[k]) { roms[0].filename[k] = fn[k]; k++; }
            roms[0].filename[k] = '\0';
            extract_rom_name(fn, roms[0].display, MAX_NAME);
            rom_count = 1;
        }
    }

    u32 total_frames = 0;
    int active = 0;
    int has_web = (web_fd >= 0 && poll && accept);
    int input_src = 0;
    u8 web_pad = 0;
    s32 web_client = -1;

    for (;;) {
        int selected = 0;

        if (rom_count > 0) {
            int cursor = 0, scroll = 0, mframe = 0, hold = 0;
            u8 prev_btn = 0;
            int visible = 14;
            if (visible > rom_count) visible = rom_count;

            for (;;) {
                u8 btn = 0;

                u8 wb = 0; int web_got = 0;
                if (has_web) {
                    web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                        setsockopt_fn, web_fd, &web_client, web_page, web_len, &wb);
                    if (web_got) web_pad = wb;
                }

                s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

                if (input_src == 0) {
                    if (nb > 0 && nb < CMD_STATE_LOAD) {
                        input_src = 1;  btn = (u8)nb;
                        udp_log(G, sendto, log_fd, log_sa, "Input: native pad\n");
                    }
                    else if (web_got && web_pad > 0 && web_pad < CMD_STATE_LOAD) {
                        input_src = 2; btn = web_pad;
                        udp_log(G, sendto, log_fd, log_sa, "Input: web controller\n");
                    }
                    if (web_got && web_pad >= CMD_STATE_LOAD) btn = web_pad;
                    if (nb >= CMD_STATE_LOAD) btn = (u8)nb;
                } else if (input_src == 1) {
                    if (nb >= 0) btn = (u8)nb;
                } else {
                    btn = web_pad;
                }

                if (btn >= CMD_STATE_LOAD) web_pad = 0;
                if (btn == CMD_EXIT) goto done;
                if (btn == CMD_STATE_LOAD || btn == CMD_STATE_SAVE) btn = 0;

                u8 pressed = btn & ~prev_btn;
                int move = 0;
                if (btn & 0x10) { hold++; if ((pressed & 0x10) || (hold > 12 && hold % 4 == 0)) move = -1; }
                else if (btn & 0x20) { hold++; if ((pressed & 0x20) || (hold > 12 && hold % 4 == 0)) move = 1; }
                else hold = 0;

                if (move) {
                    cursor += move;
                    if (cursor < 0) cursor = rom_count - 1;
                    if (cursor >= rom_count) cursor = 0;
                    if (cursor < scroll) scroll = cursor;
                    if (cursor >= scroll + visible) scroll = cursor - visible + 1;
                }
                if ((pressed & 0x01) || (pressed & 0x08)) { selected = cursor; break; }
                prev_btn = btn;

                u8 *scr = nes->screen;
                draw_menu_shell(scr, "SELECT A GAME");
                draw_str(scr, 16, 42, "LIBRARY", COL_HEAD);
                int cx = 170;
                draw_str(scr, cx, 42, "ROMS ", COL_DIM);
                draw_uint(scr, cx + 40, 42, rom_count, COL_NUM);
                draw_box(scr, 10, 52, 236, 143, COL_PANEL_2);
                draw_vline(scr, 239, 58, 189, COL_DIM);
                if (rom_count > visible) {
                    int track = 131;
                    int thumb_h = visible * track / rom_count;
                    if (thumb_h < 8) thumb_h = 8;
                    int thumb_y = 58 + scroll * (track - thumb_h) / (rom_count - visible);
                    draw_rect(scr, 238, thumb_y, 3, thumb_h, COL_SEL);
                }

                int ly = 56;
                for (int i = 0; i < visible && scroll + i < rom_count; i++) {
                    int idx = scroll + i;
                    int iy = ly + i * 10;
                    int sel = (idx == cursor);
                    int num = idx + 1;
                    if (sel) {
                        draw_rect(scr, 13, iy - 1, 222, 10, COL_PANEL_2);
                        draw_vline(scr, 14, iy - 1, iy + 9, COL_CUR);
                    }
                    if (sel && (mframe / 10) % 2)
                        draw_char(scr, 20, iy, '>', COL_CUR);
                    u8 nc = sel ? COL_SEL : COL_NUM;
                    int nx = 34;
                    nx = draw_uint(scr, nx, iy, num, nc);
                    draw_char(scr, nx, iy, '.', nc);
                    draw_str_limit(scr, 70, iy, roms[idx].display, 20, sel ? COL_SEL : COL_NORM);
                }

                draw_str_limit(scr, 16, 212, roms[cursor].display, 28, COL_HEAD);
                draw_str(scr, 16, 224, "A/START PLAY", COL_SEL);
                draw_str(scr, 144, 224, "R1/TAB EXIT", COL_DIM);

                scale_to_framebuf((u32*)fbs[active], scr, 0);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active = next_fb(active);
                mframe++;
                total_frames++;
            }
        } else {
            for (int f = 0; f < 300; f++) {
                u8 *scr = nes->screen;
                draw_menu_shell(scr, "ROM LIBRARY");
                draw_box(scr, 24, 62, 208, 102, COL_LINE);
                draw_rect(scr, 26, 64, 204, 98, COL_PANEL);
                draw_centered(scr, 78, "NO ROMS FOUND", COL_BRAND);
                draw_centered(scr, 102, "UPLOAD .NES OR .ROM", COL_NORM);
                draw_centered(scr, 124, "FTP PORT 1337", COL_DIM);
                draw_centered(scr, 146, "OR USE /SAVEDATA0/", COL_DIM);
                draw_centered(scr, 212, "RELAUNCH AFTER COPY", COL_DIM);
                scale_to_framebuf((u32*)fbs[active], scr, 0);
                NC(G, vid_flip, (u64)video, (u64)active, 1, (u64)f, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active = next_fb(active);
            }
            break;
        }

        char rom_path[96];
        { int pi = 0; const char *p = rom_dir;
          while (*p) rom_path[pi++] = *p++;
          const char *f = roms[selected].filename;
          while (*f && pi < 94) rom_path[pi++] = *f++;
          rom_path[pi] = 0; }
        char save_path[96];
        char state_path[96];
        build_ext_path(rom_path, save_path, 96, "sav");
        build_ext_path(rom_path, state_path, 96, "sst");
        clear_fb((u32*)fbs[0]);
        clear_fb((u32*)fbs[1]);
        clear_fb((u32*)fbs[2]);
        nes_reset(nes, G, aud_out, audio_h);

        nes->rom_loaded = 0;
        const char *load_error = "LOAD UNAVAILABLE";
        if (kopen && kread && kclose && rom_buf) {
            load_error = "OPEN FAILED";
            s32 fd = (s32)NC(G, kopen, (u64)rom_path, 0,0,0,0,0);
            if (fd >= 0) {
                load_error = "BAD ROM";
                u8 hdr[16];
                s32 hr = (s32)NC(G, kread, (u64)fd, (u64)hdr, 16, 0, 0, 0);
                if (hr == 16 && hdr[0]=='N' && hdr[1]=='E' && hdr[2]=='S' && hdr[3]==0x1A) {
                    load_error = "READ FAILED";
                    int nes2 = ((hdr[7] & 0x0C) == 0x08);
                    int prg_units = hdr[4];
                    int chr_units = hdr[5];
                    int pal_rom = 0;
                    if (nes2 && (hdr[9] & 0x0F) != 0x0F && (hdr[9] >> 4) != 0x0F) {
                        prg_units |= (hdr[9] & 0x0F) << 8;
                        chr_units |= (hdr[9] >> 4) << 8;
                    }
                    if (nes2) {
                        pal_rom = ((hdr[12] & 3) == 1);
                    } else {
                        int clean_tail = 1;
                        for (int i = 10; i < 16; i++)
                            if (hdr[i]) clean_tail = 0;
                        pal_rom = clean_tail && hdr[9] == 1;
                    }

                    nes->prg_size = prg_units * 0x4000;
                    nes->chr_size = chr_units * 0x2000;
                    nes->chr_banks = chr_units;
                    nes->mirror = (hdr[6] & 8) ? 4 : (hdr[6] & 1);
                    nes->mapper = (hdr[7] & 0xF0) | ((hdr[6] >> 4) & 0x0F);
                    if (nes2) nes->mapper |= (hdr[8] & 0x0F) << 8;
                    nes->prg_banks = prg_units;
                    nes->has_battery = (hdr[6] & 2) ? 1 : 0;

                    if (!mapper_supported(nes->mapper)) {
                        udp_log(G, sendto, log_fd, log_sa, "Unsupported mapper\n");
                        load_error = "UNSUPPORTED MAPPER";
                    } else if (nes->prg_size <= 0 || nes->prg_size + nes->chr_size > ROM_BUF_SIZE) {
                        udp_log(G, sendto, log_fd, log_sa, "ROM too large\n");
                        load_error = "ROM TOO LARGE";
                    } else {
                        if (hdr[6] & 4) {
                            u8 trainer[512];
                            read_full(G, kread, fd, trainer, 512);
                        }

                        nes->prg = rom_buf;
                        if (read_full(G, kread, fd, nes->prg, nes->prg_size)) {
                            if (nes->chr_size > 0) {
                                nes->chr = rom_buf + nes->prg_size;
                                nes->chr_is_ram = 0;
                                if (!read_full(G, kread, fd, nes->chr, nes->chr_size))
                                    nes->chr = 0;
                            } else {
                                nes->chr = chr_ram;
                                nes->chr_size = 0x2000;
                                nes->chr_is_ram = 1;
                                for (int i = 0; i < 0x2000; i++) chr_ram[i] = 0;
                            }

                            if (nes->chr) {
                                mapper_init(nes);

                                if (nes->has_battery)
                                    load_sram(nes, G, kopen, kread, kclose, save_path);

                                nes->sp = 0xFD;
                                nes->flags = F_I | F_U;
                                nes->prev_irq_inhibit = F_I;
                                nes->pc = cpu_read16(nes, 0xFFFC);
                                nes->rom_loaded = 1;

                                if (pal_rom)
                                    init_pal(nes);
                                load_error = 0;
                            }
                        }
                    }
                }
                NC(G, kclose, (u64)fd, 0,0,0,0,0);
            }
        }

        if (!nes->rom_loaded) {
            for (int f = 0; f < 90; f++) {
                u8 *scr = nes->screen;
                draw_menu_shell(scr, "LOAD ERROR");
                draw_box(scr, 24, 70, 208, 88, COL_LINE);
                draw_rect(scr, 26, 72, 204, 84, COL_PANEL);
                draw_centered(scr, 88, load_error ? load_error : "LOAD FAILED", COL_BRAND);
                draw_str_limit(scr, 24, 116, roms[selected].display, 26, COL_NORM);
                draw_centered(scr, 140, "RETURNING TO LIBRARY", COL_DIM);
                scale_to_framebuf((u32*)fbs[active], scr, 0);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active = next_fb(active);
                total_frames++;
            }
            continue;
        }

        udp_log(G, sendto, log_fd, log_sa, rom_path);
        udp_log(G, sendto, log_fd, log_sa, nes->is_pal ? " PAL" : " NTSC");
        {
            char mbuf[48];
            int n = 0;
            const char *p = " mapper=";
            while (*p) mbuf[n++] = *p++;
            {
                int m = nes->mapper, t, d[4], nd = 0;
                if (m == 0) d[nd++] = 0;
                else { t = m; if (t < 0) t = -t; while (t && nd < 4) { d[nd++] = t % 10; t /= 10; } }
                while (nd--) mbuf[n++] = (char)('0' + d[nd]);
            }
            mbuf[n++] = '\n';
            mbuf[n] = 0;
            udp_log(G, sendto, log_fd, log_sa, mbuf);
        }

        apu_prime(nes, 2);

        u32 frame = 0;
        int pal_acc = 60;
        int back_to_menu = 0;
        const char *state_msg = 0;
        int state_msg_frames = 0;

        for (;;) {
            u8 wb = 0; int web_got = 0;
            if (has_web) {
                web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                    setsockopt_fn, web_fd, &web_client, web_page, web_len, &wb);
                if (web_got) web_pad = wb;
            }

            s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

            if (input_src == 0) {
                if (nb > 0 && nb < CMD_STATE_LOAD) {
                    input_src = 1; nes->pad_state = (u8)nb;
                    udp_log(G, sendto, log_fd, log_sa, "Input: native pad\n");
                }
                else if (web_got && web_pad > 0 && web_pad < CMD_STATE_LOAD) {
                    input_src = 2; nes->pad_state = web_pad;
                    udp_log(G, sendto, log_fd, log_sa, "Input: web controller\n");
                }
                if (web_got && web_pad >= CMD_STATE_LOAD) nes->pad_state = web_pad;
                if (nb >= CMD_STATE_LOAD) nes->pad_state = (u8)nb;
            } else if (input_src == 1) {
                if (nb >= 0) nes->pad_state = (u8)nb;
            } else {
                nes->pad_state = web_pad;
            }

            if (nes->pad_state >= CMD_STATE_LOAD) web_pad = 0;
            if (nes->pad_state == CMD_EXIT) {
                save_sram(nes, G, kopen, kwrite, kclose, save_path);
                goto done;
            }
            if (nes->pad_state == CMD_MENU) {
                save_sram(nes, G, kopen, kwrite, kclose, save_path);
                back_to_menu = 1; nes->pad_state = 0; break;
            }
            if (nes->pad_state == CMD_STATE_SAVE) {
                int ok = save_state(nes, state_buf, G, kopen, kwrite, kclose, state_path);
                udp_log(G, sendto, log_fd, log_sa, ok ? "State saved\n" : "State save failed\n");
                state_msg = ok ? "STATE SAVED" : "SAVE FAILED";
                state_msg_frames = 60;
                nes->pad_state = 0;
            } else if (nes->pad_state == CMD_STATE_LOAD) {
                int ok = load_state(nes, state_buf, G, kopen, kread, kclose, state_path);
                udp_log(G, sendto, log_fd, log_sa, ok ? "State loaded\n" : "State load failed\n");
                state_msg = ok ? "STATE LOADED" : "LOAD FAILED";
                state_msg_frames = 60;
                nes->pad_state = 0;
            }

            int run_game = 1;
            if (nes->is_pal) {
                pal_acc += 50;
                if (pal_acc >= 60) pal_acc -= 60; else run_game = 0;
            }

            if (run_game) {
                run_frame(nes);
                if (nes->rom_loaded) apu_flush(nes);
            }
            if (state_msg_frames > 0 && state_msg) {
                draw_rect(nes->screen, 74, 4, 108, 14, COL_PANEL);
                draw_box(nes->screen, 72, 2, 112, 18, COL_LINE);
                draw_centered(nes->screen, 8, state_msg, COL_SEL);
                state_msg_frames--;
            }

            scale_to_framebuf((u32*)fbs[active], nes->screen, nes->ppu_mask);
            NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);

            if (eq && wait_eq) {
                u8 evt[64]; s32 cnt = 0;
                NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
            }

            active = next_fb(active);
            if (run_game) frame++;
            total_frames++;
            ext->frame_count = total_frames;
        }

        if (!back_to_menu) break;
    }

done:
    udp_log(G, sendto, log_fd, log_sa, "Shutting down...\n");

    if (aud_close && audio_h >= 0)
        NC(G, aud_close, (u64)audio_h, 0,0,0,0,0);

    clear_fb((u32*)fbs[0]);
    clear_fb((u32*)fbs[1]);
    clear_fb((u32*)fbs[2]);
    NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
    if (usleep) NC(G, usleep, 50000, 0,0,0,0,0);

    if (vid_close && video >= 0)
        NC(G, vid_close, (u64)video, 0,0,0,0,0);

    if (delete_eq && eq)
        NC(G, delete_eq, eq, 0,0,0,0,0);

    if (web_client >= 0 && kclose)
        NC(G, kclose, (u64)web_client, 0,0,0,0,0);

    if (web_fd >= 0 && kclose)
        NC(G, kclose, (u64)web_fd, 0,0,0,0,0);

    if (munmap) {
        if ((s64)nes != -1)
            NC(G, munmap, (u64)nes, (u64)(sizeof(struct NES) + 0x10000), 0,0,0,0);
        if (rom_buf)
            NC(G, munmap, (u64)rom_buf, ROM_BUF_SIZE, 0,0,0,0);
        if (chr_ram && (s64)chr_ram != -1)
            NC(G, munmap, (u64)chr_ram, 0x2000, 0,0,0,0);
        if (state_buf)
            NC(G, munmap, (u64)state_buf, (u64)sizeof(struct nes_state_core), 0,0,0,0);
        if ((s64)roms != -1)
            NC(G, munmap, (u64)roms, (u64)(sizeof(struct rom_entry) * MAX_ROMS), 0,0,0,0);
    }

    udp_log(G, sendto, log_fd, log_sa, "Clean exit\n");
    ext->status = 0;
    ext->step = 99;
    ext->frame_count = total_frames;
}
