#include <stdio.h>
#include <string.h>
#include "nes.h"

void apu_write_reg(struct NES *nes, u16 addr, u8 val) {
    (void)nes;
    (void)addr;
    (void)val;
}

void dmc_service_dma(struct NES *nes) {
    if (!nes || !nes->dmc.dma_pending || nes->dmc.dma_reentry)
        return;
    if (nes->dmc.dma_abort) {
        nes->dmc.dma_reentry = 1;
        nes->dmc.dma_pending = 0;
        nes->dmc.dma_abort = 0;
        nes->cycles += 1;
        if (nes->dmc.halt_addr != 0xFFFF)
            (void)cpu_read_nodma(nes, nes->dmc.halt_addr);
        nes->dmc.dma_reentry = 0;
        return;
    }
    if (!nes->dmc.enabled || nes->dmc.bytes_left == 0 || nes->dmc.sample_buf_full) {
        nes->dmc.dma_pending = 0;
        return;
    }
    nes->dmc.dma_reentry = 1;
    nes->dmc.dma_pending = 0;
    nes->dmc.sh_h_suppress = 1;
    nes->dmc.sh_suppress_until = nes->total_cycles + 120;
    /* Match production: load=3 cyc (2 dummies), reload=4 (3 dummies). */
    int stall = nes->dmc.dma_is_load ? 3 : 4;
    nes->dmc.dma_is_load = 0;
    int dummies = stall - 1;
    u16 halt = nes->dmc.halt_addr;
    nes->cycles += stall;
    for (int i = 0; i < dummies; i++) {
        if (halt != 0xFFFF)
            (void)cpu_read_nodma(nes, halt);
    }
    if (nes->prg && nes->prg_size > 0 && nes->dmc.cur_addr >= 0x8000)
        nes->cpu_data_bus = nes->prg[(nes->dmc.cur_addr - 0x8000) % (u32)nes->prg_size];
    else
        nes->cpu_data_bus = 0;
    nes->dmc.sample_buf = nes->cpu_data_bus;
    nes->dmc.sample_buf_full = 1;
    nes->dmc.bytes_left--;
    nes->dmc.dma_reentry = 0;
}

void dmc_tick(struct NES *nes) {
    (void)nes;
}

void apu_on_cpu_cycle_begin(struct NES *nes) {
    (void)nes;
}

void ppu_on_mask_write(struct NES *nes, u8 prev_mask) {
    (void)nes;
    (void)prev_mask;
}

static int expect_u8(const char *name, u8 got, u8 want) {
    if (got == want) return 0;
    printf("%s: got 0x%02X, want 0x%02X\n", name, got, want);
    return 1;
}

static int expect_int(const char *name, int got, int want) {
    if (got == want) return 0;
    printf("%s: got %d, want %d\n", name, got, want);
    return 1;
}

static int test_cpu_open_bus(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0xEA, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;

    /* Absolute open-bus read returns floating data bus (last high-byte fetch). */
    nes.cpu_data_bus = 0x50;
    int fails = expect_u8("open bus $5000", cpu_read(&nes, 0x5000), 0x50);
    nes.cpu_data_bus = 0x46;
    fails += expect_u8("open bus $4654", cpu_read(&nes, 0x4654), 0x46);
    nes.cpu_data_bus = 0x5F;
    fails += expect_u8("open bus $5FFF", cpu_read(&nes, 0x5FFF), 0x5F);

    /* Page-cross: bus stays at pre-fix high byte */
    nes.cpu_data_bus = 0x50;
    fails += expect_u8("open bus page-cross keeps old high", cpu_read(&nes, 0x5108), 0x50);

    /* $4015 does not update the *external* data bus; bit 5 is from *internal*. */
    nes.cpu_data_bus = 0x40;
    nes.cpu_db_internal = 0x00;
    u8 r4015 = cpu_read(&nes, 0x4015);
    fails += expect_u8("4015 leaves external bus", nes.cpu_data_bus, 0x40);
    fails += expect_u8("4015 bit5 from internal clear", r4015 & 0x20, 0x00);
    nes.cpu_data_bus = 0x00; /* external alone must not set bit5 */
    nes.cpu_db_internal = 0x20;
    r4015 = cpu_read(&nes, 0x4015);
    fails += expect_u8("4015 bit5 from internal set", r4015 & 0x20, 0x20);
    fails += expect_u8("4015 leaves external after bit5", nes.cpu_data_bus, 0x00);

    /* Writes always update the bus */
    cpu_write(&nes, 0x4015, 0xAB);
    fails += expect_u8("write updates bus", nes.cpu_data_bus, 0xAB);

    /* Controller: only D0 driven; D1-D7 float from data bus */
    nes.cpu_data_bus = 0x40;
    nes.pad_state = 0;
    nes.pad_shift = 0;
    nes.pad_strobe = 0;
    fails += expect_u8("4016 open bus bits", cpu_read(&nes, 0x4016) & 0xE0, 0x40);
    return fails;
}

