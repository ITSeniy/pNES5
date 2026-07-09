#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

static s32 g_tick;
static s32 g_req_tick[16];
static s32 g_get_tick[16];
static int g_req_n, g_get_n;

/* wrap dmc_tick counting — we can't, so count in phase log and approx */

void dmc_phase_log_service(struct NES *n, u16 h, int s) {
    /* cycles is the best host-side monotonic clock (1:1 with dmc_tick pad). */
    if (g_get_n < 16)
        g_get_tick[g_get_n] = n->cycles - s;
    g_get_n++;
    (void)h;
}

static void step1(struct NES *n) {
    n->dmc.ticks_exec = 0;
    int b = n->cycles;
    int last_pend = n->dmc.dma_pending;
    cpu_step(n);
    int r = n->cycles - b;
    if (r > 0) {
        while (n->dmc.ticks_exec < r)
            dmc_tick(n);
        apu_step(n, r);
        n->total_cycles += r;
    }
    if (n->dmc.dma_pending && !last_pend && g_req_n < 16)
        g_req_tick[g_req_n++] = n->cycles;
}

int main(void) {
    u8 prg[0x8000];
    memset(prg, 0xEA, sizeof(prg));
    memset(prg + 0x7FC0, 0, 0x3A);
    prg[0x7FFC] = 0;
    prg[0x7FFD] = 0x80;
    prg[0] = 0x40;

    struct NES nes;
    memset(&nes, 0, sizeof(nes));
    nes.cpu_freq = 1789773;
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    nes.noise.shift_reg = 1;
    nes.audio_handle = -1;
    nes.fc_step[0][0] = 7457;
    nes.fc_step[0][1] = 14916;
    nes.fc_step[0][2] = 22371;
    nes.fc_step[0][3] = 29831;
    nes.fc_step[0][4] = 29832;
    nes.fc_step[0][5] = 29833;

    u8 *r = nes.ram;
    u16 pc = 0x200;
    r[pc++] = 0xA9;
    r[pc++] = 0x4F;
    r[pc++] = 0x8D;
    r[pc++] = 0x10;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0xFF;
    r[pc++] = 0x8D;
    r[pc++] = 0x12;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0x00;
    r[pc++] = 0x8D;
    r[pc++] = 0x13;
    r[pc++] = 0x40;
    r[pc++] = 0xA9;
    r[pc++] = 0x10;
    r[pc++] = 0x8D;
    r[pc++] = 0x15;
    r[pc++] = 0x40;
    u16 spin = pc;
    r[pc++] = 0xEA;
    r[pc++] = 0x4C;
    r[pc++] = (u8)(spin & 0xFF);
    r[pc++] = (u8)(spin >> 8);

    nes.pc = 0x200;
    nes.sp = 0xFD;
    nes.flags = 0x24;

    for (int i = 0; i < 30000 && g_get_n < 8; i++)
        step1(&nes);

    printf("get intervals (ticks): ");
    for (int i = 1; i < g_get_n && i < 16; i++)
        printf("%d ", (int)(g_get_tick[i] - g_get_tick[i - 1]));
    printf("\nreq intervals: ");
    for (int i = 1; i < g_req_n && i < 16; i++)
        printf("%d ", (int)(g_req_tick[i] - g_req_tick[i - 1]));
    printf("\ngets=%d reqs=%d ticks=%d\n", g_get_n, g_req_n, (int)g_tick);
    return 0;
}
