#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

int main(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.prg_size = 0;
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.bytes_left = 500;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.sample_buf = 0;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;

    int last = -1;
    int n = 0;
    int iv[16];
    /* Each loop iteration = exactly 1 CPU cycle = 1 dmc_tick.
     * When pending, run DMA service (which itself dmc_ticks) without
     * extra outer ticks — service's ticks ARE the wall cycles. */
    for (int cycle = 0; cycle < 20000 && n < 12;) {
        if (nes.dmc.dma_pending && !nes.dmc.dma_reentry) {
            if (last >= 0)
                iv[n++] = cycle - last;
            last = cycle;
            nes.dmc.halt_addr = 0xFFFF;
            int c0 = cycle;
            /* dmc_service_dma adds nes->cycles and calls dmc_tick stall times */
            s32 cy0 = nes.cycles;
            dmc_service_dma(&nes);
            int stall = (int)(nes.cycles - cy0);
            cycle += stall > 0 ? stall : 4;
            (void)c0;
            continue;
        }
        dmc_tick(&nes);
        cycle++;
    }
    printf("wall-cycle DMA start intervals:");
    for (int i = 0; i < n; i++)
        printf(" %d", iv[i]);
    printf("\n");

    /* Variant: do NOT tick DMC during DMA stall (freeze output during DMA) */
    memset(&nes, 0, sizeof(nes));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.bytes_left = 500;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    last = -1;
    n = 0;
    for (int cycle = 0; cycle < 20000 && n < 12;) {
        if (nes.dmc.dma_pending && !nes.dmc.dma_reentry) {
            if (last >= 0)
                iv[n++] = cycle - last;
            last = cycle;
            /* freeze-style DMA: 4 cycles, fill buffer, no dmc_tick */
            nes.dmc.dma_pending = 0;
            nes.dmc.sample_buf = 0;
            nes.dmc.sample_buf_full = 1;
            nes.dmc.bytes_left--;
            if (nes.dmc.bytes_left == 0 && nes.dmc.loop)
                nes.dmc.bytes_left = 1;
            cycle += 4;
            continue;
        }
        dmc_tick(&nes);
        cycle++;
    }
    printf("freeze-DMA intervals (want 432):");
    for (int i = 0; i < n; i++)
        printf(" %d", iv[i]);
    printf("\n");

    return 0;
}
