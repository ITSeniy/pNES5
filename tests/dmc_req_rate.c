#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_req;

/* We can't easily hook static dmc_request - watch pending rises */
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

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
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
    nes.dmc.bytes_left = 1;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.sample_buf = 0;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;

    int rises = 0;
    s32 last_rise = -1;
    int iv[30], n = 0;
    for (int i = 0; i < 200000 && n < 20; i++) {
        u8 p0 = nes.dmc.dma_pending;
        step1(&nes);
        if (!p0 && nes.dmc.dma_pending) {
            s32 t = nes.total_cycles;
            if (last_rise >= 0)
                iv[n++] = (int)(t - last_rise);
            last_rise = t;
            rises++;
        }
        /* auto-service if still pending at end - already done in step */
        if (nes.pc > 0x280)
            nes.pc = 0x200;
    }
    printf("pending rises=%d intervals:", rises);
    for (int i = 0; i < n; i++)
        printf(" %d", iv[i]);
    printf("\n");

    /* Dump state after 10000 cycles */
    printf("final: pending=%d full=%d bytes=%d bits=%d timer=%d silence=%d loop=%d\n",
           nes.dmc.dma_pending, nes.dmc.sample_buf_full, nes.dmc.bytes_left, nes.dmc.bits_left,
           nes.dmc.timer_count, nes.dmc.silence, nes.dmc.loop);

    /* Pure tick path: pending rises */
    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.bytes_left = 100;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xC000;
    n = 0;
    last_rise = -1;
    for (int c = 0; c < 10000; c++) {
        u8 p0 = nes.dmc.dma_pending;
        dmc_tick(&nes);
        if (!p0 && nes.dmc.dma_pending) {
            if (last_rise >= 0)
                iv[n++] = c - (int)last_rise;
            last_rise = c;
            nes.dmc.halt_addr = 0xFFFF;
            dmc_service_dma(&nes);
            if (n >= 15)
                break;
        }
    }
    printf("pure-tick+service intervals:");
    for (int i = 0; i < n; i++)
        printf(" %d", iv[i]);
    printf("\n");
    return 0;
}
