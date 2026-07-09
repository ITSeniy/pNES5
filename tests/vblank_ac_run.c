/*
 * Run AccuracyCoin TEST_VBlank_Beginning / End on the host and print $50 results.
 * gcc -O2 -Isrc tests/vblank_ac_run.c src/bus.c src/mapper.c src/cpu.c src/ppu.c src/apu.c -o tests/vblank_ac_run
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"
#include "tables.h"

static void init_ntsc(struct NES *nes) {
    nes->is_pal = 0;
    nes->cpu_freq = 1789773;
    nes->num_scanlines = 262;
    nes->fc_step[0][0] = 7457;  nes->fc_step[0][1] = 14916;
    nes->fc_step[0][2] = 22371; nes->fc_step[0][3] = 29831;
    nes->fc_step[0][4] = 29832; nes->fc_step[0][5] = 29833;
    nes->fc_step[1][0] = 7457;  nes->fc_step[1][1] = 14916;
    nes->fc_step[1][2] = 22371; nes->fc_step[1][3] = 29829;
    nes->fc_step[1][4] = 37284; nes->fc_step[1][5] = 37285;
}

static int load_ines(struct NES *nes, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u8 hdr[16];
    if (fread(hdr, 1, 16, f) != 16) { fclose(f); return 0; }
    if (hdr[0]!='N'||hdr[1]!='E'||hdr[2]!='S') { fclose(f); return 0; }
    if (hdr[6] & 4) fseek(f, 512, SEEK_CUR);
    nes->prg_size = hdr[4] * 0x4000;
    nes->chr_size = hdr[5] * 0x2000;
    nes->mapper = (hdr[7] & 0xF0) | (hdr[6] >> 4);
    nes->mirror = (hdr[6] & 8) ? 4 : (hdr[6] & 1);
    nes->prg = malloc((size_t)nes->prg_size);
    nes->chr = nes->chr_size ? malloc((size_t)nes->chr_size) : calloc(1, 0x2000);
    if (!nes->chr_size) { nes->chr_size = 0x2000; nes->chr_is_ram = 1; }
    if (fread(nes->prg, 1, (size_t)nes->prg_size, f) != (size_t)nes->prg_size) { fclose(f); return 0; }
    if (!nes->chr_is_ram && fread(nes->chr, 1, (size_t)nes->chr_size, f) != (size_t)nes->chr_size) { fclose(f); return 0; }
    fclose(f);
    nes->prg_banks = hdr[4];
    nes->chr_banks = hdr[5] ? hdr[5] : 1;
    mapper_init(nes);
    nes->sp = 0xFD;
    nes->flags = F_I | F_U;
    nes->prev_irq_inhibit = F_I;
    nes->pc = cpu_read16(nes, 0xFFFC);
    nes->rom_loaded = 1;
    return 1;
}

/* Run until PC hits `stop` or max steps. */
static int run_until_pc(struct NES *nes, u16 stop, s64 max_steps) {
    for (s64 i = 0; i < max_steps; i++) {
        if (nes->pc == stop) return 1;
        /* Drive frames so VBlank keeps ticking: if cycles large, still ok via run_frame? 
         * Tests use busy-waits over many frames — must use run_frame when idle.
         * Actually clockslides are pure CPU; PPU must advance in lockstep via run_frame's
         * cycle accounting. The test is called FROM within a frame context on hardware
         * continuously. On host we need continuous PPU+CPU.
         *
         * run_frame runs a whole frame then returns. Inside a frame, cpu runs to schedule.
         * For multi-frame tests, we need either:
         *  a) one long run that spans frames, or
         *  b) run_frame in a loop while test executes — but test is the CPU code.
         *
         * Correct approach: call run_frame repeatedly; each run_frame executes CPU for
         * one frame of cycles. The PC advances through the test automatically.
         */
        run_frame(nes);
        if (nes->pc == stop) return 1;
        /* Also check if test returned via RTS to our sentinel */
        if (nes->pc == 0xFFFF) return 1;
    }
    return 0;
}

