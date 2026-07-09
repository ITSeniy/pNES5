#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

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
    /* zeros for sample at $FFC0 */
    memset(prg + 0x7FC0, 0x00, 64);

    /*
     * DMASync-like loop at $8000:
     * AD 00 40   LDA $4000
     * D0 FB      BNE $8000
     * 00         BRK
     */
    prg[0] = 0xAD;
    prg[1] = 0x00;
    prg[2] = 0x40;
    prg[3] = 0xD0;
    prg[4] = 0xFB;
    prg[5] = 0x00;

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
    nes.pc = 0x8000;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0x40;
    nes.cpu_data_bus = 0x40;

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
    nes.cpu_data_bus = 0x40;
    nes.a = 0x40;

    s32 last = -1;
    int n = 0, iv[20];
    int svc = 0;
    s32 last_svc = -1;
    int siv[20], sn = 0;
    for (int i = 0; i < 500000 && n < 12; i++) {
        u8 a0 = nes.a;
        u8 pend0 = nes.dmc.dma_pending;
        s32 t0 = nes.total_cycles;
        int c0 = nes.cycles;
        u16 pc0 = nes.pc;
        step1(&nes);
        int ran = nes.cycles - c0;
        if (pend0 && ran >= 5) {
            if (last_svc >= 0 && sn < 20)
                siv[sn++] = (int)(t0 - last_svc);
            last_svc = t0;
            svc++;
        }
        if (a0 != 0 && nes.a == 0) {
            s32 t = nes.total_cycles;
            if (last >= 0)
                iv[n++] = (int)(t - last);
            last = t;
            /* Like DMASync after catch: keep looping with loop on for measurement */
            nes.a = 0x40;
            nes.cpu_data_bus = 0x40;
            nes.pc = 0x8000;
            if (!nes.dmc.loop)
                nes.dmc.loop = 1;
            if (nes.dmc.bytes_left == 0) {
                nes.dmc.bytes_left = 1;
                nes.dmc.cur_addr = 0xFFC0;
            }
        }
        /* If stuck at BRK, restart loop */
        if (nes.pc == 0x8005 || nes.pc == 0x8006) {
            nes.pc = 0x8000;
            nes.a = 0x40;
            nes.cpu_data_bus = 0x40;
        }
        (void)pc0;
    }
    printf("DMA services detected=%d intervals:", svc);
    for (int i = 0; i < sn; i++)
        printf(" %d", siv[i]);
    printf("\n");
    printf("DMASync-like catch intervals:");
    int sum = 0;
    for (int i = 0; i < n; i++) {
        printf(" %d", iv[i]);
        sum += iv[i];
    }
    printf("\nn=%d avg=%.1f want 432\n", n, n ? (double)sum / n : 0);
    return 0;
}
