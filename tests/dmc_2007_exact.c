/* DMASync_50 + Clockslide_47 + LDA $2007 — expect A>=3 (dummy advances). */
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
    (void)n;
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
    nes.mirror = 1;
    nes.noise.shift_reg = 1;
    nes.audio_handle = -1;
    nes.fc_step[0][0] = 7457;
    nes.fc_step[0][1] = 14916;
    nes.fc_step[0][2] = 22371;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;
    for (u8 i = 0; i < 8; i++)
        nes.vram[i] = i;
    /* Prep like AccuracyCoin: buffer seeded, v at $2001 */
    nes.read_buf = 0;
    nes.vram_addr = 0x2001;

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
    for (int i = 0; i < 3; i++) {
        r[pc++] = 0x20;
        r[pc++] = 0x00;
        r[pc++] = 0x81;
    }
    for (int i = 0; i < 25; i++)
        r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    for (int i = 0; i < 22; i++)
        r[pc++] = 0xEA;
    r[pc++] = 0xA5;
    r[pc++] = 0x00;
    u16 lda = pc;
    r[pc++] = 0xAD;
    r[pc++] = 0x07;
    r[pc++] = 0x20;
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
    for (int i = 0; i < 500000; i++) {
        u16 p0 = nes.pc;
        step1(&nes);
        if (!caught && nes.pc >= after && nes.pc < 0x800 && nes.a == 0)
            caught = 1;
        if (caught && p0 == done) {
            printf("A=%02X halt=%04X v=%04X %s\n", nes.a, g_h, nes.vram_addr,
                   (nes.a >= 3 && g_h == 0x2007) ? "PASS" : "FAIL");
            return (nes.a >= 3) ? 0 : 1;
        }
        if (p0 == lda)
            (void)lda;
    }
    printf("timeout\n");
    return 2;
}
