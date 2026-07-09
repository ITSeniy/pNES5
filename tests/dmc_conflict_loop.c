/*
 * Timed approximation of AccuracyCoin Bus Conflicts test 2 loop:
 * after setup, LDA $4000 every 432 cycles for 0x40 samples.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_n;
static u16 g_h;
static s32 g_st;

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

static void delay_sub(u8 *prg, u16 a, int body) {
    u32 o = (u32)(a - 0x8000);
    for (int i = 0; i < body / 2; i++)
        prg[o++] = 0xEA;
    prg[o++] = 0x60;
}

static u16 emit_delay(u8 *ram, u16 pc, int n) {
    while (n >= 100) {
        ram[pc++] = 0x20;
        ram[pc++] = 0x00;
        ram[pc++] = 0x81;
        n -= 100;
    }
    if (n >= 2) {
        for (int i = 0; i < n / 2; i++)
            ram[pc++] = 0xEA;
    }
    return pc;
}

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    /* Put AccuracyCoin-like pattern at $EFC0 */
    memset(prg + 0x6FC0, 0x00, 32);
    memset(prg + 0x6FE0, 0xFF, 32);
    /* silent at FFC0 for DMASync catch */
    memset(prg + 0x7FC0, 0x00, 0x3A);
    prg[0x7FFC] = 0;
    prg[0x7FFD] = 0x80;
    prg[0] = 0x40;
    delay_sub(prg, 0x8100, 88);

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
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

    /* DMASync catch */
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
    /* epilogue + RTS-ish */
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
    /* burn 350 like DMASync_50 */
    pc = emit_delay(r, pc, 350);
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;

    /* Reconfigure like test 2 (from 50 cycles remaining) */
    r[pc++] = 0xA9;
    r[pc++] = 0x04; /* LDA #4 */
    r[pc++] = 0x8D;
    r[pc++] = 0x13;
    r[pc++] = 0x40; /* STA $4013 */
    r[pc++] = 0xA9;
    r[pc++] = 0xBF;
    r[pc++] = 0x8D;
    r[pc++] = 0x12;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0x4F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA2;
    r[pc++] = 0x00; /* LDX #0 */
    /* 50 - (2+4+2+4+2+4+2) = 30 left — Clockslide_30 */
    pc = emit_delay(r, pc, 30);
    /* reload DMA happens here ideally */
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    r[pc++] = 0x8D;
    r[pc++] = 0x17;
    r[pc++] = 0x40; /* STA $4017 */
    pc = emit_delay(r, pc, 418); /* 419-ish; use 418 even NOPs + pad */
    r[pc++] = 0xEA; /* +2 → closer to 420; tune if needed */

    u16 loop = pc;
    r[pc++] = 0xAD;
    r[pc++] = 0x00;
    r[pc++] = 0x40; /* LDA $4000 */
    r[pc++] = 0x95;
    r[pc++] = 0x00; /* STA $00,X  — use zp page as $500 substitute via $00,X in zero page... */
    /* Actually store to $500,X */
    pc -= 2;
    r[pc++] = 0x9D;
    r[pc++] = 0x00;
    r[pc++] = 0x05; /* STA $500,X */
    pc = emit_delay(r, pc, 400);
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    r[pc++] = 0x8D;
    r[pc++] = 0x17;
    r[pc++] = 0x40;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xE8; /* INX */
    r[pc++] = 0xE0;
    r[pc++] = 0x40; /* CPX #$40 */
    r[pc++] = 0xD0;
    r[pc] = (u8)(s8)(loop - (pc + 1));
    pc++;
    u16 done = pc;
    r[pc++] = 0x4C;
    r[pc++] = (u8)(done & 0xFF);
    r[pc++] = (u8)(done >> 8);

    nes.pc = 0x200;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0x40;
    nes.cpu_data_bus = 0x40;
    nes.frame_irq_flag = 1;

    int caught = 0;
    for (int i = 0; i < 2000000; i++) {
        step1(&nes);
        if (!caught && nes.pc >= after && nes.pc < 0x800 && nes.a == 0) {
            caught = 1;
            printf("catch ok cy=%d\n", (int)nes.cycles);
        }
        if (caught && nes.pc == done)
            break;
        if (nes.pc >= 0x8000)
            break;
    }

    printf("done X=%02X dmas=%d last_halt=%04X\n", nes.x, g_n, g_h);
    printf("$500:\n");
    int fails = 0;
    for (int x = 0; x < 0x40; x++) {
        u8 v = nes.ram[0x500 + x];
        printf("%02X%s", v, ((x & 15) == 15) ? "\n" : " ");
        int lo = x & 0x1F;
        if (lo == 0x16 || lo == 0x17)
            continue;
        u8 want = (x < 0x20) ? 0x00 : 0xFF;
        if (v != want)
            fails++;
    }
    printf("mismatches=%d cur_addr=%04X bl=%d\n", fails, nes.dmc.cur_addr, (int)nes.dmc.bytes_left);
    return fails ? 1 : 0;
}
