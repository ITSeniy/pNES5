#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    printf("SVC halt=%04X st=%d cy=%d full=%d bl=%d bits=%d pend=%d sil=%d\n", halt, stall,
           (int)nes->cycles, nes->dmc.sample_buf_full, nes->dmc.bytes_left, nes->dmc.bits_left,
           nes->dmc.dma_pending, nes->dmc.silence);
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
    r[pc++] = (u8)(s8)(lock - (pc + 1));
    u16 after = pc;
    r[pc++] = 0xA9;
    r[pc++] = 0x0F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    for (int i = 0; i < 600; i++)
        r[pc++] = 0xEA;
    r[pc++] = 0x4C;
    r[pc++] = (u8)((pc - 1) & 0xFF);
    r[pc++] = (u8)((pc - 1) >> 8);

    nes.pc = 0x200;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0x40;
    nes.cpu_data_bus = 0x40;

    int caught = 0;
    for (int i = 0; i < 5000; i++) {
        u16 p0 = nes.pc;
        u8 pend0 = nes.dmc.dma_pending;
        u8 full0 = nes.dmc.sample_buf_full;
        step1(&nes);
        if (!caught && nes.pc >= after) {
            caught = 1;
            printf("CATCH i=%d cy=%d a=%02X\n", i, (int)nes.cycles, nes.a);
        }
        if (i < 40 || nes.dmc.dma_pending != pend0 || nes.dmc.sample_buf_full != full0
            || (caught && i < 80) || (i % 50 == 0 && i < 400)) {
            printf("i=%d pc=%04X cy=%d pend=%d full=%d bl=%d bits=%d tc=%d sil=%d a=%02X\n", i, p0,
                   (int)nes.cycles, nes.dmc.dma_pending, nes.dmc.sample_buf_full, nes.dmc.bytes_left,
                   nes.dmc.bits_left, nes.dmc.timer_count, nes.dmc.silence, nes.a);
        }
        if (caught && (int)nes.cycles > 600)
            break;
    }
    return 0;
}
