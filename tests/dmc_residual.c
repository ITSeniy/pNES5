#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

void dmc_phase_log_service(struct NES *n, u16 h, int s) {
    printf("SVC halt=%04X st=%d cy=%d bits=%d full=%d bl=%d tc=%d\n", h, s, (int)n->cycles,
           n->dmc.bits_left, n->dmc.sample_buf_full, n->dmc.bytes_left, n->dmc.timer_count);
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

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    memset(prg + 0x7FC0, 0, 0x3A);
    prg[0x7FFC] = 0;
    prg[0x7FFD] = 0x80;
    prg[0] = 0x40;
    {
        u32 o = 0x100;
        for (int i = 0; i < 44; i++)
            prg[o++] = 0xEA;
        prg[o++] = 0x60;
    }

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
    nes.fc_step[0][0] = 7457;
    nes.fc_step[0][1] = 14916;
    nes.fc_step[0][2] = 22371;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;

    u8 *r = nes.ram;
    u16 pc = 0x200;
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
    r[pc++] = 0xA9;
    r[pc++] = 0x0F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA5;
    r[pc++] = 0x00;
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    for (int k = 0; k < 3; k++) {
        r[pc++] = 0x20;
        r[pc++] = 0x00;
        r[pc++] = 0x81;
    }
    for (int k = 0; k < 25; k++)
        r[pc++] = 0xEA;
    for (int k = 0; k < 23; k++)
        r[pc++] = 0xEA;
    r[pc++] = 0xEA;
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
    int last_pend = 0;
    for (int i = 0; i < 300000; i++) {
        if (nes.dmc.dma_pending && !last_pend)
            printf("PEND rise cy=%d pc=%04X bits=%d full=%d bl=%d loop=%d\n", (int)nes.cycles,
                   nes.pc, nes.dmc.bits_left, nes.dmc.sample_buf_full, nes.dmc.bytes_left,
                   nes.dmc.loop);
        last_pend = nes.dmc.dma_pending;
        step1(&nes);
        if (!caught && nes.pc >= after && nes.a == 0) {
            caught = 1;
            printf("CATCH cy=%d bits=%d full=%d bl=%d tc=%d\n", (int)nes.cycles, nes.dmc.bits_left,
                   nes.dmc.sample_buf_full, nes.dmc.bytes_left, nes.dmc.timer_count);
        }
        if (caught && nes.pc == lda) {
            printf("enter LDA cy=%d pend=%d\n", (int)nes.cycles, nes.dmc.dma_pending);
        }
        if (caught && nes.pc == done) {
            printf("LDA done A=%02X cy=%d\n", nes.a, (int)nes.cycles);
            break;
        }
        if (nes.pc >= 0x8000 && caught)
            break;
    }
    return 0;
}
