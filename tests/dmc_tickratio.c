#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
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
    nes.dmc.bytes_left = 10;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xC000;

    int sum_te = 0, sum_ran = 0, sum_pad = 0, reqs = 0, svcs = 0;
    for (int i = 0; i < 5000; i++) {
        nes.dmc.ticks_exec = 0;
        int b = nes.cycles;
        u8 p0 = nes.dmc.dma_pending;
        u8 f0 = nes.dmc.sample_buf_full;
        cpu_step(&nes);
        int ran = nes.cycles - b;
        int te = nes.dmc.ticks_exec;
        int pad = 0;
        while (nes.dmc.ticks_exec < ran) {
            dmc_tick(&nes);
            pad++;
        }
        if (!p0 && nes.dmc.dma_pending)
            reqs++;
        if (p0 && !nes.dmc.dma_pending && nes.dmc.sample_buf_full && !f0)
            svcs++;
        if (p0 && ran >= 4)
            svcs++; /* count service by steal */
        sum_te += te;
        sum_ran += ran;
        sum_pad += pad;
        nes.total_cycles += ran;
        if (nes.pc > 0x280)
            nes.pc = 0x200;
    }
    printf("sum_ran=%d bus_ticks=%d pad=%d total_dmc=%d\n", sum_ran, sum_te, sum_pad, sum_te + sum_pad);
    printf("req_rises=%d steal_svcs=%d\n", reqs, svcs);
    printf("final bits=%d timer=%d full=%d bytes=%d pending=%d silence=%d\n", nes.dmc.bits_left,
           nes.dmc.timer_count, nes.dmc.sample_buf_full, nes.dmc.bytes_left, nes.dmc.dma_pending,
           nes.dmc.silence);
    return 0;
}
