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
    prg[0] = 0x2C;
    prg[1] = 0x15;
    prg[2] = 0x40;
    prg[3] = 0x00;

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
    nes.pc = 0x8000;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0;
    nes.frame_irq_flag = 1;
    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.dma_halt_delay = 3;

    step1(&nes);
    printf("delay=3: V=%d flag=%d flags=%02X want V=0\n", (nes.flags >> 6) & 1, nes.frame_irq_flag,
           nes.flags);

    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
    nes.pc = 0x8000;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0;
    nes.frame_irq_flag = 1;
    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.dma_halt_delay = 0;

    step1(&nes);
    printf("delay=0: V=%d flag=%d flags=%02X (opcode halt => V=1)\n", (nes.flags >> 6) & 1,
           nes.frame_irq_flag, nes.flags);

    memset(&nes, 0, sizeof(nes));
    nes.frame_irq_flag = 1;
    for (int i = 0; i < 3; i++) {
        u8 v = cpu_read_nodma(&nes, 0x4015);
        printf("dummy %d val=%02X flag=%d\n", i, v, nes.frame_irq_flag);
    }
    printf("real val=%02X flag=%d\n", cpu_read_nodma(&nes, 0x4015), nes.frame_irq_flag);
    return 0;
}
