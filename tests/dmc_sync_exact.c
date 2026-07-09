/*
 * Exact DMASync_50 residual: after catch, 26-cycle epilogue residual math
 * says next DMA at +432 from catch get; after 350+6, 50 cycles remain.
 */
#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_dma_n;
static u16 g_halt;
static s32 g_start;
static int g_log;

void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    g_dma_n++;
    g_halt = halt;
    g_start = nes->cycles - stall;
    if (g_log)
        printf("DMA#%d halt=%04X stall=%d start=%d pc=%04X bits=%d full=%d bl=%d tc=%d\n",
               g_dma_n, halt, stall, (int)g_start, nes->pc, nes->dmc.bits_left,
               nes->dmc.sample_buf_full, (int)nes->dmc.bytes_left, nes->dmc.timer_count);
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

static void delay_sub(u8 *prg, u16 a, int body) {
    u32 o = (u32)(a - 0x8000);
    for (int i = 0; i < body / 2; i++)
        prg[o++] = 0xEA;
    prg[o++] = 0x60;
}

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    memset(prg + 0x7FC0, 0, 0x3A);
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

    u16 after = pc;
    /* 20-cycle epilogue + 6-cycle virtual RTS = 26 from after DMA */
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
    r[pc++] = 0xEA; /* +6 like RTS */

    /* burn 350: 3x JSR $8100 (100) + 25 NOPs (50) */
    for (int i = 0; i < 3; i++) {
        r[pc++] = 0x20;
        r[pc++] = 0x00;
        r[pc++] = 0x81;
    }
    for (int i = 0; i < 25; i++)
        r[pc++] = 0xEA;

    /* DMASync_50 RTS +6 */
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;

    /* Clockslide_47: 23 NOPs + LDA zp = 46+3=49 — use 22 NOPs + LDA zp = 44+3=47 */
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
    s32 cy_catch = 0, cy_dma_catch = 0;
    int dma_at_catch = 0;

    for (int i = 0; i < 500000; i++) {
        u16 p0 = nes.pc;
        step1(&nes);
        if (!caught && nes.pc >= after && nes.pc < 0x800 && nes.a == 0) {
            caught = 1;
            cy_catch = nes.cycles;
            cy_dma_catch = g_start;
            dma_at_catch = g_dma_n;
            g_log = 1;
            printf("CATCH cy=%d dma_start=%d a=%02X bits=%d full=%d bl=%d tc=%d loop=%d en=%d\n",
                   (int)cy_catch, (int)cy_dma_catch, nes.a, nes.dmc.bits_left,
                   nes.dmc.sample_buf_full, (int)nes.dmc.bytes_left, nes.dmc.timer_count,
                   nes.dmc.loop, nes.dmc.enabled);
        }
        if (caught && p0 == lda) {
            printf("enter LDA cy=%d rel_catch=%+d rel_dma=%+d pend=%d bits=%d full=%d tc=%d bus=%02X\n",
                   (int)nes.cycles, (int)(nes.cycles - cy_catch),
                   (int)(nes.cycles - cy_dma_catch), nes.dmc.dma_pending, nes.dmc.bits_left,
                   nes.dmc.sample_buf_full, nes.dmc.timer_count, nes.cpu_data_bus);
        }
        if (caught && p0 == done) {
            printf("LDA A=%02X (want 00) dmas_after=%d last_halt=%04X last_start=%d "
                   "rel_from_catch_dma=%+d\n",
                   nes.a, g_dma_n - dma_at_catch, g_halt, (int)g_start,
                   (int)(g_start - cy_dma_catch));
            printf("%s\n", nes.a == 0 && g_halt == 0x4000 ? "PASS" : "FAIL");
            return nes.a == 0 ? 0 : 1;
        }
        if (nes.pc >= 0x8000)
            break;
    }
    printf("did not finish caught=%d\n", caught);
    return 2;
}
