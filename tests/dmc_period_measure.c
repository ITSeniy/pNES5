/* Measure DMC reload interval at rate F. */
#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static s32 g_t[64];
static int g_n;
static int g_stall[64];

void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    if (g_n < 64) {
        g_t[g_n] = nes->cycles - stall;
        g_stall[g_n] = stall;
        g_n++;
    }
    (void)halt;
}

static void step1(struct NES *nes) {
    nes->dmc.ticks_exec = 0;
    int b = nes->cycles;
    cpu_step(nes);
    int ran = nes->cycles - b;
    if (ran > 0) {
        while (nes->dmc.ticks_exec < ran)
            dmc_tick(nes);
        apu_step(nes, ran);
        nes->total_cycles += ran;
    }
}

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    memset(prg + 0x7FC0, 0, 64);
    prg[0x7FFC] = 0;
    prg[0x7FFD] = 0x80;

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
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
    r[pc++] = 0x10; /* length 0x10*16+1 = 257 bytes — long stream */
    r[pc++] = 0x8D;
    r[pc++] = 0x13;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0x10;
    r[pc++] = 0x8D;
    r[pc++] = 0x15;
    r[pc++] = 0x40;
    u16 loop = pc;
    r[pc++] = 0xEA;
    r[pc++] = 0x4C;
    r[pc++] = (u8)(loop & 0xFF);
    r[pc++] = (u8)(loop >> 8);

    nes.pc = 0x200;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.cycles = 0;

    for (int i = 0; i < 100000 && g_n < 40; i++)
        step1(&nes);

    printf("n=%d\n", g_n);
    for (int i = 0; i < g_n && i < 20; i++) {
        if (i == 0)
            printf("t0=%d stall=%d\n", (int)g_t[0], g_stall[0]);
        else
            printf("t=%d stall=%d iv=%d\n", (int)g_t[i], g_stall[i],
                   (int)(g_t[i] - g_t[i - 1]));
    }
    return 0;
}
