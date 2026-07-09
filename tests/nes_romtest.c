#include <errno.h>
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

static void init_pal(struct NES *nes) {
    nes->is_pal = 1;
    nes->cpu_freq = 1662607;
    nes->num_scanlines = 312;
    nes->fc_step[0][0] = 8313;  nes->fc_step[0][1] = 16627;
    nes->fc_step[0][2] = 24939; nes->fc_step[0][3] = 33252;
    nes->fc_step[0][4] = 33253; nes->fc_step[0][5] = 33254;
    nes->fc_step[1][0] = 8313;  nes->fc_step[1][1] = 16627;
    nes->fc_step[1][2] = 24939; nes->fc_step[1][3] = 33253;
    nes->fc_step[1][4] = 41565; nes->fc_step[1][5] = 41566;
}

static void nes_reset_host(struct NES *nes) {
    memset(nes, 0, sizeof(*nes));
    init_ntsc(nes);
    nes->audio_handle = -1;
    nes->noise.shift_reg = 1;
    memset(nes->sram, 0xFF, sizeof(nes->sram));
}



static int read_exact(FILE *f, void *dst, size_t len) {
    return fread(dst, 1, len, f) == len;
}

static int load_ines(struct NES *nes, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 0;
    }

    u8 hdr[16];
    if (!read_exact(f, hdr, sizeof(hdr))
        || hdr[0] != 'N' || hdr[1] != 'E' || hdr[2] != 'S' || hdr[3] != 0x1A) {
        fprintf(stderr, "%s: not an iNES ROM\n", path);
        fclose(f);
        return 0;
    }

    int nes2 = ((hdr[7] & 0x0C) == 0x08);
    int prg_units = hdr[4];
    int chr_units = hdr[5];
    if (nes2 && (hdr[9] & 0x0F) != 0x0F && (hdr[9] >> 4) != 0x0F) {
        prg_units |= (hdr[9] & 0x0F) << 8;
        chr_units |= (hdr[9] >> 4) << 8;
    }

    int mapper = (hdr[7] & 0xF0) | ((hdr[6] >> 4) & 0x0F);
    if (nes2) mapper |= (hdr[8] & 0x0F) << 8;
    if (!mapper_supported(mapper)) {
        fprintf(stderr, "%s: unsupported mapper %d\n", path, mapper);
        fclose(f);
        return 0;
    }

    nes->prg_size = prg_units * 0x4000;
    nes->chr_size = chr_units * 0x2000;
    nes->chr_banks = chr_units;
    nes->prg_banks = prg_units;
    nes->mapper = mapper;
    nes->mirror = (hdr[6] & 8) ? 4 : (hdr[6] & 1);
    nes->has_battery = (hdr[6] & 2) ? 1 : 0;

    if (nes->prg_size <= 0) {
        fprintf(stderr, "%s: empty PRG ROM\n", path);
        fclose(f);
        return 0;
    }

    if (hdr[6] & 4)
        fseek(f, 512, SEEK_CUR);

    nes->prg = (u8 *)malloc((size_t)nes->prg_size);
    if (!nes->prg || !read_exact(f, nes->prg, (size_t)nes->prg_size)) {
        fprintf(stderr, "%s: failed to read PRG ROM\n", path);
        fclose(f);
        return 0;
    }

    if (nes->chr_size > 0) {
        nes->chr = (u8 *)malloc((size_t)nes->chr_size);
        if (!nes->chr || !read_exact(f, nes->chr, (size_t)nes->chr_size)) {
            fprintf(stderr, "%s: failed to read CHR ROM\n", path);
            fclose(f);
            return 0;
        }
        nes->chr_is_ram = 0;
    } else {
        nes->chr_size = 0x2000;
        nes->chr = (u8 *)calloc(1, (size_t)nes->chr_size);
        nes->chr_is_ram = 1;
    }
    fclose(f);

    if (!nes->chr) {
        fprintf(stderr, "%s: out of memory\n", path);
        return 0;
    }

    mapper_init(nes);

    int pal_rom;
    if (nes2) {
        pal_rom = ((hdr[12] & 3) == 1);
    } else {
        int clean_tail = 1;
        for (int i = 10; i < 16; i++)
            if (hdr[i]) clean_tail = 0;
        pal_rom = clean_tail && hdr[9] == 1;
    }
    if (pal_rom)
        init_pal(nes);

    nes->sp = 0xFD;
    nes->flags = F_I | F_U;
    nes->prev_irq_inhibit = F_I;
    nes->pc = cpu_read16(nes, 0xFFFC);
    nes->rom_loaded = 1;
    return 1;
}

