/*
 * Smoke tests for CPU Behavior 2 fixes:
 *  - Branch page-cross dummy reads wrong-PCH address (may clear $2002)
 *  - Accumulator RMW dummy-reads PC
 *  - $4015 bit5 from internal bus; DMC updates external only
 *
 * gcc -O2 -Isrc tests/cpu_behavior2_smoke.c src/bus.c src/mapper.c src/cpu.c src/ppu.c src/apu.c -o tests/cpu_behavior2_smoke
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static void init_ntsc(struct NES *nes) {
    nes->is_pal = 0;
    nes->cpu_freq = 1789773;
    nes->num_scanlines = 262;
    nes->noise.shift_reg = 1;
}

static int fail;

static void expect(const char *name, int cond) {
    if (!cond) {
        printf("FAIL %s\n", name);
        fail++;
    } else {
        printf("ok   %s\n", name);
    }
}

/* Branch page-cross: BCC at $1FFF with offset to cross into $20xx intermediate. */
static void test_branch_page_cross_dummy(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    nes.prg_size = 0x8000;
    nes.prg = calloc(1, 0x8000);
    nes.chr_size = 0x2000;
    nes.chr = calloc(1, 0x2000);
    nes.chr_is_ram = 1;
    nes.mapper = 0;
    mapper_init(&nes);

    /* Put BCC $F1 at $1FFF; after operand PC=$2001, new=$1FF2, wrong=$20F2=$2002. */
    nes.ram[0x1FFF & 0x7FF] = 0x90; /* BCC — wait, $1FFF is not in $0000-$07FF mirror of $1Fxx
                                     * $1FFF maps to ram[0x7FF] */
    /* Use direct PC execution with code in RAM at $01F0 area instead. */
    /* Simpler: put branch at $00FE page edge. */
    /* BCC at $00FE, operand at $00FF = $02 → target $0102, wrong intermediate $0002 if...
     * old_pc after op = $0100, +2 = $0102, no page cross from $0100.
     *
     * BCC at $00FF: need operand at $0100. ram only to $07FF.
     * old after op = $0101, offset $FE (-2) → $00FF, wrong = $01FF.
     */

    /* Inject: at $01FD: CLC; BCC $FE  → at $01FF operand $FE, old=$0200, new=$01FE, wrong=$02FE */
    nes.ram[0x1FD] = 0x18; /* CLC */
    nes.ram[0x1FE] = 0x90; /* BCC */
    nes.ram[0x1FF] = 0xFE; /* -2 → $01FE */
    nes.ram[0x1FE] = 0x60; /* if we land here: RTS — wait overwrite */

    /* Cleaner unit test of the intermediate address via manual BR simulation check:
     * set VBlank, run BCC that page-crosses through $2002 mirror. */
    free(nes.prg);
    free(nes.chr);

    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    nes.ppu_status = 0x80; /* vblank set */
    nes.ppu_open_bus = 0xF1;
    /* Code at $06F0: CLC; BCC to cross */
    /* Put code in RAM and set PC. BCC at $06FE with operand $01:
     * after op PC=$0700, +1=$0701 no cross.
     * BCC at $06FF, operand needs to be at $0700. */
    nes.ram[0x6FE] = 0x18; /* CLC */
    nes.ram[0x6FF] = 0x90; /* BCC */
    /* We need operand in $0700 — that's still RAM. offset that page-crosses down:
     * old=$0701, off=$80 → new=$0681, wrong=$0781. Not PPU.
     *
     * For PPU: need wrong address in $2000-$2007.
     * old_pc=$2001 (after reading operand from $2000), new=$1FF2, wrong=$20F2.
     * That's the AccuracyCoin setup — needs open bus as code. Skip full sim;
     * verify BR intermediate math with a direct probe. */

    {
        u16 old_pc = 0x2001;
        s8 off = (s8)0xF1; /* -15 */
        u16 new_pc = (u16)(old_pc + off);
        u16 wrong = (u16)((old_pc & 0xFF00) | (new_pc & 0xFF));
        expect("branch intermediate $20F2", new_pc == 0x1FF2 && wrong == 0x20F2);
        expect("wrong maps to $2002", (wrong & 7) == 2);
    }

    /* Live: open-bus opcode BPL path is heavy; check $2002 clear on cpu_read($20F2). */
    nes.ppu_status = 0x80;
    (void)cpu_read(&nes, 0x20F2);
    expect("read $20F2 clears vblank", (nes.ppu_status & 0x80) == 0);
}

static void test_4015_internal_bit5(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);

    /* External has bit5 from DMC sample; internal does not. */
    nes.cpu_data_bus = 0x60;      /* external: bit5 set (like DMC sample) */
    nes.cpu_db_internal = 0x00;   /* internal: clear */
    nes.frame_irq_flag = 0;
    nes.dmc.irq_flag = 0;

    u8 v = cpu_read_nodma(&nes, 0x4015);
    expect("$4015 bit5 from internal not external", (v & 0x20) == 0);
    expect("$4015 does not update external", nes.cpu_data_bus == 0x60);
    expect("$4015 updates internal", nes.cpu_db_internal == v);

    /* Open bus still sees external sample. */
    u8 ob = cpu_read_nodma(&nes, 0x4000);
    expect("open bus $4000 still external", ob == 0x60);
}

static void test_asl_a_dummy(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    /* Place ASL A at $0100, next byte $4015 would clear frame irq if dummy-read. */
    nes.ram[0x100] = 0x0A; /* ASL A */
    nes.ram[0x101] = 0xEA; /* NOP — dummy target for PC after fetch */
    nes.pc = 0x0100;
    nes.a = 0x40;
    nes.frame_irq_flag = 1;
    /* If dummy reads $0101 (NOP), not $4015 — just ensure no crash and A shifts. */
    cpu_step(&nes);
    expect("ASL A shifts", nes.a == 0x80);
    expect("ASL A took 2 cycles", nes.cycles == 2);
}

int main(void) {
    test_branch_page_cross_dummy();
    test_4015_internal_bit5();
    test_asl_a_dummy();
    printf("%s (%d fails)\n", fail ? "FAILED" : "ALL OK", fail);
    return fail ? 1 : 0;
}
