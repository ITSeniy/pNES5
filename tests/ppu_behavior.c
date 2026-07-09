/*
 * AccuracyCoin PPU BEHAVIOR unit checks:
 *  - $2007 read buffer + palette underfill ($2Fxx)
 *  - Palette greyscale read
 *  - Sprite zero hit (VerifySpriteZeroHits geometry)
 *  - Attributes-as-tiles (v=$2FC0) sprite zero
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
    nes->fc_step[0][0] = 7457;  nes->fc_step[0][1] = 14916;
    nes->fc_step[0][2] = 22371; nes->fc_step[0][3] = 29831;
    nes->fc_step[0][4] = 29832; nes->fc_step[0][5] = 29833;
    nes->fc_step[1][0] = 7457;  nes->fc_step[1][1] = 14916;
    nes->fc_step[1][2] = 22371; nes->fc_step[1][3] = 29829;
    nes->fc_step[1][4] = 37284; nes->fc_step[1][5] = 37285;
}

static int fails;

static void expect_u8(const char *n, u8 got, u8 want) {
    if (got != want) {
        printf("FAIL %s: got $%02X want $%02X\n", n, got, want);
        fails++;
    } else {
        printf("PASS %s: $%02X\n", n, got);
    }
}

static void expect_nz(const char *n, int v) {
    if (!v) {
        printf("FAIL %s: expected non-zero\n", n);
        fails++;
    } else {
        printf("PASS %s\n", n);
    }
}

static void expect_z(const char *n, int v) {
    if (v) {
        printf("FAIL %s: expected zero, got %d\n", n, v);
        fails++;
    } else {
        printf("PASS %s\n", n);
    }
}

static int load_ines(struct NES *nes, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    u8 hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || hdr[0] != 'N') { fclose(f); return 0; }
    int prg_units = hdr[4], chr_units = hdr[5];
    nes->prg_size = prg_units * 0x4000;
    nes->chr_size = chr_units * 0x2000;
    nes->mapper = (hdr[7] & 0xF0) | ((hdr[6] >> 4) & 0x0F);
    nes->mirror = (hdr[6] & 8) ? 4 : (hdr[6] & 1);
    nes->prg = malloc((size_t)nes->prg_size);
    nes->chr = malloc((size_t)nes->chr_size);
    if (!nes->prg || !nes->chr) { fclose(f); return 0; }
    if (fread(nes->prg, 1, (size_t)nes->prg_size, f) != (size_t)nes->prg_size) {
        fclose(f); return 0;
    }
    if (chr_units) {
        if (fread(nes->chr, 1, (size_t)nes->chr_size, f) != (size_t)nes->chr_size) {
            fclose(f); return 0;
        }
        nes->chr_is_ram = 0;
    }
    fclose(f);
    nes->chr_enable = 1;
    nes->prg_banks = prg_units;
    nes->chr_banks = chr_units;
    mapper_init(nes);
    return 1;
}

static void set_addr(struct NES *nes, u16 a) {
    cpu_write(nes, 0x2006, (u8)(a >> 8));
    cpu_write(nes, 0x2006, (u8)(a & 0xFF));
}

static u8 read2007(struct NES *nes) {
    return cpu_read(nes, 0x2007);
}

static void write2007(struct NES *nes, u8 v) {
    cpu_write(nes, 0x2007, v);
}

static void test_read_buffer(struct NES *nes) {
    printf("\n--- PPU Read Buffer / Palette underfill ---\n");
    nes->ppu_mask = 0;
    nes->in_vblank = 1;
    nes->write_toggle = 0;

    /* Test 7: buffer filled from nametable under palette */
    set_addr(nes, 0x2F00);
    write2007(nes, 0x5A);
    set_addr(nes, 0x2C00);
    (void)read2007(nes);
    (void)read2007(nes);
    (void)read2007(nes); /* buffer has $02-ish sequence not important */
    set_addr(nes, 0x3F00);
    (void)read2007(nes); /* palette immediate; buffer <- [$2F00]=$5A */
    set_addr(nes, 0x0000);
    expect_u8("palette underfill buffer", read2007(nes), 0x5A);

    /* Immediate palette read + open bus high bits */
    set_addr(nes, 0x3F01);
    write2007(nes, 0x2D);
    set_addr(nes, 0x3F01);
    nes->ppu_open_bus = 0x00;
    /* dummy not needed for palette — first read is immediate */
    expect_u8("palette immediate low", (u8)(read2007(nes) & 0x3F), 0x2D);

    /* Greyscale read */
    set_addr(nes, 0x3F1C);
    write2007(nes, 0x5A);
    set_addr(nes, 0x3F1C);
    nes->ppu_mask = 0x01;
    nes->ppu_open_bus = 0;
    expect_u8("greyscale palette read", read2007(nes), 0x10);
    nes->ppu_mask = 0;
}

