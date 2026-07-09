/*
 * Real AccuracyCoin shape: open-bus catch → 26-cycle epilogue → burn 350
 * → (virtual RTS) → Clockslide_47 → LDA $4000. Want A==0 and halt==$4000.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_log;
static int g_dma_n;
static u16 g_last_halt;
static s32 g_last_dma_cy;

void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    g_dma_n++;
    g_last_halt = halt;
    g_last_dma_cy = nes->cycles - stall;
    if (g_log)
        printf("  DMA#%d halt=%04X stall=%d start=%d pc=%04X\n", g_dma_n, halt, stall,
               (int)g_last_dma_cy, nes->pc);
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

static void install_delay_sub(u8 *prg, u16 cpu_addr, int body_cycles) {
    u32 off = (u32)(cpu_addr - 0x8000);
    int nops = body_cycles / 2;
    for (int i = 0; i < nops; i++)
        prg[off++] = 0xEA;
    prg[off++] = 0x60;
}

static u16 emit_jsr(u8 *ram, u16 pc, u16 abs) {
    ram[pc++] = 0x20;
    ram[pc++] = (u8)(abs & 0xFF);
    ram[pc++] = (u8)(abs >> 8);
    return pc;
}

static u16 emit_nops(u8 *ram, u16 pc, int n) {
    for (int i = 0; i < n; i++)
        ram[pc++] = 0xEA;
    return pc;
}

static u16 emit_delay(u8 *ram, u16 pc, int n) {
    while (n >= 100) {
        pc = emit_jsr(ram, pc, 0x8100);
        n -= 100;
    }
    if (n >= 2)
        pc = emit_nops(ram, pc, n / 2);
    return pc;
}

int main(int argc, char **argv) {
    int burn = 350;
    int slide = 47;
    if (argc > 1)
        burn = atoi(argv[1]);
    if (argc > 2)
        slide = atoi(argv[2]);
    g_log = argc > 3 ? atoi(argv[3]) : 1;

    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    memset(prg + 0x7FC0, 0x00, 0x3A);
    prg[0x7FFA] = 0x00;
    prg[0x7FFB] = 0x80;
    prg[0x7FFC] = 0x00;
    prg[0x7FFD] = 0x80;
    prg[0x7FFE] = 0x00;
    prg[0x7FFF] = 0x80;
    prg[0x0000] = 0x40;
    install_delay_sub(prg, 0x8100, 88);

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.fc_step[0][0] = 7457;
    nes.fc_step[0][1] = 14916;
    nes.fc_step[0][2] = 22371;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;

    u8 *r = nes.ram;
    u16 pc = 0x0200;

    /* DMASync setup */
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

    u16 after_catch = pc;
    /* DMASync epilogue (20 cycles of instr; AccuracyCoin +RTS = 26 from DMA) */
    r[pc++] = 0xA9;
    r[pc++] = 0x0F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA5;
    r[pc++] = 0x00;
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    /* no RTS — inline burn 350 like DMASync_50 body after JSR DMASync */
    pc = emit_delay(r, pc, burn);
    /* DMASync_50's RTS is 6 cycles; we omit it and shorten burn instead if needed.
     * Real path: JSR DMASync (ends RTS) then JSR Clockslide 350 then RTS.
     * Here burn includes what Clockslide_350 does after catch epilogue. */

    pc = emit_delay(r, pc, slide);
    u16 lda_pc = pc;
    r[pc++] = 0xAD;
    r[pc++] = 0x00;
    r[pc++] = 0x40;
    u16 done = pc;
    r[pc++] = 0x4C;
    r[pc++] = (u8)(done & 0xFF);
    r[pc++] = (u8)(done >> 8);

    printf("burn=%d slide=%d lock=%04X after=%04X lda=%04X\n", burn, slide, lock, after_catch,
           lda_pc);

    nes.pc = 0x0200;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0x40;
    nes.cpu_data_bus = 0x40;
    nes.cycles = 0;
    nes.total_cycles = 0;
    g_dma_n = 0;

    int caught = 0;
    s32 cy_catch = 0;
    for (int i = 0; i < 500000; i++) {
        step1(&nes);
        if (nes.pc >= after_catch && nes.pc < 0x800 && nes.a == 0) {
            caught = 1;
            cy_catch = nes.cycles;
            break;
        }
        if (nes.pc >= 0x8000)
            break;
    }
    printf("catch %s cy=%d a=%02X halt=%04X dma_n=%d\n", caught ? "OK" : "FAIL", (int)cy_catch,
           nes.a, g_last_halt, g_dma_n);
    if (!caught)
        return 2;

    s32 t_lda = -1;
    u8 a_lda = 0xFF;
    int dma_before = g_dma_n;
    for (int i = 0; i < 200000; i++) {
        u16 p0 = nes.pc;
        if (p0 == lda_pc)
            t_lda = nes.cycles;
        step1(&nes);
        if (p0 == lda_pc) {
            a_lda = nes.a;
            break;
        }
        if (p0 == done)
            break;
    }
    printf("after catch: dmas=%d last_halt=%04X last_start=%d (catch was %d, delta=%d)\n",
           g_dma_n - dma_before, g_last_halt, (int)g_last_dma_cy, (int)cy_catch,
           (int)(g_last_dma_cy - cy_catch));
    printf("LDA A=%02X at cy=%d (rel catch %+d) %s\n", a_lda, (int)t_lda,
           (int)(t_lda - cy_catch), a_lda == 0 ? "PASS" : "FAIL");
    return a_lda == 0 ? 0 : 1;
}
