#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static void init_ntsc(struct NES *nes) {
    nes->is_pal = 0;
    nes->cpu_freq = 1789773;
    nes->fc_step[0][0] = 7457;  nes->fc_step[0][1] = 14916;
    nes->fc_step[0][2] = 22371; nes->fc_step[0][3] = 29831;
    nes->fc_step[0][4] = 29832; nes->fc_step[0][5] = 29833;
    nes->fc_step[1][0] = 7457;  nes->fc_step[1][1] = 14916;
    nes->fc_step[1][2] = 22371; nes->fc_step[1][3] = 29829;
    nes->fc_step[1][4] = 37284; nes->fc_step[1][5] = 37285;
}

/* Same DMC pad path as ppu step_cpu_apu */
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
    FILE *f = fopen("roms/AccuracyCoin.nes", "rb");
    if (!f) {
        perror("roms/AccuracyCoin.nes");
        return 1;
    }
    u8 hdr[16];
    if (fread(hdr, 1, 16, f) != 16) {
        fclose(f);
        return 1;
    }
    int prg_size = hdr[4] * 0x4000;
    u8 *prg = (u8 *)malloc((size_t)prg_size);
    if (!prg || (int)fread(prg, 1, (size_t)prg_size, f) != prg_size) {
        fclose(f);
        free(prg);
        return 1;
    }
    fclose(f);

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    nes.prg = prg;
    nes.prg_size = prg_size;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;

    /* --- Interval between reload DMA requests (timer only) --- */
    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15; /* rate F → 54*8 = 432 */
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.bytes_left = 100;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.sample_buf = 0;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;

    int last = -1;
    int ni = 0;
    int intervals[16];
    for (int c = 0; c < 8000 && ni < 12; c++) {
        dmc_tick(&nes);
        if (nes.dmc.dma_pending) {
            if (last >= 0)
                intervals[ni++] = c - last;
            last = c;
            nes.dmc.halt_addr = 0xFFFF;
            dmc_service_dma(&nes);
        }
    }
    printf("timer-only DMA intervals (want 432):");
    for (int i = 0; i < ni; i++)
        printf(" %d", intervals[i]);
    printf("\n");

    /* --- Rate E: 72*8 = 576 --- */
    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 14;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.bytes_left = 100;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    last = -1;
    ni = 0;
    for (int c = 0; c < 10000 && ni < 8; c++) {
        dmc_tick(&nes);
        if (nes.dmc.dma_pending) {
            if (last >= 0)
                intervals[ni++] = c - last;
            last = c;
            nes.dmc.halt_addr = 0xFFFF;
            dmc_service_dma(&nes);
        }
    }
    printf("timer-only rate E intervals (want 576):");
    for (int i = 0; i < ni; i++)
        printf(" %d", intervals[i]);
    printf("\n");

    /* --- Full CPU path: NOP loop measuring open-bus DMA catches --- */
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    nes.prg = prg;
    nes.prg_size = prg_size;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
    /* Fill RAM with NOP; PC runs in RAM */
    memset(nes.ram, 0xEA, sizeof(nes.ram));
    nes.pc = 0x0200;
    nes.sp = 0xFD;
    nes.flags = 0x24;

    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.bytes_left = 50;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.halt_addr = 0xFFFF;
    dmc_service_dma(&nes);

    last = -1;
    ni = 0;
    for (int i = 0; i < 20000 && ni < 10; i++) {
        s32 t0 = nes.total_cycles + nes.cycles;
        /* LDA $4000 from open bus probe via direct cpu_read sequence would
         * desync vs full step — instead poll after each step. */
        step1(&nes);
        if (nes.dmc.dma_pending) {
            /* will service on next memory access inside step */
        }
        /* Detect sample on bus after a step that serviced DMA */
        (void)t0;
    }
    /* Measure by instrumenting: count cycles between dma_pending rises */
    memset(&nes.dmc, 0, sizeof(nes.dmc));
    nes.dmc.enabled = 1;
    nes.dmc.loop = 1;
    nes.dmc.period_idx = 15;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.bytes_left = 50;
    nes.dmc.sample_buf_full = 1;
    nes.dmc.bits_left = 0;
    nes.dmc.timer_count = 0;
    nes.pc = 0x0200;
    nes.cycles = 0;
    nes.total_cycles = 0;
    last = -1;
    ni = 0;
    {
        int services = 0;
        s32 prev_tot = -1;
        for (int i = 0; i < 40000 && ni < 12; i++) {
            s32 t_before = nes.total_cycles + nes.cycles;
            u8 pend_before = nes.dmc.dma_pending;
            u8 full_before = nes.dmc.sample_buf_full;
            step1(&nes);
            /* Serviced if pending cleared and buffer became full, or cycles jumped */
            s32 t_after = nes.total_cycles + nes.cycles;
            int ran = (int)(t_after - t_before);
            if (pend_before && !nes.dmc.dma_pending && nes.dmc.sample_buf_full && ran >= 4) {
                if (prev_tot >= 0)
                    intervals[ni++] = (int)(t_before - prev_tot);
                prev_tot = t_before;
                services++;
            }
            (void)full_before;
        }
        printf("CPU-step DMA service intervals (n=%d):", services);
        for (int i = 0; i < ni; i++)
            printf(" %d", intervals[i]);
        printf("\n");
    }

    /* --- Emulate AccuracyCoin $4015 test 2 timing skeleton --- */
    {
        memset(&nes, 0, sizeof(nes));
        init_ntsc(&nes);
        nes.prg = prg;
        nes.prg_size = prg_size;
        nes.mapper = 0;
        nes.chr_enable = 1;
        nes.audio_handle = -1;
        nes.noise.shift_reg = 1;
        memset(nes.ram, 0xEA, sizeof(nes.ram));
        /* Program at $0200:
         * A9 4F    LDA #$4F
         * 8D 10 40 STA $4010
         * A9 00    LDA #$00
         * 8D 17 40 STA $4017
         * then we cycle-skip, then
         * 2C 15 40 BIT $4015
         */
        u8 *p = nes.ram + 0x200;
        p[0]=0xA9; p[1]=0x4F;
        p[2]=0x8D; p[3]=0x10; p[4]=0x40;
        p[5]=0xA9; p[6]=0x00;
        p[7]=0x8D; p[8]=0x17; p[9]=0x40;
        /* pad with NOPs then BIT $4015 */
        int off = 10;
        for (int i = 0; i < 200; i++)
            p[off++] = 0xEA;
        p[off++]=0x2C; p[off++]=0x15; p[off++]=0x40; /* BIT $4015 */
        p[off++]=0x00; /* BRK stop */

        nes.pc = 0x0200;
        nes.sp = 0xFD;
        nes.flags = 0x24; /* I set like SEI */

        /* Pre-sync: one DMA, then set like after DMASync_50 (50 cyc to next) */
        nes.dmc.enabled = 1;
        nes.dmc.loop = 1;
        nes.dmc.period_idx = 15;
        nes.dmc.sample_addr = 0xFFC0;
        nes.dmc.sample_len = 1;
        nes.dmc.cur_addr = 0xFFC0;
        nes.dmc.bytes_left = 1;
        nes.dmc.sample_buf_full = 0;
        nes.dmc.bits_left = 1;
        nes.dmc.timer_count = 50; /* crude: fire soon */
        nes.dmc.dma_pending = 0;
        nes.frame_irq_inhibit = 0;
        nes.frame_mode = 0;
        nes.frame_counter = 29830; /* IRQ window */

        printf("4015 diag: starting PC=%04X frame_irq=%d\n", nes.pc, nes.frame_irq_flag);
        for (int i = 0; i < 5000; i++) {
            u16 pc0 = nes.pc;
            step1(&nes);
            if (pc0 == 0x0200 + off - 3) {
                /* about to execute BIT or just did */
            }
            if (nes.pc > 0x0200 + off - 3 && nes.pc <= 0x0200 + off) {
                printf("after BIT area: pc=%04X V=%d frame_irq=%d A=%02X\n",
                       nes.pc, (nes.flags >> 6) & 1, nes.frame_irq_flag, nes.a);
                break;
            }
            if (nes.pc == 0 || (nes.ram[nes.pc] == 0x00 && nes.pc >= 0x0200 + off - 1))
                break;
        }
        printf("final: flags=%02X frame_irq=%d pending=%d\n",
               nes.flags, nes.frame_irq_flag, nes.dmc.dma_pending);
    }

    /* --- Open bus after DMA on $4000 (reload, 3 dummies) --- */
    nes.cpu_data_bus = 0x40;
    nes.dmc.cur_addr = 0xEFC0;
    nes.dmc.bytes_left = 5;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.dma_is_load = 0;
    nes.dmc.halt_addr = 0x4000;
    dmc_service_dma(&nes);
    printf("DMA+$4000 sample on bus=%02X (want 00 from EFC0)\n", nes.cpu_data_bus);
    printf("open bus read=%02X\n", cpu_read_nodma(&nes, 0x4000));

    /* After 32 bytes should be FF */
    nes.dmc.cur_addr = 0xEFC0 + 32;
    nes.dmc.bytes_left = 5;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.halt_addr = 0x4000;
    dmc_service_dma(&nes);
    printf("DMA+EFE0 sample on bus=%02X (want FF)\n", nes.cpu_data_bus);

    free(prg);
    return 0;
}