static void test_sprite_zero_geom(struct NES *nes) {
    printf("\n--- Sprite zero geometry (VerifySpriteZeroHits) ---\n");
    memset(nes->vram, 0x24, sizeof(nes->vram));
    memset(nes->oam, 0xFF, sizeof(nes->oam));
    nes->ppu_ctrl = 0x00; /* BG + spr pattern table $0000 */
    nes->ppu_mask = 0x1E; /* show left 8 + bg + spr */
    nes->fine_x = 0;
    nes->write_toggle = 0;
    nes->ppu_status = 0;

    /* BG tile $C0 at $2C21 → screen (8,8); opaque pixel (0,0) of tile */
    set_addr(nes, 0x2C21);
    write2007(nes, 0xC0);

    /* Intentional miss: Y=4 → pixel at scanline 5 */
    nes->oam[0] = 0x04;
    nes->oam[1] = 0xC0;
    nes->oam[2] = 0x03;
    nes->oam[3] = 0x08;
    nes->vram_addr = 0x2C00;
    nes->temp_addr = 0x2C00;
    nes->ppu_status = 0;
    render_scanline(nes, 5);
    expect_z("miss Y=4 no hit on SL5 (sp0 bit)", nes->ppu_status & 0x40);
    nes->ppu_status = 0;
    render_scanline(nes, 8);
    expect_z("miss Y=4 no hit on SL8", nes->ppu_status & 0x40);

    /*
     * $2006 $2C,$00 leaves fine Y = 2, so tile row 1 ($2C21) appears on
     * frame scanline 6 (not 8). Y=5 → sprite pixel on SL6 → hit.
     */
    nes->write_toggle = 0;
    set_addr(nes, 0x2C00);
    nes->oam[0] = 0x05;
    nes->ppu_status = 0;
    {
        int hit_y = -1;
        for (int y = 0; y < 16; y++) {
            if (nes->ppu_mask & 0x18)
                nes->vram_addr = (nes->vram_addr & 0xFBE0) | (nes->temp_addr & 0x041F);
            render_scanline(nes, y);
            if ((nes->ppu_status & 0x40) && hit_y < 0)
                hit_y = y;
            if (nes->ppu_mask & 0x18) {
                u16 v = nes->vram_addr;
                if ((v & 0x7000) != 0x7000) v += 0x1000;
                else {
                    v &= ~0x7000;
                    int cy = (v & 0x03E0) >> 5;
                    if (cy == 29) { cy = 0; v ^= 0x0800; }
                    else if (cy == 31) cy = 0;
                    else cy++;
                    v = (v & ~0x03E0) | (cy << 5);
                }
                nes->vram_addr = v;
            }
        }
        if (hit_y == 6)
            printf("PASS hit Y=5 on SL6 (VerifySpriteZeroHits)\n");
        else {
            printf("FAIL hit Y=5: hit_y=%d status=$%02X\n", hit_y, nes->ppu_status);
            fails++;
        }
    }

    /* Attributes as tiles: v=$2FC0, tile $25 at $2FC8, sprite at X=$40 Y=$00 */
    memset(nes->vram, 0x24, sizeof(nes->vram));
    set_addr(nes, 0x2FC0);
    for (int i = 0; i < 0x40; i++) write2007(nes, 0x24);
    set_addr(nes, 0x2FC8);
    write2007(nes, 0x25);
    nes->oam[0] = 0x00;
    nes->oam[1] = 0x25;
    nes->oam[2] = 0xFF;
    nes->oam[3] = 0x40;
    nes->vram_addr = 0x2FC0;
    nes->temp_addr = 0x2FC0;
    nes->ppu_status = 0;
    /* tile $25 opaque on rows 5-6 → scanlines 6-7 (Y=0 → sy=1) */
    for (int y = 0; y < 16; y++) {
        u16 vsave = nes->vram_addr;
        render_scanline(nes, y);
        /* advance fine Y like hardware after each line */
        {
            u16 v = nes->vram_addr;
            if ((v & 0x7000) != 0x7000) v += 0x1000;
            else {
                v &= ~0x7000;
                int cy = (v & 0x03E0) >> 5;
                if (cy == 29) { cy = 0; v ^= 0x0800; }
                else if (cy == 31) cy = 0;
                else cy++;
                v = (v & ~0x03E0) | (cy << 5);
            }
            nes->vram_addr = v;
        }
        if (nes->ppu_status & 0x40)
            printf("  attr-as-tiles hit on SL %d (v was $%04X)\n", y, vsave);
    }
    expect_nz("attributes as tiles sprite zero", nes->ppu_status & 0x40);
}

int main(int argc, char **argv) {
    const char *rom = argc > 1 ? argv[1] : "roms/AccuracyCoin.nes";
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    init_ntsc(&nes);
    if (!load_ines(&nes, rom)) {
        fprintf(stderr, "load failed: %s\n", rom);
        return 1;
    }
    printf("mirror=%d mapper=%d chr=%d\n", nes.mirror, nes.mapper, (int)nes.chr_size);

    test_read_buffer(&nes);
    test_sprite_zero_geom(&nes);

    free(nes.prg);
    free(nes.chr);
    printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
