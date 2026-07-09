/*
 * Exact-ish AccuracyCoin DMASync_50 + DMA+$4015 test 2 phase probe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static int g_log_dma;
static s32 g_dma_log_t[512];
static u16 g_dma_log_halt[512];
static int g_dma_log_n;
static int g_dma_log_stall[512];

void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    if (!g_log_dma || g_dma_log_n >= 512)
        return;
    g_dma_log_t[g_dma_log_n] = nes->cycles - stall;
    g_dma_log_halt[g_dma_log_n] = halt;
    g_dma_log_stall[g_dma_log_n] = stall;
    g_dma_log_n++;
}

static void step1(struct NES *nes) {
    nes->dmc.ticks_exec = 0;
    int before = nes->cycles;
    cpu_step(nes);
    int ran = nes->cycles - before;
    if (ran > 0) {
        while (nes->dmc.ticks_exec < ran)
            dmc_tick(nes);
        apu_step(nes, ran);
        nes->total_cycles += ran;
    }
}

static void install_delay_sub(u8 *prg, u16 cpu_addr, int body_cycles) {
    /* body_cycles NOPs + RTS; JSR+RTS=12, so JSR costs body+12 */
    u32 off = (u32)(cpu_addr - 0x8000);
    int nops = body_cycles / 2;
    for (int i = 0; i < nops; i++)
        prg[off++] = 0xEA;
    prg[off++] = 0x60;
}

static u16 emit_jsr(u8 *ram, u16 pc, u16 abs) {
    ram[pc++] = 0x20;
    ram[pc++] = (u8)(abs & 0xFF);
    ram[pc++] = (u8)(abs >> 8);
    return pc;
}

static u16 emit_nops(u8 *ram, u16 pc, int n) {
    for (int i = 0; i < n; i++)
        ram[pc++] = 0xEA;
    return pc;
}

/* Burn exactly n cycles with JSRs to $8100 (100 cyc) + NOPs. n>=0 */
static u16 emit_delay(u8 *ram, u16 pc, int n) {
    while (n >= 100) {
        pc = emit_jsr(ram, pc, 0x8100);
        n -= 100;
    }
    if (n >= 2)
        pc = emit_nops(ram, pc, n / 2);
    if (n & 1)
        ram[pc++] = 0xEA; /* 2 cy — off by 1 if odd; caller should pass even */
    return pc;
}

