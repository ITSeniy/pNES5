#ifndef NES_MAPPER_H
#define NES_MAPPER_H

#include "nes.h"

/* iNES mapper support check (shared by payload + host tests). */
int  mapper_supported(int mapper);

/* Power-on / after ROM load defaults for the active mapper. */
void mapper_init(struct NES *nes);

/* Cart space helpers used by the CPU/PPU buses. */
u8   mapper_prg_read(struct NES *nes, u16 addr);
u8   mapper_cpu_read(struct NES *nes, u16 addr);   /* $6000-$7FFF (+ PRG) */
void mapper_cpu_write(struct NES *nes, u16 addr, u8 val);

u8   mapper_chr_read(struct NES *nes, u16 addr);
void mapper_chr_write(struct NES *nes, u16 addr, u8 val);

/* Timing hooks */
void mapper_notify_a12(struct NES *nes, u16 ppu_addr);
void mapper_scanline_clock(struct NES *nes);
void mapper_cpu_clock(struct NES *nes, int cycles);

#endif
