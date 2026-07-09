#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_n;
static s32 g_st;
static u16 g_h;

void dmc_phase_log_service(struct NES *n, u16 h, int s) {
    g_n++;
    g_h = h;
    g_st = n->cycles - s;
    printf("DMA#%d halt=%04X start=%d pc=%04X\n", g_n, h, (int)g_st, n->pc);
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
    {
        u32 o = 0x100;
        for (int i = 0; i < 44; i++)
            prg[o++] = 0xEA;
        prg[o++] = 0x60;
    }

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

    u8 *r = nes.ram;
    u16 pc = 0x200;
    r[pc++] = 0xA9;
    r[pc++] = 0x4F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    r[pc++] = 0x8D;
    r[pc++] = 0x11;
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
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    u16 lock = pc;
    r[pc++] = 0xAD;
    r[pc++] = 0x00;
    r[pc++] = 0x40;
    r[pc++] = 0xD0;
    r[pc] = (u8)(s8)(lock - (pc + 1));
    pc++;
    u16 after = pc;
    /* epilogue 20 + virtual RTS 6 */
    r[pc++] = 0xA9;
    r[pc++] = 0x0F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA5;
    r[pc++] = 0x00;
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    /* burn 350 */
    for (int i = 0; i < 3; i++) {
        r[pc++] = 0x20;
        r[pc++] = 0x00;
        r[pc++] = 0x81;
    }
    for (int i = 0; i < 25; i++)
        r[pc++] = 0xEA;
    /* DMASync_50 RTS */
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    /* Clockslide_47 */
    for (int i = 0; i < 22; i++)
        r[pc++] = 0xEA;
    r[pc++] = 0xA5;
    r[pc++] = 0x00;
    u16 lda = pc;
    r[pc++] = 0xAD;
    r[pc++] = 0x00;
    r[pc++] = 0x40;
    u16 done = pc;
    r[pc++] = 0x4C;
    r[pc++] = (u8)(done & 0xFF);
    r[pc++] = (u8)(done >> 8);

    nes.pc = 0x200;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0x40;
    nes.cpu_data_bus = 0x40;

    int caught = 0;
    s32 c0 = 0;
    for (int i = 0; i < 500000; i++) {
        u16 p0 = nes.pc;
        step1(&nes);
        if (!caught && nes.pc >= after && nes.pc < 0x800 && nes.a == 0) {
            caught = 1;
            c0 = g_st;
            printf("CATCH halt=%04X dma_start=%d after_cy=%d delta=%d\n", g_h, (int)c0,
                   (int)nes.cycles, (int)(nes.cycles - c0));
        }
        if (caught && p0 == lda) {
            printf("LDA enter cy=%d rel=%d pend=%d bits=%d tc=%d bus=%02X\n", (int)nes.cycles,
                   (int)(nes.cycles - c0), nes.dmc.dma_pending, nes.dmc.bits_left,
                   nes.dmc.timer_count, nes.cpu_data_bus);
        }
        if (caught && p0 == done) {
            printf("A=%02X last_halt=%04X rel_dma=%d total_dma=%d\n", nes.a, g_h, (int)(g_st - c0),
                   g_n);
            printf("%s\n", (nes.a == 0 && g_h == 0x4000) ? "PASS" : "FAIL");
            return nes.a == 0 ? 0 : 1;
        }
    }
    printf("timeout caught=%d\n", caught);
    return 2;
}