static int test_cpu_open_bus_lda(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0xEA, sizeof(prg));
    /* At $8000: LDA $5000 ; NOP */
    prg[0x0000] = 0xAD;
    prg[0x0001] = 0x00;
    prg[0x0002] = 0x50;
    prg[0x0003] = 0xEA;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.pc = 0x8000;
    nes.flags = F_I | F_U;
    nes.sp = 0xFD;

    cpu_step(&nes); /* LDA $5000 */
    int fails = expect_u8("LDA $5000 open bus A", nes.a, 0x50);
    fails += expect_u8("LDA $5000 leaves bus", nes.cpu_data_bus, 0x50);
    return fails;
}

/* AccuracyCoin Open Bus test 4: JSR $5600 runs LSR $56,X then RTS via open bus. */
/* SHX: normal H+1 AND. */
static int test_shx_dmc_suppress(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0xEA, sizeof(prg));
    prg[0] = 0x9E;
    prg[1] = 0x00;
    prg[2] = 0x05;
    /* Also put zeros at end of PRG for DMC sample @$FFC0 */
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.pc = 0x8000;
    nes.sp = 0xFD;
    nes.flags = F_I | F_U;
    nes.x = 0xA5;
    nes.y = 0;
    nes.ram[0x500] = 0x5A;

    cpu_step(&nes);
    int fails = expect_u8("SHX normal value", nes.ram[0x500], 0x04);

    /* Recent DMC DMA window → H suppressed */
    nes.pc = 0x8000;
    nes.ram[0x500] = 0x5A;
    nes.dmc.sh_h_suppress = 1;
    nes.dmc.sh_suppress_until = nes.total_cycles + 100;
    cpu_step(&nes);
    fails += expect_u8("SHX after DMA window is X", nes.ram[0x500], 0xA5);
    return fails;
}

/* Open-bus $4000 after a DMA get leaves sample on the data bus. */
static int test_dmc_openbus_ld4000(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0x00, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xFFC0;
    nes.dmc.sample_addr = 0xFFC0;
    nes.dmc.sample_len = 1;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.halt_addr = 0xFFFF;
    nes.cpu_data_bus = 0x40;

    /* DMA get puts sample $00 on the bus (as during LDA $4000 final cycle). */
    dmc_service_dma(&nes);
    int fails = expect_u8("DMA sample on bus", nes.cpu_data_bus, 0x00);
    fails += expect_int("SH suppress after DMA", nes.dmc.sh_h_suppress, 1);
    /* Open-bus read of $4000 must return that sample. */
    fails += expect_u8("open bus $4000", cpu_read(&nes, 0x4000), 0x00);
    return fails;
}

