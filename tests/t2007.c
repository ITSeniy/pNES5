#include <stdio.h>
#include <string.h>
#include "nes.h"
#include "mapper.h"

void dmc_phase_log_service(struct NES *n, u16 h, int s) {
    printf("SVC halt=%04X st=%d pend_after will clear\n", h, s);
    (void)n;
}

int main(void) {
    struct NES nes;
    u8 prg[0x8000];
    memset(&nes, 0, sizeof(nes));
    memset(prg, 0, sizeof(prg));
    nes.prg = prg;
    nes.prg_size = 0x8000;
    nes.chr_enable = 1;
    nes.mirror = 1;
    for (u8 i = 0; i < 5; i++)
        nes.vram[i] = i;
    nes.read_buf = 0;
    nes.vram_addr = 0x2001;
    nes.dmc.enabled = 1;
    nes.dmc.bytes_left = 1;
    nes.dmc.cur_addr = 0xC000;
    nes.dmc.sample_buf_full = 0;
    nes.dmc.dma_pending = 1;
    nes.dmc.dma_is_load = 0;
    printf("before pend=%d en=%d bl=%d full=%d delay=%d abort=%d reentry=%d\n",
           nes.dmc.dma_pending, nes.dmc.enabled, (int)nes.dmc.bytes_left,
           nes.dmc.sample_buf_full, nes.dmc.dma_halt_delay, nes.dmc.dma_abort,
           nes.dmc.dma_reentry);
    u8 a = cpu_read(&nes, 0x2007);
    printf("A=%02X v=%04X pend=%d full=%d\n", a, nes.vram_addr, nes.dmc.dma_pending,
           nes.dmc.sample_buf_full);
    return 0;
}