int main(int argc, char **argv) {
    int extra = 0;
    if (argc > 1)
        extra = atoi(argv[1]);

    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    memset(prg + 0x7FC0, 0x00, 64);
    /* $8100: 88-cycle body + RTS → JSR total 100 */
    install_delay_sub(prg, 0x8100, 88);

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.fc_step[0][0] = 7457;
    nes.fc_step[0][1] = 14916;
    nes.fc_step[0][2] = 22371;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.mapper = 0;
    nes.chr_enable = 1;
    nes.audio_handle = -1;
    nes.noise.shift_reg = 1;

    u8 *r = nes.ram;
    u16 pc = 0x0200;

    /* --- DMASync setup (rate F loop, FFC0, len1, enable) --- */
    r[pc++] = 0xA9; r[pc++] = 0x4F;
    r[pc++] = 0x8D; r[pc++] = 0x10; r[pc++] = 0x40;
    r[pc++] = 0xA9; r[pc++] = 0x00;
    r[pc++] = 0x8D; r[pc++] = 0x11; r[pc++] = 0x40;
    r[pc++] = 0xA9; r[pc++] = 0xFF;
    r[pc++] = 0x8D; r[pc++] = 0x12; r[pc++] = 0x40;
    r[pc++] = 0xA9; r[pc++] = 0x00;
    r[pc++] = 0x8D; r[pc++] = 0x13; r[pc++] = 0x40;
    r[pc++] = 0xA9; r[pc++] = 0x10;
    r[pc++] = 0x8D; r[pc++] = 0x15; r[pc++] = 0x40;
    r[pc++] = 0xEA;
    r[pc++] = 0xEA;

    /* open-bus lock */
    u16 lock = pc;
    r[pc++] = 0xAD; r[pc++] = 0x00; r[pc++] = 0x40;
    r[pc++] = 0xD0; r[pc++] = (u8)(lock - (pc + 1));

    /*
     * DMASync tail after catch (26 cycles in AccuracyCoin counting from BNE):
     *   LDA #$0F STA $4010  (6)
     *   LDA $00             (3)
     *   LDA copy_a — use LDA #$00 (2) as stand-in  (we don't need Copy_A)
     *   — AccuracyCoin: 2+4+3+4+6 = 19 from after BNE to after RTS of DMASync
     *   Comment says 26 from a different origin; next DMA 432 after catch.
     *
     * After catch DMA, next reload ~432 later. DMASync leaves 406 after its RTS.
     * DMASync_50: clockslide 350 + RTS 6 → 50 remaining.
     */
    r[pc++] = 0xA9; r[pc++] = 0x0F;
    r[pc++] = 0x8D; r[pc++] = 0x10; r[pc++] = 0x40;
    r[pc++] = 0xA5; r[pc++] = 0x00; /* LDA $00 */
    r[pc++] = 0xA9; r[pc++] = 0x00; /* stand-in for LDA Copy_A */

    /*
     * DMASync_50 burns 350 to leave 50 until DMA. Empirically our catch→here
     * residual is ~64 not 56, so burn 350+14=364 to leave ~50.
     */
    u16 d50 = pc;
    int sync50_burn = 364;
    if (argc > 2)
        sync50_burn = atoi(argv[2]);
    pc = emit_delay(r, pc, sync50_burn);

    /* T=0 equivalent: 50 cycles until DMA. Test body: */
    r[pc++] = 0xA9; r[pc++] = 0x4F;
    r[pc++] = 0x8D; r[pc++] = 0x10; r[pc++] = 0x40; /* 6 */
    r[pc++] = 0xA9; r[pc++] = 0x00;
    r[pc++] = 0x8D; r[pc++] = 0x17; r[pc++] = 0x40; /* 6 — total 12 */

    /*
     * AccuracyCoin: 29780+200+16 = 29996 more? Comment 29892 for 12+29780+100.
     * Use 29780+200+16 - 0 = 29996 from after the 12, OR 29892-12=29880 from T=0 after 12.
     * From T=0: 12 + X = time to BIT. Use X = 29892 - 12 = 29880 (comment path with CS200=100).
     * Also try 30008-12=29996 (CS200=200).
     */
    int body = 29880 + extra;
    if (body & 1)
        body++;
    pc = emit_delay(r, pc, body);

    u16 bit_pc = pc;
    r[pc++] = 0x2C; r[pc++] = 0x15; r[pc++] = 0x40;
    r[pc++] = 0x00;

    printf("pcs: lock=%04X d50=%04X bit=%04X end=%04X body_delay=%d extra=%d\n", lock, d50,
           bit_pc, pc, body, extra);
    if (pc >= 0x800) {
        printf("RAM overflow\n");
        return 1;
    }

    nes.pc = 0x0200;
    nes.sp = 0xFD;
    nes.flags = 0x24;
    nes.a = 0x40;
    nes.cpu_data_bus = 0x40;
    nes.cycles = 0;
    nes.total_cycles = 0;
    g_log_dma = 0;

    for (int i = 0; i < 5000000 && nes.pc < lock + 5; i++) {
        u16 p = nes.pc;
        step1(&nes);
        if (p >= lock && p < lock + 5 && nes.a == 0 && (nes.flags & 0x02)) {
            /* may still be on BNE */
        }
        if (nes.pc > lock + 5 && nes.a == 0)
            break;
    }
    /* ensure we're past lock */
    for (int i = 0; i < 20 && nes.pc <= lock + 5; i++)
        step1(&nes);

    printf("after lock region pc=%04X a=%02X cy=%d\n", nes.pc, nes.a, (int)nes.cycles);

    g_log_dma = 1;
    g_dma_log_n = 0;
    s32 t0_mark = -1;
    s32 t_bit = -1;
    u8 v = 0xFF;

    /* Mark T=0 as first time we reach instruction after 350-delay (test body start).
     * That's the LDA #$4F after emit_delay 350. Find by scanning: after d50 delay. */
    u16 body_start = d50;
    /* skip the delay emission — body_start should be first instr after 350 delay.
     * Recompute: d50 is start of delay; delay is series of JSR/NOP. Easier: bit_pc - 3 - body/2...
     * Store body_start properly. */
    /* From construction: after emit_delay 350, next bytes are A9 4F. Search from d50. */
    body_start = d50;
    while (body_start < bit_pc && !(r[body_start] == 0xA9 && r[body_start + 1] == 0x4F &&
                                    r[body_start + 2] == 0x8D))
        body_start++;
    printf("body_start=%04X\n", body_start);

    for (int i = 0; i < 3000000; i++) {
        u16 p0 = nes.pc;
        if (p0 == body_start && t0_mark < 0)
            t0_mark = nes.cycles;
        if (p0 == bit_pc)
            t_bit = nes.cycles;
        step1(&nes);
        if (p0 == bit_pc) {
            v = (u8)((nes.flags >> 6) & 1);
            break;
        }
        if (nes.pc >= 0xFF00)
            break;
    }

    printf("T0=%d BIT=%d delta=%d V=%d (want 0)\n", (int)t0_mark, (int)t_bit,
           (int)(t_bit - t0_mark), v);

    printf("DMA n=%d; first 8 after T0 and last 8:\n", g_dma_log_n);
    int shown = 0;
    for (int i = 0; i < g_dma_log_n && shown < 8; i++) {
        int rel0 = t0_mark >= 0 ? (int)(g_dma_log_t[i] - t0_mark) : -99999;
        if (rel0 < -20)
            continue;
        printf("  t=%d halt=%04X relT0=%+d\n", (int)g_dma_log_t[i], g_dma_log_halt[i], rel0);
        shown++;
    }
    printf("  ...\n");
    int s = g_dma_log_n > 8 ? g_dma_log_n - 8 : 0;
    for (int i = s; i < g_dma_log_n; i++) {
        int rel0 = t0_mark >= 0 ? (int)(g_dma_log_t[i] - t0_mark) : 0;
        int relb = t_bit >= 0 ? (int)(g_dma_log_t[i] - t_bit) : 0;
        printf("  t=%d halt=%04X relT0=%+d relBIT=%+d%s\n", (int)g_dma_log_t[i], g_dma_log_halt[i],
               rel0, relb, g_dma_log_halt[i] == 0x4015 ? " $4015" : "");
    }
    if (g_dma_log_n >= 2) {
        printf("iv:");
        for (int i = (g_dma_log_n > 8 ? g_dma_log_n - 8 : 1); i < g_dma_log_n; i++)
            printf(" %d", (int)(g_dma_log_t[i] - g_dma_log_t[i - 1]));
        printf("\n");
    }
    int hit4015 = 0;
    for (int i = 0; i < g_dma_log_n; i++)
        if (g_dma_log_halt[i] == 0x4015)
            hit4015 = 1;
    printf("%s\n", hit4015 ? "HIT $4015" : "NO $4015 halt");
    return v == 0 ? 0 : 2;
}