/* Reload DMA during $2007: 3 dummy reads + real → buffer skips ahead. */
static int test_dmc_dummy_2007(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    nes.mirror = 1;

    /* Nametable $2000..$2004 = 0,1,2,3,4 (horizontal mirror → vram[]) */
    for (u8 i = 0; i < 5; i++)
        nes.vram[i] = i;
    nes.read_buf = 0; /* prep left buffer=0, v at $2001 */
    nes.vram_addr = 0x2001;

    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1; /* reload → 3 dummies */

    /* cpu_read $2007 services DMA first (3 dummies of $2007) then real read */
    u8 a = cpu_read(&nes, 0x2007);
    /*
     * dummies: ret 0 load1, ret1 load2, ret2 load3; real ret 3.
     * AccuracyCoin wants A >= 3 for DMA+$2007R.
     */
    int fails = expect_u8("DMA+$2007 dummy advance", a, 3);
    fails += expect_int("v after 4 $2007 reads", (int)nes.vram_addr, 0x2005);
    return fails;
}

/* Load DMA after $4015 must not halt on the next opcode — delay 3 reads. */
static int test_dmc_load_delay_2002(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0xEA, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    nes.ppu_status = 0x80; /* VBlank set */
    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_addr = 0xC000;
    nes.dmc.sample_len = 1;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.dma_is_load = 1;
    nes.dmc.dma_halt_delay = 3;

    /* 3 dummy bus reads (like op/adl/adh) — must NOT clear VBlank yet */
    (void)cpu_read(&nes, 0x8000);
    (void)cpu_read(&nes, 0x8001);
    (void)cpu_read(&nes, 0x8002);
    int fails = expect_u8("VBlank still set after delay", nes.ppu_status & 0x80, 0x80);
    fails += expect_int("DMA still pending", nes.dmc.dma_pending, 1);

    /* 4th read is $2002 — load DMA dummies clear VBlank before real read */
    u8 st = cpu_read(&nes, 0x2002);
    fails += expect_u8("VBlank cleared by load DMA dummies", nes.ppu_status & 0x80, 0);
    fails += expect_u8("$2002 after DMA dummies has VBlank clear", st & 0x80, 0);
    return fails;
}

static int test_cpu_open_bus_jsr_execute(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0xEA, sizeof(prg));
    /* $8000: JSR $5600 ; LDA $56 ; ... */
    prg[0x0000] = 0x20; /* JSR */
    prg[0x0001] = 0x00;
    prg[0x0002] = 0x56;
    prg[0x0003] = 0xA5; /* LDA zp */
    prg[0x0004] = 0x56;
    prg[0x0005] = 0x00; /* BRK halt */

    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.pc = 0x8000;
    nes.flags = F_I | F_U;
    nes.sp = 0xFD;
    nes.x = 0;
    nes.ram[0x56] = 0xC0; /* becomes $60 (RTS) after LSR */

    cpu_step(&nes); /* JSR $5600 — bus must end as $56 */
    int fails = expect_u8("JSR leaves ADH on bus", nes.cpu_data_bus, 0x56);
    fails += expect_int("JSR PC open bus", nes.pc, 0x5600);

    cpu_step(&nes); /* LSR $56,X from open bus */
    fails += expect_u8("LSR result in zp", nes.ram[0x56], 0x60);
    fails += expect_u8("LSR write on bus", nes.cpu_data_bus, 0x60);

    cpu_step(&nes); /* RTS from open bus ($60) */
    fails += expect_int("RTS back after JSR", nes.pc, 0x8003);

    cpu_step(&nes); /* LDA $56 */
    fails += expect_u8("zp after open-bus LSR", nes.a, 0x60);
    return fails;
}

static int test_controller_strobe(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));

    nes.pad_state = 0xA5;
    cpu_write(&nes, 0x4016, 1);
    int fails = 0;
    fails += expect_u8("strobe read 1", cpu_read(&nes, 0x4016) & 1, 1);
    fails += expect_u8("strobe read 2", cpu_read(&nes, 0x4016) & 1, 1);

    cpu_write(&nes, 0x4016, 0);
    u8 bits[10];
    for (int i = 0; i < 10; i++)
        bits[i] = cpu_read(&nes, 0x4016) & 1;

    u8 want[10] = {1,0,1,0,0,1,0,1,1,1};
    for (int i = 0; i < 10; i++)
        fails += expect_u8("latched controller bit", bits[i], want[i]);
    return fails;
}

