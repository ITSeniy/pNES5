/* Run N frames of a ROM on host to catch hard crashes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

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
    int prg_units = hdr[4], chr_units = hdr[5];
    nes->prg_size = prg_units * 0x4000;
    nes->chr_size = chr_units * 0x2000;
    nes->mapper = (hdr[7] & 0xF0) | ((hdr[6] >> 4) & 0x0F);
    nes->mirror = (hdr[6] & 8) ? 4 : (hdr[6] & 1);
    nes->prg = malloc((size_t)nes->prg_size);
    nes->chr = malloc(nes->chr_size > 0 ? (size_t)nes->chr_size : 0x2000);
    if (!nes->prg || !nes->chr) { fclose(f); return 0; }
    if (fread(nes->prg, 1, (size_t)nes->prg_size, f) != (size_t)nes->prg_size) {
        fclose(f); return 0;
    }
    if (chr_units) {
        if (fread(nes->chr, 1, (size_t)nes->chr_size, f) != (size_t)nes->chr_size) {
            fclose(f); return 0;
        }
        nes->chr_is_ram = 0;
    } else {
        nes->chr_size = 0x2000;
        nes->chr_is_ram = 1;
        memset(nes->chr, 0, 0x2000);
    }
    fclose(f);
    nes->chr_enable = 1;
    nes->prg_banks = prg_units;
    nes->chr_banks = chr_units ? chr_units : 1;
    mapper_init(nes);
    nes->sp = 0xFD;
    nes->flags = 0x24;
    nes->pc = (u16)mapper_prg_read(nes, 0xFFFC) | ((u16)mapper_prg_read(nes, 0xFFFD) << 8);
    /* Use cpu_read16 if available — mapper path for vectors is fine on NROM */
    {
        u16 lo = 0, hi = 0;
        /* NROM: last 16 bytes of PRG */
        if (nes->prg_size >= 6) {
            lo = nes->prg[nes->prg_size - 4];
            hi = nes->prg[nes->prg_size - 3];
            nes->pc = (u16)(lo | (hi << 8));
        }
    }
    nes->rom_loaded = 1;
    nes->noise.shift_reg = 1;
    return 1;
}

int main(int argc, char **argv) {
    const char *rom = argc > 1 ? argv[1] : "roms/AccuracyCoin.nes";
    int frames = argc > 2 ? atoi(argv[2]) : 120;
    struct NES *nes = calloc(1, sizeof(*nes));
    if (!nes) return 1;
    init_ntsc(nes);
    if (!load_ines(nes, rom)) {
        fprintf(stderr, "load fail %s\n", rom);
        return 1;
    }
    printf("pc=%04X mapper=%d frames=%d\n", nes->pc, nes->mapper, frames);
    for (int i = 0; i < frames; i++) {
        run_frame(nes);
        if ((i & 15) == 15)
            printf("frame %d pc=%04X status=%02X mask=%02X\n",
                   i, nes->pc, nes->ppu_status, nes->ppu_mask);
    }
    printf("OK after %d frames pc=%04X\n", frames, nes->pc);
    free(nes->prg);
    free(nes->chr);
    free(nes);
    return 0;
}
