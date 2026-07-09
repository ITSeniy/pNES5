#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static s32 g_ticks_at_dma[16];
static s32 g_cy_at_dma[16];
static u16 g_halt[16];
static int g_n;

void dmc_phase_log_service(struct NES *n, u16 h, int s) {
    if (g_n < 16) {
        g_ticks_at_dma[g_n] = n->dmc.ticks_exec;
        /* Host harness: cycles accumulates; do not add total_cycles (double-count). */
        g_cy_at_dma[g_n] = n->cycles - s;
        g_halt[g_n] = h;
    }
    g_n++;
    printf("DMA#%d halt=%04X cy=%d bits=%d tc=%d full=%d\n", g_n, h, (int)(n->cycles - s),
           n->dmc.bits_left, n->dmc.timer_count, n->dmc.sample_buf_full);
}

static void step1(struct NES *n) {
    n->dmc.ticks_exec = 0;
    int b = n->cycles;
    cpu_step(n);
    int r = n->cycles - b;
    if (r > 0) {
        while (n->dmc.ticks_exec < r)
            dmc_tick(n);
        apu_step(n, r);
        n->total_cycles += r;
    }
}

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    memset(prg + 0x7FC0, 0, 0x3A);
    prg[0x7FFC] = 0;
    prg[0x7FFD] = 0x80;
    prg[0] = 0x40;

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    nes.noise.shift_reg = 1;
    nes.audio_handle = -1;
    nes.fc_step[0][0] = 7457;
    nes.fc_step[0][1] = 14916;
    nes.fc_step[0][2] = 22371;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;

    /* Just enable DMC loop and NOP forever; measure get intervals */
    u8 *r = nes.ram;
    u16 pc = 0x200;
    r[pc++] = 0xA9;
    r[pc++] = 0x4F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0xFF;
    r[pc++] = 0x8D;
    r[pc++] = 0x12;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    r[pc++] = 0x8D;
    r[pc++] = 0x13;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0x10;
    r[pc++] = 0x8D;
    r[pc++] = 0x15;
    r[pc++] = 0x40;
    u16 spin = pc;
    r[pc++] = 0xEA;
    r[pc++] = 0x4C;
    r[pc++] = (u8)(spin & 0xFF);
    r[pc++] = (u8)(spin >> 8);

    nes.pc = 0x200;
    nes.sp = 0xFD;
    nes.flags = 0x24;

    for (int i = 0; i < 20000 && g_n < 8; i++)
        step1(&nes);

    printf("intervals (want 432 after first reload):\n");
    for (int i = 1; i < g_n && i < 16; i++)
        printf("  DMA#%d->#%d: %d halt=%04X\n", i, i + 1, (int)(g_cy_at_dma[i] - g_cy_at_dma[i - 1]),
               g_halt[i]);
    return 0;
}