static int test_mapper_bank_wrapping(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.chr_enable = 1;

    /* UxROM: bank count derived from prg_size (16KB units) */
    nes.mapper = 2;
    nes.prg_size = 0xC000; /* 3 x 16KB */
    cpu_write(&nes, 0x8000, 5);
    int fails = expect_int("UxROM non-power-of-two bank", nes.prg_bank, 2);

    nes.mapper = 3;
    nes.chr_size = 0x6000;
    cpu_write(&nes, 0x8000, 5);
    fails += expect_int("CNROM non-power-of-two bank", nes.chr_bank, 2);

    nes.mapper = 7;
    nes.prg_size = 0x18000;
    cpu_write(&nes, 0x8000, 5);
    fails += expect_int("AxROM non-power-of-two bank", nes.prg_bank, 2);

    nes.mapper = 11;
    nes.prg_size = 0x18000;
    nes.chr_size = 0x6000;
    cpu_write(&nes, 0x8000, 0x52);
    fails += expect_int("Color Dreams PRG bank", nes.prg_bank, 2);
    fails += expect_int("Color Dreams CHR bank", nes.chr_bank, 2);

    nes.mapper = 70;
    nes.prg_size = 5 * 0x4000;
    nes.chr_size = 0x6000;
    cpu_write(&nes, 0x8000, 0x72);
    fails += expect_int("Mapper 70 PRG bank", nes.prg_bank, 2);
    fails += expect_int("Mapper 70 CHR bank", nes.chr_bank, 2);

    nes.mapper = 71;
    nes.prg_size = 3 * 0x4000;
    cpu_write(&nes, 0xC000, 5);
    fails += expect_int("Camerica PRG bank", nes.prg_bank, 2);
    cpu_write(&nes, 0x9000, 0x10);
    fails += expect_int("Camerica one-screen mirror", nes.mirror, 3);

    nes.mapper = 78;
    nes.prg_size = 5 * 0x4000;
    nes.chr_size = 0x6000;
    cpu_write(&nes, 0x8000, 0x7E);
    fails += expect_int("Mapper 78 PRG bank", nes.prg_bank, 1);
    fails += expect_int("Mapper 78 CHR bank", nes.chr_bank, 1);
    fails += expect_int("Mapper 78 one-screen mirror", nes.mirror, 3);

    nes.mapper = 93;
    nes.prg_size = 5 * 0x4000;
    cpu_write(&nes, 0x8000, 0x70);
    fails += expect_int("Sunsoft-2 PRG bank", nes.prg_bank, 2);

    nes.mapper = 140;
    nes.prg_size = 0x18000;
    nes.chr_size = 0x6000;
    cpu_write(&nes, 0x6000, 0x52);
    fails += expect_int("Jaleco JF-11 PRG bank", nes.prg_bank, 1);
    fails += expect_int("Jaleco JF-11 CHR bank", nes.chr_bank, 2);

    nes.mapper = 152;
    nes.prg_size = 5 * 0x4000;
    nes.chr_size = 0x6000;
    cpu_write(&nes, 0x8000, 0xE2);
    fails += expect_int("Mapper 152 PRG bank", nes.prg_bank, 1);
    fails += expect_int("Mapper 152 CHR bank", nes.chr_bank, 2);
    fails += expect_int("Mapper 152 one-screen mirror", nes.mirror, 3);
    return fails;
}

static int test_chr_ram_banking(void) {
    struct NES nes;
    u8 chr[0x2000];
    memset(&nes, 0, sizeof(nes));
    memset(chr, 0, sizeof(chr));
    nes.mapper = 4;
    nes.chr = chr;
    nes.chr_size = 0x2000;
    nes.chr_is_ram = 1;
    nes.chr_enable = 1;
    nes.mmc3_regs[2] = 4; /* 1KB bank at $1000 in mode 0 */
    nes.mmc3_chr_mode = 0;

    /* Write through banked CHR-RAM window $1000 (reg2=4 -> offset 0x1000) */
    ppu_write(&nes, 0x1000, 0xAB);
    int fails = expect_u8("CHR-RAM banked write", chr[0x1000], 0xAB);
    fails += expect_u8("CHR-RAM banked read", ppu_read(&nes, 0x1000), 0xAB);
    return fails;
}

