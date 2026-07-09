#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static void init_ntsc(struct NES *nes) {
    nes->cpu_freq = 1789773;
    nes->fc_step[0][0] = 7457;
    nes->fc_step[0][1] = 14916;
    nes->fc_step[0][2] = 22371;
    nes->fc_step[0][3] = 29831;
    nes->fc_step[0][4] = 29832;
    nes->fc_step[0][5] = 29833;
}

static int step1(struct NES *nes) {
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
    return ran;
}

/* Global to count real services */
static s32 g_last_svc = -1;
static int g_n, g_iv[64];
static int g_svc_count;

/* We'll detect service by wrapping: check cycles jump from DMA */
int main(void) {
    u8 prg[0x8000];
    memset(prg, 0, sizeof(prg));
    /* zeros at FFC0 region in 32k map: offset 0x7FC0 */
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
    memset(nes.ram, 0xEA, sizeof(nes.ram));
    nes.pc = 0x200;
    nes.sp = 0xFD;
    nes.flags = 0x24;

    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.bytes_left = 1;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.halt_addr = 0xFFFF;
    dmc_service_dma(&nes);

    /* Run many NOPs; record cycle time of each DMA service via cycles delta */
    s32 prev_tot = nes.total_cycles;
    int prev_cycles_field = 0;
    g_n = 0;
    for (int i = 0; i < 100000 && g_n < 40; i++) {
        s32 t0 = nes.total_cycles + nes.cycles;
        int c0 = nes.cycles;
        u8 pend = nes.dmc.dma_pending;
        step1(&nes);
        int ran = (int)((nes.total_cycles + nes.cycles) - t0);
        /* Service happened if we had pending and ran includes stall (>=4 extra) */
        if (pend && ran >= 5) {
            s32 tstart = t0;
            if (g_last_svc >= 0)
                g_iv[g_n++] = (int)(tstart - g_last_svc);
            g_last_svc = tstart;
            g_svc_count++;
        }
        if (nes.pc > 0x280)
            nes.pc = 0x200;
        (void)c0;
        (void)prev_tot;
        (void)prev_cycles_field;
    }
    printf("NOP full-path DMA intervals (n=%d):", g_n);
    int min = 9999, max = 0, sum = 0;
    for (int i = 0; i < g_n; i++) {
        printf(" %d", g_iv[i]);
        if (g_iv[i] < min) min = g_iv[i];
        if (g_iv[i] > max) max = g_iv[i];
        sum += g_iv[i];
    }
    printf("\nmin=%d max=%d avg=%.2f\n", min, max, g_n ? (double)sum / g_n : 0);

    /* Simulate open-bus catch loop: LDA $4000 in a tight loop in RAM
     * A5 style: AD 00 40 = LDA $4000, D0 FD = BNE -3? need relative
     * Loop: LDA $4000 / BNE loop
     */
    memset(nes.ram, 0xEA, sizeof(nes.ram));
    /* AD 00 40  LDA $4000
     * D0 FB     BNE -5 (to LDA)
     */
    nes.ram[0x200] = 0xAD;
    nes.ram[0x201] = 0x00;
    nes.ram[0x202] = 0x40;
    nes.ram[0x203] = 0xD0;
    nes.ram[0x204] = 0xFB;
    nes.pc = 0x200;
    nes.total_cycles = 0;
    nes.cycles = 0;
    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.bytes_left = 1;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.halt_addr = 0xFFFF;
    dmc_service_dma(&nes);
    nes.cpu_data_bus = 0x40;
    nes.a = 0x40;

    int catches = 0;
    s32 last_catch = -1;
    int civ[20];
    int cn = 0;
    for (int i = 0; i < 200000 && cn < 15; i++) {
        u8 a0 = nes.a;
        step1(&nes);
        /* After LDA that got 0, A becomes 0 and BNE not taken - pc advances */
        if (a0 != 0 && nes.a == 0) {
            s32 t = nes.total_cycles;
            if (last_catch >= 0)
                civ[cn++] = (int)(t - last_catch);
            last_catch = t;
            catches++;
            /* restore loop: set A nonzero and pc back - actually DMASync sets $0F after first catch.
             * For continuous: re-enable loop and force A=$40, pc=200 */
            nes.a = 0x40;
            nes.cpu_data_bus = 0x40;
            nes.pc = 0x200;
            nes.dmc.loop = 1;
            if (nes.dmc.bytes_left == 0) {
                nes.dmc.bytes_left = 1;
                nes.dmc.cur_addr = 0xFFC0;
            }
        }
        if (nes.pc > 0x210)
            nes.pc = 0x200;
    }
    printf("open-bus catch intervals (n=%d):", cn);
    min = 9999;
    max = 0;
    sum = 0;
    for (int i = 0; i < cn; i++) {
        printf(" %d", civ[i]);
        if (civ[i] < min) min = civ[i];
        if (civ[i] > max) max = civ[i];
        sum += civ[i];
    }
    printf("\nmin=%d max=%d avg=%.2f (want ~432)\n", min, max, cn ? (double)sum / cn : 0);

    return 0;
}
