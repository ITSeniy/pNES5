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

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
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
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.bytes_left = 200;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.sample_buf = 0;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;

    s32 last_svc = -1;
    int n = 0;
    int iv[20];
    for (int i = 0; i < 50000 && n < 15; i++) {
        u8 pend0 = nes.dmc.dma_pending;
        s32 t0 = nes.total_cycles + nes.cycles;
        step1(&nes);
        if (pend0 && !nes.dmc.dma_pending && nes.dmc.sample_buf_full) {
            if (last_svc >= 0)
                iv[n++] = (int)(t0 - last_svc);
            last_svc = t0;
        }
        if (nes.pc > 0x280)
            nes.pc = 0x200;
    }
    printf("service intervals:");
    for (int i = 0; i < n; i++)
        printf(" %d", iv[i]);
    printf("\n");

    /* Tick ratio for pure NOPs */
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
    nes.dmc.enabled = 0;
    int sum_ticks = 0, sum_ran = 0;
    for (int i = 0; i < 500; i++) {
        nes.dmc.ticks_exec = 0;
        int b = nes.cycles;
        cpu_step(&nes);
        int ran = nes.cycles - b;
        int te = nes.dmc.ticks_exec;
        while (te < ran) {
            te++;
        }
        sum_ticks += te;
        sum_ran += ran;
        nes.total_cycles += ran;
        if (nes.pc > 0x280)
            nes.pc = 0x200;
    }
    printf("NOP path: cpu_cycles=%d dmc_ticks=%d ratio=%.3f\n", sum_ran, sum_ticks,
           (double)sum_ticks / sum_ran);

    /* Direct counter: how many times dmc_request would fire per 4320 ticks */
    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.bytes_left = 1000;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    int reqs = 0;
    for (int c = 0; c < 4320; c++) {
        u8 p0 = nes.dmc.dma_pending;
        dmc_tick(&nes);
        if (!p0 && nes.dmc.dma_pending) {
            reqs++;
            /* instant fill like DMA without extra ticks */
            nes.dmc.dma_pending = 0;
            nes.dmc.sample_buf_full = 1;
            nes.dmc.sample_buf = 0;
            if (nes.dmc.bytes_left > 0)
                nes.dmc.bytes_left--;
            if (nes.dmc.bytes_left == 0 && nes.dmc.loop) {
                nes.dmc.bytes_left = 1;
            }
        }
    }
    printf("requests in 4320 ticks with instant fill: %d (want 10)\n", reqs);

    /* Same but service with 4 ticks like real DMA */
    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.bytes_left = 1000;
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    reqs = 0;
    for (int c = 0; c < 4320;) {
        dmc_tick(&nes);
        c++;
        if (nes.dmc.dma_pending) {
            reqs++;
            nes.dmc.halt_addr = 0xFFFF;
            int before = c;
            /* service uses 4 dmc_ticks internally - don't double count in c? */
            s32 te0 = nes.dmc.ticks_exec;
            dmc_service_dma(&nes);
            (void)te0;
            (void)before;
            /* dmc_service_dma already called dmc_tick 4 times; those are real
             * time — advance c by 3 more since we already did 1 loop tick? */
            /* Actually service is separate from loop tick. Loop did 1 tick,
             * then service does 4 more = 5 total for this iteration. */
            c += 3; /* approximate: service's 4 ticks, 1 already counted... */
            /* This is messy — use tick budget instead */
            break;
        }
    }

    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.bytes_left = 1000;
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    reqs = 0;
    int tick = 0;
    while (tick < 4320) {
        dmc_tick(&nes);
        tick++;
        if (nes.dmc.dma_pending) {
            reqs++;
            nes.dmc.halt_addr = 0xFFFF;
            /* Manual 4-cycle DMA without calling dmc_tick inside... */
            /* Use real service which ticks 4 times */
            int t1 = tick;
            dmc_service_dma(&nes);
            /* service did 4 ticks; our tick only advanced 1 — add 3 */
            tick += 3;
            (void)t1;
        }
    }
    printf("requests in 4320 wall ticks with 4-cyc DMA: %d (want 10)\n", reqs);

    return 0;
}
