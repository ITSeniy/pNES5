/*
 * AccuracyCoin DMA Bus Conflicts test 2 skeleton:
 * 64 DMA gets from $FFC0 with open-bus capture after each get.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_n;
static u16 g_h;

void dmc_phase_log_service(struct NES *n, u16 h, int s) {
    g_n++;
    g_h = h;
    (void)n;
    (void)s;
}

static void step1(struct NES *n) {
    n->dmc.ticks_exec = 0;
    int b = n->cycles;
    cpu_step(n);
    int r = n->cycles - b;
    if (r > 0) {
        while (n->dmc.ticks_exec < r)
            dmc_tick(n);
        apu_step(n, r);
        n->total_cycles += r;
    }
}

static int load_ines(struct NES *nes, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    u8 hdr[16];
    if (fread(hdr, 1, 16, f) != 16) {
        fclose(f);
        return 0;
    }
    int prg_units = hdr[4];
    nes->prg_size = prg_units * 0x4000;
    nes->chr_size = hdr[5] * 0x2000;
    nes->mapper = (hdr[7] & 0xF0) | ((hdr[6] >> 4) & 0x0F);
    nes->mirror = (hdr[6] & 8) ? 4 : (hdr[6] & 1);
    nes->prg = (u8 *)malloc((size_t)nes->prg_size);
    if (!nes->prg || fread(nes->prg, 1, (size_t)nes->prg_size, f) != (size_t)nes->prg_size) {
        fclose(f);
        return 0;
    }
    if (nes->chr_size > 0) {
        nes->chr = (u8 *)malloc((size_t)nes->chr_size);
        fread(nes->chr, 1, (size_t)nes->chr_size, f);
    } else {
        nes->chr_size = 0x2000;
        nes->chr = (u8 *)calloc(1, (size_t)nes->chr_size);
        nes->chr_is_ram = 1;
    }
    fclose(f);
    nes->prg_banks = prg_units;
    nes->chr_banks = hdr[5];
    mapper_init(nes);
    return 1;
}

int main(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.noise.shift_reg = 1;
    nes.audio_handle = -1;
    nes.chr_enable = 1;
    nes.fc_step[0][0] = 7457;
    nes.fc_step[0][1] = 14916;
    nes.fc_step[0][2] = 22371;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;

    if (!load_ines(&nes, "roms/AccuracyCoin.nes")) {
        fprintf(stderr, "load failed\n");
        return 2;
    }

    printf("mapper=%d prg=%d\n", nes.mapper, nes.prg_size);
    /* $4012=$BF → $C000|($BF<<6) = $EFC0 (00×32 then FF×32 in AccuracyCoin) */
    u16 base = (u16)(0xC000 | (0xBF << 6));
    printf("sample base $%04X\n", base);
    printf("PRG: ");
    for (int i = 0; i < 64; i++)
        printf("%02X%s", mapper_prg_read(&nes, (u16)(base + i)), ((i & 15) == 15) ? "\n     " : " ");
    printf("\n");

    /* Direct conflict simulation: fetch samples with dmc path */
    nes.pad_state = 0;
    nes.pad_shift = 0;
    nes.pad_strobe = 0;
    nes.frame_irq_flag = 1;

    u8 got[0x40];
    u16 addr = base;
    for (int i = 0; i < 0x40; i++) {
        /* Simulate dmc_fetch_sample via a one-shot DMA service */
        nes.dmc.enabled = 1;
        nes.dmc.bytes_left = 1;
        nes.dmc.cur_addr = addr;
        nes.dmc.sample_buf_full = 0;
        nes.dmc.dma_pending = 1;
        nes.dmc.dma_is_load = 0;
        nes.dmc.halt_addr = 0x4000;
        nes.cpu_data_bus = 0x40;
        dmc_service_dma(&nes);
        got[i] = nes.cpu_data_bus;
        addr++;
        if (addr == 0)
            addr = 0x8000;
    }

    printf("simulated bus after DMA:\n");
    for (int i = 0; i < 0x40; i++) {
        printf("%02X%s", got[i], ((i & 15) == 15) ? "\n" : " ");
    }

    int fails = 0;
    for (int x = 0; x < 0x40; x++) {
        int lo = x & 0x1F;
        if (lo == 0x16 || lo == 0x17)
            continue;
        u8 want = (x < 0x20) ? 0x00 : 0xFF;
        if (got[x] != want) {
            if (fails < 12)
                printf("mismatch x=%02X got=%02X want=%02X prg=%02X\n", x, got[x], want,
                       mapper_prg_read(&nes, (u16)(base + x)));
            fails++;
        }
    }
    printf("mismatches (excl controllers): %d\n", fails);
    printf("$16=%02X $17=%02X $36=%02X $37=%02X\n", got[0x16], got[0x17], got[0x36], got[0x37]);
    printf("frame_irq after $15-mirrors: flag=%d\n", nes.frame_irq_flag);

    free(nes.prg);
    free(nes.chr);
    return fails ? 1 : 0;
}