static void free_rom(struct NES *nes) {
    free(nes->prg);
    free(nes->chr);
    nes->prg = 0;
    nes->chr = 0;
}

static int parse_int_arg(const char *s, int fallback) {
    char *end = 0;
    long v = strtol(s, &end, 0);
    return (end && *end == 0) ? (int)v : fallback;
}

struct pad_event {
    int frame;
    u8 state;
};

static int parse_pad_event(const char *s, struct pad_event *ev) {
    char *end = 0;
    long frame = strtol(s, &end, 0);
    if (!end || *end != ':') return 0;
    long state = strtol(end + 1, &end, 0);
    if (!end || *end != 0) return 0;
    ev->frame = (int)frame;
    ev->state = (u8)state;
    return 1;
}

static void print_blargg_status(struct NES *nes) {
    u8 status = nes->sram[0];
    printf("blargg $6000: 0x%02X", status);
    if (status == 0) printf(" PASS");
    else if (status == 0x80) printf(" RUNNING");
    else if (status == 0x81) printf(" RESET_NEEDED");
    else if (status != 0xFF) printf(" FAIL_CODE_%u", status);
    else printf(" UNTOUCHED");
    printf("\n");

    if (nes->sram[4] >= 32 && nes->sram[4] < 127) {
        printf("message: ");
        for (int i = 4; i < 0x2000 && nes->sram[i]; i++) {
            u8 ch = nes->sram[i];
            putchar((ch >= 32 && ch < 127) ? ch : '.');
        }
        putchar('\n');
    }
}

static int dump_ppm(struct NES *nes, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 0;
    }
    fprintf(f, "P6\n%d %d\n255\n", NES_W, NES_H);
    for (int y = 0; y < NES_H; y++) {
        for (int x = 0; x < NES_W; x++) {
            u32 c = nes_rgb(nes->screen[y * NES_W + x] & 0x3F);
            fputc((c >> 16) & 0xFF, f);
            fputc((c >> 8) & 0xFF, f);
            fputc(c & 0xFF, f);
        }
    }
    fclose(f);
    return 1;
}

static void wav_u16(FILE *f, u16 v) {
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static void wav_u32(FILE *f, u32 v) {
    wav_u16(f, v & 0xFFFF);
    wav_u16(f, v >> 16);
}

static FILE *open_wav(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 0;
    }
    fwrite("RIFF", 1, 4, f); wav_u32(f, 0);
    fwrite("WAVEfmt ", 1, 8, f); wav_u32(f, 16);
    wav_u16(f, 1); wav_u16(f, 2);
    wav_u32(f, SAMPLE_RATE);
    wav_u32(f, SAMPLE_RATE * 4);
    wav_u16(f, 4); wav_u16(f, 16);
    fwrite("data", 1, 4, f); wav_u32(f, 0);
    return f;
}

static void write_wav_samples(FILE *f, struct NES *nes, u32 *bytes) {
    if (!f || nes->audio_pos <= 0) return;
    int count = nes->audio_pos * 2;
    fwrite(nes->audio_buf, sizeof(s16), (size_t)count, f);
    *bytes += (u32)(count * sizeof(s16));
    nes->audio_pos = 0;
}

