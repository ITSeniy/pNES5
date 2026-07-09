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

    /* After open-bus catch style: loop on, rate F, 1-byte, just finished a DMA */
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.bytes_left = 1;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.sample_buf = 0;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    /* Force phase: first DMA in 50 cycles */
    /* Burn 0, start ticking - request will come in ~432 */

    s32 t0 = 0;
    nes.total_cycles = 0;
    nes.cycles = 0;
    int steals = 0;
    int base_target = 30008; /* approx instr cycles to BIT */
    /* Run until total_cycles >= something large, count times ran > base opcode */
    while (nes.total_cycles < 35000) {
        s32 before = nes.total_cycles + nes.cycles;
        u8 pend = nes.dmc.dma_pending;
        int c0 = nes.cycles;
        step1(&nes);
        int ran = (int)((nes.total_cycles + nes.cycles) - before);
        /* NOP base 2; if ran > 2, had DMA steal */
        if (ran > 2)
            steals++;
        if (nes.pc > 0x280)
            nes.pc = 0x200;
        (void)pend;
        (void)c0;
        (void)t0;
    }
    printf("total_cycles=%d steals(ran>2)=%d expected_steals~%d\n",
           (int)nes.total_cycles, steals, (int)(nes.total_cycles / 432));

    /* More accurate: count dmc_service_dma by tracking sample_buf fills from empty */
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

    int services = 0;
    while (nes.total_cycles < 30008) {
        u8 full0 = nes.dmc.sample_buf_full;
        u8 pend0 = nes.dmc.dma_pending;
        s32 t0b = nes.total_cycles + nes.cycles;
        step1(&nes);
        int ran = (int)((nes.total_cycles + nes.cycles) - t0b);
        if (pend0 && !nes.dmc.dma_pending && nes.dmc.sample_buf_full && ran >= 4)
            services++;
        if (nes.pc > 0x280)
            nes.pc = 0x200;
        (void)full0;
    }
    printf("services in first 30008 wall cycles: %d (want ~69)\n", services);
    printf("final total_cycles=%d\n", (int)nes.total_cycles);
    return 0;
}