static int test_fme7_prg_slots(void) {
    struct NES nes;
    u8 prg[0x10000];
    memset(&nes, 0, sizeof(nes));
    for (int i = 0; i < 0x10000; i++) prg[i] = (u8)(i >> 8);
    nes.mapper = 69;
    nes.prg = prg;
    nes.prg_size = 0x10000;
    nes.fme7_prg[1] = 2; /* $8000 -> bank 2 */
    nes.fme7_prg[2] = 3;
    nes.fme7_prg[3] = 4;
    nes.fme7_ram_mode = 0xC0;
    int fails = expect_u8("FME-7 $8000 bank", cpu_read(&nes, 0x8000), 0x40); /* bank2 * 0x2000 */
    fails += expect_u8("FME-7 $A000 bank", cpu_read(&nes, 0xA000), 0x60);
    fails += expect_u8("FME-7 $C000 bank", cpu_read(&nes, 0xC000), 0x80);
    fails += expect_u8("FME-7 $E000 fixed", cpu_read(&nes, 0xE000), 0xE0); /* last 8KB */
    return fails;
}

static int test_mmc1_prg_ram_disable(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.mapper = 1;
    nes.mmc1_prg = 0x00;
    nes.sram[0] = 0x5A;
    int fails = expect_u8("MMC1 PRG-RAM enabled", cpu_read(&nes, 0x6000), 0x5A);
    nes.mmc1_prg = 0x10;
    fails += expect_u8("MMC1 PRG-RAM disabled read", cpu_read(&nes, 0x6000), 0);
    cpu_write(&nes, 0x6000, 0x99);
    fails += expect_u8("MMC1 PRG-RAM disabled write", nes.sram[0], 0x5A);
    return fails;
}

static int test_mmc3_wram_and_fourscreen(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.mapper = 4;
    nes.mmc3_wram_enable = 1;
    nes.mmc3_wram_protect = 0;
    nes.sram[1] = 0x11;
    nes.mirror = 4; /* four-screen */

    cpu_write(&nes, 0xA000, 1); /* should not change four-screen */
    int fails = expect_int("MMC3 four-screen lock", nes.mirror, 4);

    cpu_write(&nes, 0xA001, 0x00); /* disable WRAM */
    fails += expect_u8("MMC3 WRAM disabled", cpu_read(&nes, 0x6001), 0);

    cpu_write(&nes, 0xA001, 0x80); /* enable, not protect */
    fails += expect_u8("MMC3 WRAM re-enabled", cpu_read(&nes, 0x6001), 0x11);

    cpu_write(&nes, 0xA001, 0xC0); /* enable + protect */
    cpu_write(&nes, 0x6001, 0x22);
    fails += expect_u8("MMC3 WRAM write protect", nes.sram[1], 0x11);
    return fails;
}

static int test_mmc1_consecutive_write(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.mapper = 1;
    nes.mmc1_ctrl = 0x0C;
    nes.mmc1_last_write_cycle = -2;
    nes.total_cycles = 100;

    /* Five bits for control register via $8000 (spaced like real STA writes) */
    for (int i = 0; i < 5; i++) {
        nes.total_cycles = 100 + i * 4;
        cpu_write(&nes, 0x8000, 0x00); /* shift in 0s → ctrl=0, one-screen lower */
    }
    int fails = expect_u8("MMC1 5-bit load", nes.mmc1_ctrl, 0x00);
    fails += expect_int("MMC1 mirror from ctrl", nes.mirror, 2);

    /* Same total_cycles second write (RMW style) must be ignored */
    nes.total_cycles = 200;
    nes.mmc1_last_write_cycle = -2;
    nes.mmc1_count = 0;
    nes.mmc1_shift = 0;
    cpu_write(&nes, 0x8000, 1);
    cpu_write(&nes, 0x8000, 1); /* ignored: same total_cycles */
    fails += expect_int("MMC1 consecutive count", nes.mmc1_count, 1);
    return fails;
}

