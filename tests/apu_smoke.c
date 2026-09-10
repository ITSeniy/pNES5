#include <stdio.h>
#include <string.h>
#include "nes.h"

u8 cpu_read(struct NES *nes, u16 addr) {
    (void)nes;
    (void)addr;
    return 0;
}

u8 cpu_read_nodma(struct NES *nes, u16 addr) {
    (void)nes;
    (void)addr;
    return 0;
}

u8 mapper_prg_read(struct NES *nes, u16 addr) {
    (void)nes;
    (void)addr;
    return 0;
}

u8 mapper_cpu_read(struct NES *nes, u16 addr) {
    (void)nes;
    (void)addr;
    return 0;
}

void cpu_dma_repeat_read(struct NES *nes) {
    (void)nes;
}

static void init_ntsc_frame_steps(struct NES *nes) {
    nes->cpu_freq = 1789773;
    nes->fc_step[0][0] = 7457;  nes->fc_step[0][1] = 14916;
    nes->fc_step[0][2] = 22371; nes->fc_step[0][3] = 29831;
    nes->fc_step[0][4] = 29832; nes->fc_step[0][5] = 29833;
    nes->fc_step[1][0] = 7457;  nes->fc_step[1][1] = 14916;
    nes->fc_step[1][2] = 22371; nes->fc_step[1][3] = 29829;
    nes->fc_step[1][4] = 37284; nes->fc_step[1][5] = 37285;
}

static int expect_int(const char *name, int got, int want) {
    if (got == want) return 0;
    printf("%s: got %d, want %d\n", name, got, want);
    return 1;
}

static int test_frame_irq(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc_frame_steps(&nes);

    apu_step(&nes, 29831);
    int fails = 0;
    fails += expect_int("frame irq flag", nes.frame_irq_flag, 1);
    fails += expect_int("frame irq pending", nes.apu_irq_pending, 1);

    apu_write_reg(&nes, 0x4017, 0x40);
    fails += expect_int("frame irq inhibit clears flag", nes.frame_irq_flag, 0);
    fails += expect_int("frame irq inhibit clears pending", nes.apu_irq_pending, 0);
    return fails;
}

static int test_dmc_registers(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));

    apu_write_reg(&nes, 0x4010, 0xCF);
    apu_write_reg(&nes, 0x4011, 0x55);
    apu_write_reg(&nes, 0x4012, 0x40);
    apu_write_reg(&nes, 0x4013, 0x02);
    apu_write_reg(&nes, 0x4015, 0x10);

    int fails = 0;
    fails += expect_int("dmc irq enable", nes.dmc.irq_enable, 1);
    fails += expect_int("dmc loop", nes.dmc.loop, 1);
    fails += expect_int("dmc period", nes.dmc.period_idx, 15);
    fails += expect_int("dmc output", nes.dmc.output_level, 0x55);
    fails += expect_int("dmc sample addr", nes.dmc.sample_addr, 0xD000);
    fails += expect_int("dmc sample len", nes.dmc.sample_len, 0x21);
    /* Load DMA is deferred until a CPU read cycle (cannot halt on $4015 write). */
    fails += expect_int("dmc bytes left", nes.dmc.bytes_left, 0x21);
    fails += expect_int("dmc sample buffer empty until get", nes.dmc.sample_buf_full, 0);
    fails += expect_int("dmc load DMA pending", nes.dmc.dma_pending, 1);
    fails += expect_int("dmc load flag", nes.dmc.dma_is_load, 1);
    return fails;
}

static int test_pulse_sweep_disabled_outputs(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc_frame_steps(&nes);
    nes.audio_handle = -1;

    apu_write_reg(&nes, 0x4000, 0x5F);
    apu_write_reg(&nes, 0x4001, 0x08);
    apu_write_reg(&nes, 0x4002, 0x98);
    apu_write_reg(&nes, 0x4003, 0x00);
    apu_write_reg(&nes, 0x4015, 0x01);
    apu_write_reg(&nes, 0x4003, 0x00);

    apu_step(&nes, 20000);

    int peak = 0;
    for (int i = 0; i < nes.audio_pos * 2; i++) {
        int v = nes.audio_buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }

    return expect_int("pulse disabled sweep still audible", peak > 0, 1);
}

static int test_audio_ring_producer(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc_frame_steps(&nes);
    nes.audio_thread_running = 1;
    nes.audio_read_pos = 0;
    nes.audio_write_pos = AUDIO_PRIME_BUFS * SAMPLES_PER_BUF;

    apu_step(&nes, 2000);
    int fails = 0;
    fails += expect_int("audio ring receives samples",
                        nes.audio_write_pos > AUDIO_PRIME_BUFS * SAMPLES_PER_BUF, 1);
    fails += expect_int("threaded audio bypasses linear capture", nes.audio_pos, 0);

    memset(&nes, 0, sizeof(nes));
    init_ntsc_frame_steps(&nes);
    nes.audio_thread_running = 1;
    nes.audio_write_pos = AUDIO_RING_FRAMES;
    apu_step(&nes, 200);
    fails += expect_int("full audio ring is not overwritten",
                        nes.audio_write_pos, AUDIO_RING_FRAMES);
    fails += expect_int("audio ring overrun is counted", nes.audio_overruns > 0, 1);
    return fails;
}

int main(void) {
    int fails = 0;
    fails += test_frame_irq();
    fails += test_dmc_registers();
    fails += test_pulse_sweep_disabled_outputs();
    fails += test_audio_ring_producer();

    if (fails) {
        printf("apu_smoke: %d failure(s)\n", fails);
        return 1;
    }

    printf("apu_smoke: ok\n");
    return 0;
}
