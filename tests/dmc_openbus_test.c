/*
 * AccuracyCoin: DMASync catch + DMASync_50 residual + Open Bus T2.
 * Quiet log: only key events.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_log;
static int g_dma_n;
static u16 g_last_halt;
static int g_last_stall;
static s32 g_last_dma_cy;

void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    g_dma_n++;
    g_last_halt = halt;
    g_last_stall = stall;
    g_last_dma_cy = nes->cycles - stall;
    if (g_log)
        printf("  DMA#%d halt=%04X stall=%d start_cy=%d pc=%04X\n", g_dma_n, halt, stall,
               (int)g_last_dma_cy, nes->pc);
}

static void step1(struct NES *nes) {
    nes->dmc.ticks_exec = 0;
    int before = nes->cycles;
    cpu_step(nes);
    int ran = nes->cycles - before;
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
    if (argc > 3)
        g_log = atoi(argv[3]);

    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    /* Silent sample at $FFC0 — do NOT wipe vectors at $FFFA-FFFF */
    memset(prg + 0x7FC0, 0x00, 0x3A);
    prg[0x7FFA] = 0x00;
    prg[0x7FFB] = 0x80; /* NMI → $8000 */
    prg[0x7FFC] = 0x00;
    prg[0x7FFD] = 0x80; /* RESET */
    prg[0x7FFE] = 0x00;
    prg[0x7FFF] = 0x80; /* IRQ/BRK */
    install_delay_sub(prg, 0x8100, 88);
    /* $8000: RTI so any IRQ returns */
    prg[0x0000] = 0x40;

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

    /* DMASync setup (matches AccuracyCoin) */
    r[pc++] = 0xA9;
    r[pc++] = 0x4F; /* LDA #$4F */
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
    r[pc++] = 0x40; /* LDA $4000 */
    r[pc++] = 0xD0;
    /* pc = operand address; branch target = (pc+1)+rel */
    r[pc] = (u8)(s8)(lock - (pc + 1));
    pc++;

    u16 after_catch = pc;
    r[pc++] = 0xA9;
    r[pc++] = 0x0F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA5;
    r[pc++] = 0x00;
    r[pc++] = 0xA9;
    r[pc++] = 0x00;

    /* DMASync_50 burns 350 (AccuracyCoin). Leave 50 after virtual RTS. */
    u16 d50 = pc;
    pc = emit_delay(r, pc, burn);

    u16 body = pc; /* T0: 50 cycles until DMA in ideal model */
    pc = emit_delay(r, pc, slide);
    u16 lda_pc = pc;
    r[pc++] = 0xAD;
    r[pc++] = 0x00;
    r[pc++] = 0x40;
    u16 done_pc = pc;
    r[pc++] = 0xA9;
    r[pc++] = 0xEA; /* LDA #$EA marker */
    r[pc++] = 0x4C;
    r[pc++] = (u8)(done_pc & 0xFF);
    r[pc++] = (u8)(done_pc >> 8); /* JMP self */

    printf("burn=%d slide=%d lock=%04X after=%04X d50=%04X body=%04X lda=%04X\n", burn, slide,
           lock, after_catch, d50, body, lda_pc);

    nes.pc = 0x0200;
    nes.sp = 0xFD;
    nes.flags = 0x24; /* I set */
    nes.a = 0x40;
    nes.cpu_data_bus = 0x40;
    nes.cycles = 0;
    nes.total_cycles = 0;
    g_log = (argc > 3) ? atoi(argv[3]) : 0;
    g_dma_n = 0;

    /* Run until exit lock (pc >= after_catch with A=0 from sample on bus) */
    int caught = 0;
    for (int i = 0; i < 200000; i++) {
        step1(&nes);
        if (nes.pc >= after_catch && nes.pc < 0x800 && nes.a == 0) {
            caught = 1;
            break;
        }
        if (nes.pc >= 0x8000)
            break;
    }
    printf("catch: %s pc=%04X a=%02X bus=%02X cy=%d dma_n=%d last_halt=%04X\n",
           caught ? "OK" : "FAIL", nes.pc, nes.a, nes.cpu_data_bus, (int)nes.cycles, g_dma_n,
           g_last_halt);

    if (!caught)
        return 2;

    s32 cy_after_catch = nes.cycles;
    s32 t_body = -1;
    s32 t_lda = -1;
    u8 a_lda = 0xFF;

    g_log = (argc > 3) ? atoi(argv[3]) : 1;
    for (int i = 0; i < 100000; i++) {
        u16 p0 = nes.pc;
        if (p0 == body && t_body < 0)
            t_body = nes.cycles;
        if (p0 == lda_pc && t_lda < 0)
            t_lda = nes.cycles;
        step1(&nes);
        if (p0 == lda_pc) {
            a_lda = nes.a;
            break;
        }
        if (p0 == done_pc)
            break;
    }

    s32 dma_rel_body =
        (t_body >= 0 && g_last_dma_cy >= 0) ? (g_last_dma_cy - t_body) : -99999;
    printf("body_cy=%d lda_cy=%d delta=%d\n", (int)t_body, (int)t_lda,
           (int)(t_lda - t_body));
    printf("last_DMA start=%d halt=%04X stall=%d rel_body=%+d (want ~%d for openbus)\n",
           (int)g_last_dma_cy, g_last_halt, g_last_stall, (int)dma_rel_body, slide + 3);
    printf("LDA $4000 A=%02X %s (want 00), dma_total=%d, catch_to_now=%d\n", a_lda,
           a_lda == 0 ? "PASS" : "FAIL", g_dma_n, (int)(nes.cycles - cy_after_catch));

    return a_lda == 0 ? 0 : 1;
}
