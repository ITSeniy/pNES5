/*
 * Host checks for DMA+$4016 OE, DMA+$2007 dummies, open-bus sample hold.
 */
#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    (void)nes;
    (void)halt;
    (void)stall;
}

static int fails;

static void expect_u8(const char *n, u8 g, u8 e) {
    if (g != e) {
        printf("FAIL %s: got %02X want %02X\n", n, g, e);
        fails++;
    } else
        printf("ok   %s\n", n);
}

static void expect_i(const char *n, int g, int e) {
    if (g != e) {
        printf("FAIL %s: got %d want %d\n", n, g, e);
        fails++;
    } else
        printf("ok   %s\n", n);
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

/*
 * Reload DMA on $2007 data cycle (DMASync-aligned): dummies re-read $2007.
 * Pending is armed on the data address itself — ASAP halt (no stream defer).
 */
static void test_2007_via_abs_sim(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0xEA, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    nes.mirror = 1;
    for (u8 i = 0; i < 8; i++)
        nes.vram[i] = i;
    nes.read_buf = 0;
    nes.vram_addr = 0x2001;

    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.dma_is_load = 0;

    u8 a = cpu_read(&nes, 0x2007);
    expect_u8("LDA $2007 after DMA dummies", a, 3);
    expect_i("halt was $2007 path (v advanced 4)", (int)nes.vram_addr, 0x2005);
}

/* DMA dummies on $4016: single OE clock across reload. */
static void test_4016_contiguous_oe(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0xEA, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    /* Buttons: only A (bit0=1 after first clock... pad_state bit0 = A) */
    nes.pad_state = 0x01;
    nes.pad_shift = 0x01;
    nes.pad_strobe = 0;

    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.dma_is_load = 0;
    nes.cpu_data_bus = 0x40;

    /* Direct service on $4016 (as if data cycle of LDA $4016) */
    nes.dmc.halt_addr = 0x4016;
    /* Use cpu_read which services */
    u8 a = cpu_read(&nes, 0x4016);
    /* First clock got bit0=1, dummies re-read same bit, final same */
    expect_u8("DMA+$4016 first bit is A", (u8)(a & 1), 1);
    /* Shift advanced only once during whole DMA+read */
    expect_u8("pad_shift after one OE", nes.pad_shift, 0x80); /* after one clock empty → 0x80 fill */

    /* Second read outside DMA clocks again */
    a = cpu_read(&nes, 0x4016);
    expect_u8("second $4016 after DMA", (u8)(a & 1), 0);
}

/* Open bus still sees sample when halt was on ADH-like RAM put. */
static void test_openbus_hold(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.halt_addr = 0x0200; /* code/RAM — not $4000 */
    nes.ram[0x0200] = 0x40;
    nes.cpu_data_bus = 0x40;

    dmc_service_dma(&nes);
    expect_u8("sample on bus after DMA", nes.cpu_data_bus, 0x00);
    /* Simulate ADH operand put of $40 — hold should keep $00 */
    (void)cpu_read_nodma(&nes, 0x0200);
    expect_u8("hold blocks code put", nes.cpu_data_bus, 0x00);
    expect_u8("open bus $4000 still sample", cpu_read_nodma(&nes, 0x4000), 0x00);
}

/* $4016 I/O must clear hold and update bus with controller bit. */
static void test_4016_clears_hold(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.chr_enable = 1;
    nes.cpu_data_bus = 0x00;
    nes.dmc.bus_hold_ttl = 2;
    nes.pad_state = 0xFF;
    nes.pad_shift = 0xFF;
    u8 a = cpu_read_nodma(&nes, 0x4016);
    expect_u8("$4016 clears hold and returns bit", (u8)(a & 1), 1);
    expect_i("hold cleared", nes.dmc.bus_hold_ttl, 0);
    expect_u8("bus has joy bit0", (u8)(nes.cpu_data_bus & 1), 1);
}

int main(void) {
    fails = 0;
    printf("--- 2007 abs ---\n");
    test_2007_via_abs_sim();
    printf("--- 4016 OE ---\n");
    test_4016_contiguous_oe();
    printf("--- openbus hold ---\n");
    test_openbus_hold();
    printf("--- 4016 clears hold ---\n");
    test_4016_clears_hold();
    printf(fails ? "FAILED %d\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