static void setup_and_call(struct NES *nes, u16 test_addr) {
    /* Minimal menu-less entry like RunTest */
    nes->ram[0x10] = 1; /* ErrorCode */
    nes->ram[0x3A] = 1; /* result_VblankSync_PreTest = OK (skip infinite abort) */
    /* NMI vector already points to $700 — put RTI there */
    nes->ram[0x700] = 0x40; /* RTI */
    /* Disable NMI/rendering */
    nes->ppu_ctrl = 0;
    nes->ppu_mask = 0;

    /* Warm up a few frames so PPU is live */
    for (int i = 0; i < 10; i++) run_frame(nes);

    /* Push sentinel return address $FFFE so RTS lands on $FFFF (we use 0xFFFE-1?) 
     * RTS pulls PCL, PCH and increments. Push $FF $FE → return to $FFFE? 
     * Actually RTS: pull PCL, pull PCH, PC = addr+1. To land on $FFFF push $FE $FF → PC=$FFFF.
     */
    nes->ram[0x100 + nes->sp] = 0xFE; nes->sp--;
    nes->ram[0x100 + nes->sp] = 0xFF; nes->sp--;
    /* Place BRK/RTI at $FFFF so we halt: use infinite JMP at FFFF — 
     * simpler: put RTS target as a JMP loop at $0200 */
    nes->sp = 0xFD;
    nes->ram[0x1FD] = 0xFF; /* high */
    nes->ram[0x1FC] = 0x01; /* low -1 for RTS → $0200? RTS adds 1: push 0x01FF → $0200 */
    /* push return-1 = $01FF so RTS → $0200 */
    nes->sp = 0xFB;
    nes->ram[0x1FC] = 0xFF;
    nes->ram[0x1FB] = 0x01;
    /* $0200: infinite JMP $0200 */
    nes->ram[0x200] = 0x4C;
    nes->ram[0x201] = 0x00;
    nes->ram[0x202] = 0x02;

    nes->pc = test_addr;
    nes->a = nes->x = nes->y = 0;

    for (int f = 0; f < 5000; f++) {
        run_frame(nes);
        if (nes->pc >= 0x200 && nes->pc <= 0x202) {
            printf("returned after %d frames, A=%02X\n", f, nes->a);
            return;
        }
    }
    printf("timeout PC=%04X A=%02X\n", nes->pc, nes->a);
}

static void dump50(struct NES *nes, const char *tag) {
    printf("%s $50:", tag);
    for (int i = 0; i < 10; i++) printf(" %02X", nes->ram[0x50 + i]);
    printf("\n");
}

static int run_one(const char *rom, u16 addr, const char *tag, const char *expect) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;
    memset(nes.sram, 0xFF, sizeof(nes.sram));
    if (!load_ines(&nes, rom)) {
        fprintf(stderr, "load failed %s\n", rom);
        return 1;
    }
    for (int i = 0; i < 120; i++) run_frame(&nes);
    printf("=== %s @ $%04X (pretest $3A=%02X jsr_off=$3C=%02X) ===\n",
           tag, addr, nes.ram[0x3A], nes.ram[0x3C]);
    setup_and_call(&nes, addr);
    dump50(&nes, tag);
    printf("expect: %s\n", expect);
    printf("result A=%02X\n\n", nes.a);
    free(nes.prg);
    free(nes.chr);
    return 0;
}

int main(int argc, char **argv) {
    const char *rom = argc > 1 ? argv[1] : "roms/AccuracyCoin.nes";
    run_one(rom, 0xB2AC, "begin", "02 02 02 02 00 01 01 (skip idx3)");
    run_one(rom, 0xB300, "end", "01 01 01 01 00 00 00");
    run_one(rom, 0xB40A, "nmi_timing", "03/02 then 02s then 01s");
    run_one(rom, 0xB473, "nmi_suppress", "see expected FF-masked");
    run_one(rom, 0xB4C2, "nmi_vbl_end", "01 01 01 00 00 00 00");
    run_one(rom, 0xB51E, "nmi_disabled", "00 00 00 FF 01 01 01");
    return 0;
}