static void close_wav(FILE *f, u32 data_bytes) {
    if (!f) return;
    fseek(f, 4, SEEK_SET);
    wav_u32(f, 36 + data_bytes);
    fseek(f, 40, SEEK_SET);
    wav_u32(f, data_bytes);
    fclose(f);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s ROM [--frames N] [--steps N] [--nestest] [--pc HEX] [--expect-pass] [--dump-ppm PATH] [--dump-wav PATH] [--pad-event FRAME:HEX]\n"
        "  blargg-style tests: run frames, then read $6000 and message at $6004\n"
        "  nestest smoke: use --nestest --steps N to start at $C000 and print final CPU state\n",
        argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    int frames = 600;
    int steps = 0;
    int nestest = 0;
    int expect_pass = 0;
    int override_pc = -1;
    int debug_state = 0;
    const char *dump_path = 0;
    const char *wav_path = 0;
    struct pad_event pad_events[64];
    int pad_event_count = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = parse_int_arg(argv[++i], frames);
        } else if (!strcmp(argv[i], "--steps") && i + 1 < argc) {
            steps = parse_int_arg(argv[++i], steps);
        } else if (!strcmp(argv[i], "--pc") && i + 1 < argc) {
            override_pc = parse_int_arg(argv[++i], override_pc);
        } else if (!strcmp(argv[i], "--nestest")) {
            nestest = 1;
            override_pc = 0xC000;
            if (steps == 0) steps = 8991;
        } else if (!strcmp(argv[i], "--expect-pass")) {
            expect_pass = 1;
        } else if (!strcmp(argv[i], "--dump-ppm") && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (!strcmp(argv[i], "--dump-wav") && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (!strcmp(argv[i], "--debug-state")) {
            debug_state = 1;
        } else if (!strcmp(argv[i], "--pad-event") && i + 1 < argc) {
            if (pad_event_count >= 64 || !parse_pad_event(argv[++i], &pad_events[pad_event_count++])) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    struct NES nes;
    nes_reset_host(&nes);
    if (!load_ines(&nes, argv[1]))
        return 1;

    if (override_pc >= 0)
        nes.pc = (u16)override_pc;

    printf("ROM: %s\n", argv[1]);
    printf("mapper=%d prg=%d chr=%d region=%s pc=%04X\n",
           nes.mapper, nes.prg_size, nes.chr_size, nes.is_pal ? "PAL" : "NTSC", nes.pc);

    if (steps > 0) {
        for (int i = 0; i < steps; i++)
            cpu_step(&nes);
    }

    if (!nestest) {
        FILE *wav = wav_path ? open_wav(wav_path) : 0;
        u32 wav_bytes = 0;
        for (int i = 0; i < frames; i++) {
            for (int e = 0; e < pad_event_count; e++)
                if (pad_events[e].frame == i)
                    nes.pad_state = pad_events[e].state;
            run_frame(&nes);
            write_wav_samples(wav, &nes, &wav_bytes);
        }
        close_wav(wav, wav_bytes);
        if (wav_path && wav_bytes)
            printf("dumped audio: %s (%u bytes)\n", wav_path, wav_bytes);
        if (!debug_state)
            print_blargg_status(&nes);
        if (dump_path && dump_ppm(&nes, dump_path))
            printf("dumped screen: %s\n", dump_path);
        if (debug_state) {
            printf("state: PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%d TOTAL=%d\n",
                   nes.pc, nes.a, nes.x, nes.y, nes.flags, nes.sp, nes.cycles, nes.total_cycles);
            printf("ppu: ctrl=%02X mask=%02X status=%02X v=%04X t=%04X fine=%u odd=%u overrun=%d rem=%d\n",
                   nes.ppu_ctrl, nes.ppu_mask, nes.ppu_status, nes.vram_addr, nes.temp_addr,
                   nes.fine_x, nes.odd_frame, nes.frame_cpu_overrun, nes.frame_cycle_rem);
            printf("mmc3: sel=%02X prg_mode=%u chr_mode=%u regs=%02X %02X %02X %02X %02X %02X %02X %02X irq_latch=%02X irq_count=%02X irq_en=%u irq_reload=%u\n",
                   nes.mmc3_select, nes.mmc3_prg_mode, nes.mmc3_chr_mode,
                   nes.mmc3_regs[0], nes.mmc3_regs[1], nes.mmc3_regs[2], nes.mmc3_regs[3],
                   nes.mmc3_regs[4], nes.mmc3_regs[5], nes.mmc3_regs[6], nes.mmc3_regs[7],
                   nes.mmc3_irq_latch, nes.mmc3_irq_count, nes.mmc3_irq_enable, nes.mmc3_irq_reload);
            printf("ram: 1C=%02X 1D=%02X 1E=%02X 1F=%02X 20=%02X 21=%02X 22=%02X 35=%02X 36=%02X 37=%02X 38=%02X EF=%02X\n",
                   nes.ram[0x1C], nes.ram[0x1D], nes.ram[0x1E], nes.ram[0x1F],
                   nes.ram[0x20], nes.ram[0x21], nes.ram[0x22], nes.ram[0x35],
                   nes.ram[0x36], nes.ram[0x37], nes.ram[0x38], nes.ram[0xEF]);
            printf("apu: p0(en=%u len=%u duty=%u vol=%u const=%u halt=%u sw=%u neg=%u sh=%u per=%u t=%u pos=%u) "
                   "p1(en=%u len=%u duty=%u vol=%u const=%u halt=%u sw=%u neg=%u sh=%u per=%u t=%u pos=%u) "
                   "tri(en=%u len=%u lin=%u reload=%u ctl=%u load=%u t=%u step=%u) "
                   "noi(en=%u len=%u vol=%u const=%u halt=%u mode=%u per=%u shift=%04X) "
                   "dmc(en=%u irq=%u loop=%u per=%u out=%u addr=%04X len=%u cur=%04X left=%u buf=%u bits=%u sil=%u)\n",
                   nes.pulse[0].enabled, nes.pulse[0].length, nes.pulse[0].duty,
                   nes.pulse[0].vol_period, nes.pulse[0].const_vol, nes.pulse[0].halt,
                   nes.pulse[0].sweep_en, nes.pulse[0].sweep_neg, nes.pulse[0].sweep_shift,
                   nes.pulse[0].sweep_period, nes.pulse[0].timer, nes.pulse[0].duty_pos,
                   nes.pulse[1].enabled, nes.pulse[1].length, nes.pulse[1].duty,
                   nes.pulse[1].vol_period, nes.pulse[1].const_vol, nes.pulse[1].halt,
                   nes.pulse[1].sweep_en, nes.pulse[1].sweep_neg, nes.pulse[1].sweep_shift,
                   nes.pulse[1].sweep_period, nes.pulse[1].timer, nes.pulse[1].duty_pos,
                   nes.tri.enabled, nes.tri.length, nes.tri.linear_counter,
                   nes.tri.linear_reload, nes.tri.control, nes.tri.linear_load,
                   nes.tri.timer, nes.tri.step,
                   nes.noise.enabled, nes.noise.length, nes.noise.vol_period,
                   nes.noise.const_vol, nes.noise.halt, nes.noise.mode,
                   nes.noise.period_idx, nes.noise.shift_reg,
                   nes.dmc.enabled, nes.dmc.irq_enable, nes.dmc.loop,
                   nes.dmc.period_idx, nes.dmc.output_level, nes.dmc.sample_addr,
                   nes.dmc.sample_len, nes.dmc.cur_addr, nes.dmc.bytes_left,
                   nes.dmc.sample_buf_full, nes.dmc.bits_left, nes.dmc.silence);
        }
    } else {
        printf("CPU after %d steps: PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%d\n",
               steps, nes.pc, nes.a, nes.x, nes.y, nes.flags, nes.sp, nes.cycles);
    }

    int rc = 0;
    if (expect_pass)
        rc = (nes.sram[0] == 0) ? 0 : 1;

    free_rom(&nes);
    return rc;
}