static int test_mapper_nina_and_cprom(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.chr_enable = 1;

    nes.mapper = 79;
    nes.prg_size = 0x10000;
    nes.chr_size = 0x10000; /* 8 x 8KB */
    cpu_write(&nes, 0x4100, 0x1D); /* PRG=1, CHR=5 */
    int fails = expect_int("NINA-03 PRG", nes.prg_bank, 1);
    fails += expect_int("NINA-03 CHR", nes.chr_bank, 5);

    nes.mapper = 113;
    nes.prg_size = 0x40000;
    nes.chr_size = 0x20000;
    cpu_write(&nes, 0x4100, 0xDA); /* V mirror, PRG=3, CHR=10 */
    fails += expect_int("113 PRG", nes.prg_bank, 3);
    fails += expect_int("113 CHR", nes.chr_bank, 2 | 8);
    fails += expect_int("113 mirror", nes.mirror, 1);

    nes.mapper = 13;
    nes.chr_size = 0x4000;
    cpu_write(&nes, 0x8000, 0x03);
    fails += expect_int("CPROM CHR bank", nes.chr_bank, 3);
    return fails;
}

static int test_mapper185_chr_disable(void) {
    struct NES nes;
    u8 chr[0x2000];
    memset(&nes, 0, sizeof(nes));
    memset(chr, 0x77, sizeof(chr));
    nes.mapper = 185;
    nes.chr = chr;
    nes.chr_size = 0x2000;
    nes.chr_enable = 0;
    int fails = expect_u8("185 disabled open bus", ppu_read(&nes, 0x0000), 0xFF);
    cpu_write(&nes, 0x8000, 0x11);
    fails += expect_int("185 enable flag", nes.chr_enable, 1);
    fails += expect_u8("185 enabled read", ppu_read(&nes, 0x0000), 0x77);
    cpu_write(&nes, 0x8000, 0x00);
    fails += expect_int("185 disable again", nes.chr_enable, 0);
    return fails;
}

static int test_four_screen_mirroring(void) {
    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.mirror = 4;

    ppu_write(&nes, 0x2000, 0x11);
    ppu_write(&nes, 0x2400, 0x22);
    ppu_write(&nes, 0x2800, 0x33);
    ppu_write(&nes, 0x2C00, 0x44);

    int fails = 0;
    fails += expect_u8("four-screen nt0", ppu_read(&nes, 0x2000), 0x11);
    fails += expect_u8("four-screen nt1", ppu_read(&nes, 0x2400), 0x22);
    fails += expect_u8("four-screen nt2", ppu_read(&nes, 0x2800), 0x33);
    fails += expect_u8("four-screen nt3", ppu_read(&nes, 0x2C00), 0x44);
    return fails;
}

int main(void) {
    int fails = 0;
    fails += test_controller_strobe();
    fails += test_cpu_open_bus();
    fails += test_cpu_open_bus_lda();
    fails += test_cpu_open_bus_jsr_execute();
    fails += test_shx_dmc_suppress();
    fails += test_dmc_openbus_ld4000();
    fails += test_dmc_dummy_2007();
    fails += test_dmc_load_delay_2002();
    fails += test_mapper_bank_wrapping();
    fails += test_chr_ram_banking();
    fails += test_fme7_prg_slots();
    fails += test_mmc1_prg_ram_disable();
    fails += test_mmc3_wram_and_fourscreen();
    fails += test_mmc1_consecutive_write();
    fails += test_mapper185_chr_disable();
    fails += test_mapper_nina_and_cprom();
    fails += test_four_screen_mirroring();

    if (fails) {
        printf("core_smoke: %d failure(s)\n", fails);
        return 1;
    }

    printf("core_smoke: ok\n");
    return 0;
}
