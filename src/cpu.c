#include "nes.h"

u16 cpu_read16(struct NES *nes, u16 addr) {
    return cpu_read(nes, addr) | ((u16)cpu_read(nes, addr + 1) << 8);
}

static u16 read16_wrap(struct NES *nes, u16 addr) {
    u16 lo = cpu_read(nes, addr);
    u16 hi = cpu_read(nes, (addr & 0xFF00) | ((addr + 1) & 0xFF));
    return lo | (hi << 8);
}

static void push8(struct NES *nes, u8 v)  { cpu_write(nes, 0x100 | nes->sp--, v); }
static void push16(struct NES *nes, u16 v){ push8(nes, v >> 8); push8(nes, v & 0xFF); }
static u8   pull8(struct NES *nes)         { return cpu_read(nes, 0x100 | ++nes->sp); }
static u16  pull16(struct NES *nes)        { u16 lo = pull8(nes); return lo | ((u16)pull8(nes) << 8); }
static int  page_cross(u16 a, u16 b)      { return (a & 0xFF00) != (b & 0xFF00); }

#define IMM() (nes->pc++)
#define ZP()  (cpu_read(nes, nes->pc++))
#define ZPX() ((cpu_read(nes, nes->pc++) + nes->x) & 0xFF)
#define ZPY() ((cpu_read(nes, nes->pc++) + nes->y) & 0xFF)
#define ABS() (t16 = cpu_read16(nes, nes->pc), nes->pc += 2, t16)
#define ABX() (t16 = cpu_read16(nes, nes->pc), nes->pc += 2, xp = page_cross(t16, t16 + nes->x), (u16)(t16 + nes->x))
#define ABY() (t16 = cpu_read16(nes, nes->pc), nes->pc += 2, xp = page_cross(t16, t16 + nes->y), (u16)(t16 + nes->y))
#define IZX() (t8 = cpu_read(nes, nes->pc++) + nes->x, read16_wrap(nes, t8 & 0xFF))
#define IZY() (t16 = read16_wrap(nes, cpu_read(nes, nes->pc++)), xp = page_cross(t16, t16 + nes->y), (u16)(t16 + nes->y))
#define ABX_R() (t16 = cpu_read16(nes, nes->pc), nes->pc += 2, addr = (u16)(t16 + nes->x), xp = page_cross(t16, addr), xp ? (void)cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF)) : (void)0, addr)
#define ABY_R() (t16 = cpu_read16(nes, nes->pc), nes->pc += 2, addr = (u16)(t16 + nes->y), xp = page_cross(t16, addr), xp ? (void)cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF)) : (void)0, addr)
#define IZY_R() (t16 = read16_wrap(nes, cpu_read(nes, nes->pc++)), addr = (u16)(t16 + nes->y), xp = page_cross(t16, addr), xp ? (void)cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF)) : (void)0, addr)
#define ABX_W() (t16 = cpu_read16(nes, nes->pc), nes->pc += 2, addr = (u16)(t16 + nes->x), cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF)), addr)
#define ABY_W() (t16 = cpu_read16(nes, nes->pc), nes->pc += 2, addr = (u16)(t16 + nes->y), cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF)), addr)
#define IZY_W() (t16 = read16_wrap(nes, cpu_read(nes, nes->pc++)), addr = (u16)(t16 + nes->y), cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF)), addr)
/*
 * Branch bus timing + interrupt poll (AccuracyCoin IFlagLatency A/B, NMOS 6502):
 *  taken, no page cross (3 cyc): no poll on last cycle → IRQ delayed one instruction
 *  taken, page cross (4 cyc): poll before cycle 4 → IRQ may run immediately after
 */
#define BR(cond) do { \
    s8 off = (s8)cpu_read(nes, nes->pc++); \
    if (cond) { \
        u16 old_pc = nes->pc; \
        xp = page_cross(old_pc, (u16)(old_pc + off)); \
        (void)cpu_read(nes, old_pc); \
        nes->pc = (u16)(old_pc + off); \
        if (xp) { \
            (void)cpu_read(nes, nes->pc); \
            nes->prev_irq_inhibit = nes->flags & F_I; \
        } else { \
            /* Last-cycle poll skipped: delay IRQ/NMI by one instruction. */ \
            nes->prev_irq_inhibit = F_I; \
            if (nes->nmi_pending && !nes->nmi_delay) \
                nes->nmi_delay = 1; \
        } \
        nes->cycles += 1 + xp; \
    } \
} while(0)

/*
 * SH* "H+1" byte for value / address corruption.
 * If DMC DMA recently pulled RDY, high-byte contribution is open bus ($FF).
 */
static u8 sh_h1(struct NES *nes, u16 base) {
    u8 h1 = (u8)(((base >> 8) + 1) & 0xFF);
    s32 now = nes->total_cycles + nes->cycles;
    if (nes->dmc.sh_h_suppress
        || (nes->dmc.sh_suppress_until && now < nes->dmc.sh_suppress_until)) {
        h1 = 0xFF;
        nes->dmc.sh_h_suppress = 0;
        /* keep sh_suppress_until so multiple SH* in the window still match */
    }
    return h1;
}

/* Shared ALU / RMW helpers — one body each instead of macro expansion. */
static void do_adc(struct NES *nes, u8 v) {
    u16 sum = nes->a + v + (nes->flags & F_C);
    nes->flags &= ~(F_C | F_V);
    if (sum > 0xFF) nes->flags |= F_C;
    if (~(nes->a ^ v) & (nes->a ^ sum) & 0x80) nes->flags |= F_V;
    nes->a = (u8)sum;
    SET_ZN(nes->a);
}

static void do_sbc(struct NES *nes, u8 v) {
    u16 diff = nes->a - v - (1 - (nes->flags & F_C));
    nes->flags &= ~(F_C | F_V);
    if (diff < 0x100) nes->flags |= F_C;
    if ((nes->a ^ v) & (nes->a ^ diff) & 0x80) nes->flags |= F_V;
    nes->a = (u8)diff;
    SET_ZN(nes->a);
}

static void do_cmp(struct NES *nes, u8 r, u8 v) {
    nes->flags = (nes->flags & ~F_C) | (r >= v ? F_C : 0);
    SET_ZN((u8)(r - v));
}

static u8 rmw_asl(struct NES *nes, u16 a) {
    u8 val = cpu_read(nes, a);
    nes->flags = (nes->flags & ~F_C) | (val >> 7);
    cpu_write(nes, a, val);
    val <<= 1;
    cpu_write(nes, a, val);
    SET_ZN(val);
    return val;
}

static u8 rmw_lsr(struct NES *nes, u16 a) {
    u8 val = cpu_read(nes, a);
    nes->flags = (nes->flags & ~F_C) | (val & 1);
    cpu_write(nes, a, val);
    val >>= 1;
    cpu_write(nes, a, val);
    SET_ZN(val);
    return val;
}

static u8 rmw_rol(struct NES *nes, u16 a) {
    u8 val = cpu_read(nes, a);
    u8 old = val, t8 = val >> 7;
    val = (u8)((val << 1) | (nes->flags & F_C));
    nes->flags = (nes->flags & ~F_C) | t8;
    cpu_write(nes, a, old);
    cpu_write(nes, a, val);
    SET_ZN(val);
    return val;
}

static u8 rmw_ror(struct NES *nes, u16 a) {
    u8 val = cpu_read(nes, a);
    u8 old = val, t8 = val & 1;
    val = (u8)((val >> 1) | ((nes->flags & F_C) << 7));
    nes->flags = (nes->flags & ~F_C) | t8;
    cpu_write(nes, a, old);
    cpu_write(nes, a, val);
    SET_ZN(val);
    return val;
}

static u8 rmw_dec(struct NES *nes, u16 ea) {
    u8 val = cpu_read(nes, ea);
    cpu_write(nes, ea, val);
    val--;
    cpu_write(nes, ea, val);
    SET_ZN(val);
    return val;
}

static u8 rmw_inc(struct NES *nes, u16 ea) {
    u8 val = cpu_read(nes, ea);
    cpu_write(nes, ea, val);
    val++;
    cpu_write(nes, ea, val);
    SET_ZN(val);
    return val;
}

static void rmw_dcp(struct NES *nes, u16 ea) {
    do_cmp(nes, nes->a, rmw_dec(nes, ea));
}

static void rmw_isb(struct NES *nes, u16 ea) {
    do_sbc(nes, rmw_inc(nes, ea));
}

/* SH* store with page-cross address corruption */
static void sh_store(struct NES *nes, u16 base, u16 addr, u8 val) {
    if (page_cross(base, addr))
        addr = (u16)((val << 8) | (addr & 0xFF));
    cpu_write(nes, addr, val);
}

void cpu_step(struct NES *nes) {
    u8 nmi_blocked = 0;
    u8 nmi_overlaps = 0;
    u8 nmi_overlap_cycle = 0;
    if (nes->nmi_delay) {
        nes->nmi_delay--;
        nmi_blocked = 1;
    }
    if (nes->nmi_in_instr) {
        nmi_overlaps = nes->nmi_pending;
        nmi_overlap_cycle = nes->nmi_instr_cycle;
        nes->nmi_in_instr = 0;
        nes->nmi_instr_cycle = 0;
        nmi_blocked = 1;
    }

    if (!nmi_blocked && nes->nmi_pending && !nes->prev_nmi_line) {
        nes->nmi_pending = 0;
        nes->prev_nmi_line = 1;
        push16(nes, nes->pc);
        push8(nes, (nes->flags | F_U) & ~F_B);
        nes->flags |= F_I;
        nes->pc = cpu_read16(nes, 0xFFFA);
        nes->cycles += 7;
        nes->prev_irq_inhibit = F_I;
        return;
    }
    if (!nes->nmi_pending) nes->prev_nmi_line = 0;

    if ((nes->irq_pending || nes->apu_irq_pending) && !nes->prev_irq_inhibit) {
        /*
         * Same hijack window as BRK: NMI during early IRQ vectoring steals the
         * vector; late NMI is delayed until after the IRQ sequence (AccuracyCoin
         * NmiAndIrq / NmiAndBrk).
         */
        u16 vector = 0xFFFE;
        push16(nes, nes->pc);
        push8(nes, (nes->flags | F_U) & ~F_B);
        nes->flags |= F_I;
        if (nmi_overlaps && nmi_overlap_cycle <= 5) {
            nes->nmi_pending = 0;
            nes->prev_nmi_line = 1;
            vector = 0xFFFA;
        } else {
            if (nmi_overlaps)
                nes->nmi_delay = 1;
            nes->irq_pending = 0;
        }
        nes->pc = cpu_read16(nes, vector);
        nes->cycles += 7;
        nes->prev_irq_inhibit = F_I;
        return;
    }

    nes->prev_irq_inhibit = nes->flags & F_I;

    u8 op = cpu_read(nes, nes->pc++);
    u16 addr = 0, t16;
    u8 val, t8;
    int xp = 0;

    switch (op) {
    case 0x00: nes->pc++; push16(nes,nes->pc); push8(nes,nes->flags|F_B|F_U); nes->flags|=F_I; if(nmi_overlaps&&nmi_overlap_cycle<=5){nes->nmi_pending=0; nes->prev_nmi_line=1; nes->pc=cpu_read16(nes,0xFFFA);}else{if(nmi_overlaps)nes->nmi_delay=1; nes->pc=cpu_read16(nes,0xFFFE);} nes->cycles+=7; return;
    case 0x01: nes->a|=cpu_read(nes,IZX()); SET_ZN(nes->a); nes->cycles+=6; return;
    case 0x02: nes->pc--; nes->cycles+=2; return;
    case 0x03: addr=IZX(); val=rmw_asl(nes,addr); nes->a|=val; SET_ZN(nes->a); nes->cycles+=8; return;
    case 0x04: cpu_read(nes,ZP()); nes->cycles+=3; return;
    case 0x05: nes->a|=cpu_read(nes,ZP()); SET_ZN(nes->a); nes->cycles+=3; return;
    case 0x06: addr=ZP(); val=rmw_asl(nes,addr); nes->cycles+=5; return;
    case 0x07: addr=ZP(); val=rmw_asl(nes,addr); nes->a|=val; SET_ZN(nes->a); nes->cycles+=5; return;
    case 0x08: (void)cpu_read(nes, nes->pc); push8(nes,nes->flags|F_B|F_U); nes->cycles+=3; return;
    case 0x09: nes->a|=cpu_read(nes,IMM()); SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x0A: nes->flags=(nes->flags&~F_C)|(nes->a>>7); nes->a<<=1; SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x0B: nes->a&=cpu_read(nes,IMM()); SET_ZN(nes->a); nes->flags=(nes->flags&~F_C)|((nes->a>>7)&1); nes->cycles+=2; return;
    case 0x0C: addr=ABS(); cpu_read(nes,addr); nes->cycles+=4; return;
    case 0x0D: nes->a|=cpu_read(nes,ABS()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0x0E: addr=ABS(); val=rmw_asl(nes,addr); nes->cycles+=6; return;
    case 0x0F: addr=ABS(); val=rmw_asl(nes,addr); nes->a|=val; SET_ZN(nes->a); nes->cycles+=6; return;

    case 0x10: BR(!(nes->flags&F_N)); nes->cycles+=2; return;
    case 0x11: nes->a|=cpu_read(nes,IZY_R()); SET_ZN(nes->a); nes->cycles+=5+xp; return;
    case 0x12: nes->pc--; nes->cycles+=2; return;
    case 0x13: addr=IZY_W(); val=rmw_asl(nes,addr); nes->a|=val; SET_ZN(nes->a); nes->cycles+=8; return;
    case 0x14: cpu_read(nes,ZPX()); nes->cycles+=4; return;
    case 0x15: nes->a|=cpu_read(nes,ZPX()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0x16: addr=ZPX(); val=rmw_asl(nes,addr); nes->cycles+=6; return;
    case 0x17: addr=ZPX(); val=rmw_asl(nes,addr); nes->a|=val; SET_ZN(nes->a); nes->cycles+=6; return;
    case 0x18: (void)cpu_read(nes, nes->pc); nes->flags&=~F_C; nes->cycles+=2; return;
    case 0x19: nes->a|=cpu_read(nes,ABY_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0x1A: (void)cpu_read(nes, nes->pc); nes->cycles+=2; return;
    case 0x1B: addr=ABY_W(); val=rmw_asl(nes,addr); nes->a|=val; SET_ZN(nes->a); nes->cycles+=7; return;
    case 0x1C: addr=ABX_R(); cpu_read(nes,addr); nes->cycles+=4+xp; return;
    case 0x1D: nes->a|=cpu_read(nes,ABX_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0x1E: addr=ABX_W(); val=rmw_asl(nes,addr); nes->cycles+=7; return;
    case 0x1F: addr=ABX_W(); val=rmw_asl(nes,addr); nes->a|=val; SET_ZN(nes->a); nes->cycles+=7; return;

    case 0x20: {
        /* Real 6502 JSR order (critical for open-bus / AccuracyCoin):
         *  1 opcode already fetched
         *  2 fetch ADL, PC++
         *  3 dummy read stack
         *  4 push PCH
         *  5 push PCL   (return addr = address of ADH byte)
         *  6 fetch ADH  ← last bus cycle; leaves high byte on data bus
         */
        u8 lo = cpu_read(nes, nes->pc++);
        (void)cpu_read(nes, (u16)(0x100 | nes->sp));
        push16(nes, nes->pc); /* points at high operand */
        u8 hi = cpu_read(nes, nes->pc);
        nes->pc = (u16)(((u16)hi << 8) | lo);
        nes->cycles += 6;
        return;
    }
    case 0x21: nes->a&=cpu_read(nes,IZX()); SET_ZN(nes->a); nes->cycles+=6; return;
    case 0x22: nes->pc--; nes->cycles+=2; return;
    case 0x23: addr=IZX(); val=rmw_rol(nes,addr); nes->a&=val; SET_ZN(nes->a); nes->cycles+=8; return;
    case 0x24: val=cpu_read(nes,ZP()); nes->flags=(nes->flags&~(F_Z|F_V|F_N))|((nes->a&val)==0?F_Z:0)|(val&0xC0); nes->cycles+=3; return;
    case 0x25: nes->a&=cpu_read(nes,ZP()); SET_ZN(nes->a); nes->cycles+=3; return;
    case 0x26: addr=ZP(); val=rmw_rol(nes,addr); nes->cycles+=5; return;
    case 0x27: addr=ZP(); val=rmw_rol(nes,addr); nes->a&=val; SET_ZN(nes->a); nes->cycles+=5; return;
    case 0x28: {
        /* PLP: poll uses I before pull; I change takes effect after (CLI-like delay). */
        (void)cpu_read(nes, nes->pc);
        (void)cpu_read(nes, (u16)(0x100 | nes->sp));
        {
            u8 i_before = nes->flags & F_I;
            nes->flags = (pull8(nes) & ~(F_B | F_U)) | F_U;
            if (i_before && !(nes->flags & F_I))
                nes->prev_irq_inhibit = F_I;
            else if (!i_before && (nes->flags & F_I)
                     && (nes->irq_pending || nes->apu_irq_pending))
                nes->prev_irq_inhibit = 0;
            else
                nes->prev_irq_inhibit = nes->flags & F_I;
        }
        nes->cycles += 4;
        return;
    }
    case 0x29: nes->a&=cpu_read(nes,IMM()); SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x2A: t8=nes->a>>7; nes->a=(nes->a<<1)|(nes->flags&F_C); nes->flags=(nes->flags&~F_C)|t8; SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x2B: nes->a&=cpu_read(nes,IMM()); SET_ZN(nes->a); nes->flags=(nes->flags&~F_C)|((nes->a>>7)&1); nes->cycles+=2; return;
    case 0x2C: val=cpu_read(nes,ABS()); nes->flags=(nes->flags&~(F_Z|F_V|F_N))|((nes->a&val)==0?F_Z:0)|(val&0xC0); nes->cycles+=4; return;
    case 0x2D: nes->a&=cpu_read(nes,ABS()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0x2E: addr=ABS(); val=rmw_rol(nes,addr); nes->cycles+=6; return;
    case 0x2F: addr=ABS(); val=rmw_rol(nes,addr); nes->a&=val; SET_ZN(nes->a); nes->cycles+=6; return;

    case 0x30: BR(nes->flags&F_N); nes->cycles+=2; return;
    case 0x31: nes->a&=cpu_read(nes,IZY_R()); SET_ZN(nes->a); nes->cycles+=5+xp; return;
    case 0x32: nes->pc--; nes->cycles+=2; return;
    case 0x33: addr=IZY_W(); val=rmw_rol(nes,addr); nes->a&=val; SET_ZN(nes->a); nes->cycles+=8; return;
    case 0x34: cpu_read(nes,ZPX()); nes->cycles+=4; return;
    case 0x35: nes->a&=cpu_read(nes,ZPX()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0x36: addr=ZPX(); val=rmw_rol(nes,addr); nes->cycles+=6; return;
    case 0x37: addr=ZPX(); val=rmw_rol(nes,addr); nes->a&=val; SET_ZN(nes->a); nes->cycles+=6; return;
    case 0x38: (void)cpu_read(nes, nes->pc); nes->flags|=F_C; nes->cycles+=2; return;
    case 0x39: nes->a&=cpu_read(nes,ABY_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0x3A: (void)cpu_read(nes, nes->pc); nes->cycles+=2; return;
    case 0x3B: addr=ABY_W(); val=rmw_rol(nes,addr); nes->a&=val; SET_ZN(nes->a); nes->cycles+=7; return;
    case 0x3C: addr=ABX_R(); cpu_read(nes,addr); nes->cycles+=4+xp; return;
    case 0x3D: nes->a&=cpu_read(nes,ABX_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0x3E: addr=ABX_W(); val=rmw_rol(nes,addr); nes->cycles+=7; return;
    case 0x3F: addr=ABX_W(); val=rmw_rol(nes,addr); nes->a&=val; SET_ZN(nes->a); nes->cycles+=7; return;

    case 0x40: (void)cpu_read(nes, nes->pc); (void)cpu_read(nes, (u16)(0x100 | nes->sp)); nes->flags=(pull8(nes)&~(F_B|F_U))|F_U; nes->pc=pull16(nes); nes->cycles+=6; nes->prev_irq_inhibit=nes->flags&F_I; return;
    case 0x41: nes->a^=cpu_read(nes,IZX()); SET_ZN(nes->a); nes->cycles+=6; return;
    case 0x42: nes->pc--; nes->cycles+=2; return;
    case 0x43: addr=IZX(); val=rmw_lsr(nes,addr); nes->a^=val; SET_ZN(nes->a); nes->cycles+=8; return;
    case 0x44: cpu_read(nes,ZP()); nes->cycles+=3; return;
    case 0x45: nes->a^=cpu_read(nes,ZP()); SET_ZN(nes->a); nes->cycles+=3; return;
    case 0x46: addr=ZP(); val=rmw_lsr(nes,addr); nes->cycles+=5; return;
    case 0x47: addr=ZP(); val=rmw_lsr(nes,addr); nes->a^=val; SET_ZN(nes->a); nes->cycles+=5; return;
    case 0x48: (void)cpu_read(nes, nes->pc); push8(nes,nes->a); nes->cycles+=3; return;
    case 0x49: nes->a^=cpu_read(nes,IMM()); SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x4A: nes->flags=(nes->flags&~F_C)|(nes->a&1); nes->a>>=1; SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x4B: nes->a&=cpu_read(nes,IMM()); nes->flags=(nes->flags&~F_C)|(nes->a&1); nes->a>>=1; SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x4C: nes->pc=ABS(); nes->cycles+=3; return;
    case 0x4D: nes->a^=cpu_read(nes,ABS()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0x4E: addr=ABS(); val=rmw_lsr(nes,addr); nes->cycles+=6; return;
    case 0x4F: addr=ABS(); val=rmw_lsr(nes,addr); nes->a^=val; SET_ZN(nes->a); nes->cycles+=6; return;

    case 0x50: BR(!(nes->flags&F_V)); nes->cycles+=2; return;
    case 0x51: nes->a^=cpu_read(nes,IZY_R()); SET_ZN(nes->a); nes->cycles+=5+xp; return;
    case 0x52: nes->pc--; nes->cycles+=2; return;
    case 0x53: addr=IZY_W(); val=rmw_lsr(nes,addr); nes->a^=val; SET_ZN(nes->a); nes->cycles+=8; return;
    case 0x54: cpu_read(nes,ZPX()); nes->cycles+=4; return;
    case 0x55: nes->a^=cpu_read(nes,ZPX()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0x56: addr=ZPX(); val=rmw_lsr(nes,addr); nes->cycles+=6; return;
    case 0x57: addr=ZPX(); val=rmw_lsr(nes,addr); nes->a^=val; SET_ZN(nes->a); nes->cycles+=6; return;
    case 0x58: {
        /*
         * CLI: interrupt poll is before I is cleared, so an already-pending IRQ
         * is not taken until after the following instruction.
         */
        (void)cpu_read(nes, nes->pc);
        nes->flags &= ~F_I;
        nes->prev_irq_inhibit = F_I;
        nes->cycles += 2;
        return;
    }
    case 0x59: nes->a^=cpu_read(nes,ABY_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0x5A: (void)cpu_read(nes, nes->pc); nes->cycles+=2; return;
    case 0x5B: addr=ABY_W(); val=rmw_lsr(nes,addr); nes->a^=val; SET_ZN(nes->a); nes->cycles+=7; return;
    case 0x5C: addr=ABX_R(); cpu_read(nes,addr); nes->cycles+=4+xp; return;
    case 0x5D: nes->a^=cpu_read(nes,ABX_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0x5E: addr=ABX_W(); val=rmw_lsr(nes,addr); nes->cycles+=7; return;
    case 0x5F: addr=ABX_W(); val=rmw_lsr(nes,addr); nes->a^=val; SET_ZN(nes->a); nes->cycles+=7; return;

    case 0x60: (void)cpu_read(nes, nes->pc); (void)cpu_read(nes, (u16)(0x100 | nes->sp)); nes->pc=pull16(nes)+1; nes->cycles+=6; return;
    case 0x61: val=cpu_read(nes,IZX()); do_adc(nes,val); nes->cycles+=6; return;
    case 0x62: nes->pc--; nes->cycles+=2; return;
    case 0x63: addr=IZX(); val=rmw_ror(nes,addr); do_adc(nes,val); nes->cycles+=8; return;
    case 0x64: cpu_read(nes,ZP()); nes->cycles+=3; return;
    case 0x65: val=cpu_read(nes,ZP()); do_adc(nes,val); nes->cycles+=3; return;
    case 0x66: addr=ZP(); val=rmw_ror(nes,addr); nes->cycles+=5; return;
    case 0x67: addr=ZP(); val=rmw_ror(nes,addr); do_adc(nes,val); nes->cycles+=5; return;
    case 0x68: (void)cpu_read(nes, nes->pc); (void)cpu_read(nes, (u16)(0x100 | nes->sp)); nes->a=pull8(nes); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0x69: val=cpu_read(nes,IMM()); do_adc(nes,val); nes->cycles+=2; return;
    case 0x6A: t8=nes->a&1; nes->a=(nes->a>>1)|((nes->flags&F_C)<<7); nes->flags=(nes->flags&~F_C)|t8; SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x6B:
        nes->a&=cpu_read(nes,IMM());
        nes->a=(nes->a>>1)|((nes->flags&F_C)<<7);
        SET_ZN(nes->a); nes->flags&=~(F_C|F_V);
        if (nes->a&0x40) nes->flags|=F_C;
        if (((nes->a>>6)^(nes->a>>5))&1) nes->flags|=F_V;
        nes->cycles+=2; return;
    case 0x6C: addr=ABS(); nes->pc=read16_wrap(nes,addr); nes->cycles+=5; return;
    case 0x6D: val=cpu_read(nes,ABS()); do_adc(nes,val); nes->cycles+=4; return;
    case 0x6E: addr=ABS(); val=rmw_ror(nes,addr); nes->cycles+=6; return;
    case 0x6F: addr=ABS(); val=rmw_ror(nes,addr); do_adc(nes,val); nes->cycles+=6; return;

    case 0x70: BR(nes->flags&F_V); nes->cycles+=2; return;
    case 0x71: val=cpu_read(nes,IZY_R()); do_adc(nes,val); nes->cycles+=5+xp; return;
    case 0x72: nes->pc--; nes->cycles+=2; return;
    case 0x73: addr=IZY_W(); val=rmw_ror(nes,addr); do_adc(nes,val); nes->cycles+=8; return;
    case 0x74: cpu_read(nes,ZPX()); nes->cycles+=4; return;
    case 0x75: val=cpu_read(nes,ZPX()); do_adc(nes,val); nes->cycles+=4; return;
    case 0x76: addr=ZPX(); val=rmw_ror(nes,addr); nes->cycles+=6; return;
    case 0x77: addr=ZPX(); val=rmw_ror(nes,addr); do_adc(nes,val); nes->cycles+=6; return;
    case 0x78: {
        /*
         * SEI: poll is before I is set. If IRQ is low while I was clear, the
         * interrupt is taken after SEI even though I ends set (AccuracyCoin
         * IFlagLatency test 3/4 — pushed flags include I).
         */
        (void)cpu_read(nes, nes->pc);
        {
            u8 i_before = nes->flags & F_I;
            u8 irq_line = (nes->irq_pending || nes->apu_irq_pending) ? 1 : 0;
            nes->flags |= F_I;
            if (irq_line && !i_before)
                nes->prev_irq_inhibit = 0;
            else
                nes->prev_irq_inhibit = F_I;
        }
        nes->cycles += 2;
        return;
    }
    case 0x79: val=cpu_read(nes,ABY_R()); do_adc(nes,val); nes->cycles+=4+xp; return;
    case 0x7A: (void)cpu_read(nes, nes->pc); nes->cycles+=2; return;
    case 0x7B: addr=ABY_W(); val=rmw_ror(nes,addr); do_adc(nes,val); nes->cycles+=7; return;
    case 0x7C: addr=ABX_R(); cpu_read(nes,addr); nes->cycles+=4+xp; return;
    case 0x7D: val=cpu_read(nes,ABX_R()); do_adc(nes,val); nes->cycles+=4+xp; return;
    case 0x7E: addr=ABX_W(); val=rmw_ror(nes,addr); nes->cycles+=7; return;
    case 0x7F: addr=ABX_W(); val=rmw_ror(nes,addr); do_adc(nes,val); nes->cycles+=7; return;

    case 0x80: IMM(); nes->cycles+=2; return;
    case 0x81: cpu_write(nes,IZX(),nes->a); nes->cycles+=6; return;
    case 0x82: IMM(); nes->cycles+=2; return;
    case 0x83: cpu_write(nes,IZX(),nes->a&nes->x); nes->cycles+=6; return;
    case 0x84: cpu_write(nes,ZP(),nes->y); nes->cycles+=3; return;
    case 0x85: cpu_write(nes,ZP(),nes->a); nes->cycles+=3; return;
    case 0x86: cpu_write(nes,ZP(),nes->x); nes->cycles+=3; return;
    case 0x87: cpu_write(nes,ZP(),nes->a&nes->x); nes->cycles+=3; return;
    case 0x88: (void)cpu_read(nes, nes->pc); nes->y--; SET_ZN(nes->y); nes->cycles+=2; return;
    case 0x89: IMM(); nes->cycles+=2; return;
    case 0x8A: (void)cpu_read(nes, nes->pc); nes->a=nes->x; SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x8B: nes->a=(nes->a&nes->x)&cpu_read(nes,IMM()); SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x8C: cpu_write(nes,ABS(),nes->y); nes->cycles+=4; return;
    case 0x8D: cpu_write(nes,ABS(),nes->a); nes->cycles+=4; return;
    case 0x8E: cpu_write(nes,ABS(),nes->x); nes->cycles+=4; return;
    case 0x8F: cpu_write(nes,ABS(),nes->a&nes->x); nes->cycles+=4; return;

    case 0x90: BR(!(nes->flags&F_C)); nes->cycles+=2; return;
    case 0x91: cpu_write(nes,IZY_W(),nes->a); nes->cycles+=6; return;
    case 0x92: nes->pc--; nes->cycles+=2; return;
    case 0x93: {
        /* SHA (Indirect),Y */
        t16 = read16_wrap(nes, cpu_read(nes, nes->pc++));
        addr = (u16)(t16 + nes->y);
        cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF));
        sh_store(nes, t16, addr, (u8)(nes->a & nes->x & sh_h1(nes, t16)));
        nes->cycles += 6;
        return;
    }
    case 0x94: cpu_write(nes,ZPX(),nes->y); nes->cycles+=4; return;
    case 0x95: cpu_write(nes,ZPX(),nes->a); nes->cycles+=4; return;
    case 0x96: cpu_write(nes,ZPY(),nes->x); nes->cycles+=4; return;
    case 0x97: cpu_write(nes,ZPY(),nes->a&nes->x); nes->cycles+=4; return;
    case 0x98: (void)cpu_read(nes, nes->pc); nes->a=nes->y; SET_ZN(nes->a); nes->cycles+=2; return;
    case 0x99: cpu_write(nes,ABY_W(),nes->a); nes->cycles+=5; return;
    case 0x9A: (void)cpu_read(nes, nes->pc); nes->sp=nes->x; nes->cycles+=2; return;
    case 0x9B: {
        /* SHS Absolute,Y: SP=A&X; write SP&(H+1) */
        t16 = cpu_read16(nes, nes->pc); nes->pc += 2;
        addr = (u16)(t16 + nes->y);
        cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF));
        nes->sp = nes->a & nes->x;
        sh_store(nes, t16, addr, (u8)(nes->sp & sh_h1(nes, t16)));
        nes->cycles += 5;
        return;
    }
    case 0x9C: {
        /* SHY Absolute,X */
        t16 = cpu_read16(nes, nes->pc); nes->pc += 2;
        addr = (u16)(t16 + nes->x);
        cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF));
        sh_store(nes, t16, addr, (u8)(nes->y & sh_h1(nes, t16)));
        nes->cycles += 5;
        return;
    }
    case 0x9D: cpu_write(nes,ABX_W(),nes->a); nes->cycles+=5; return;
    case 0x9E: {
        /* SHX Absolute,Y */
        t16 = cpu_read16(nes, nes->pc); nes->pc += 2;
        addr = (u16)(t16 + nes->y);
        cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF));
        sh_store(nes, t16, addr, (u8)(nes->x & sh_h1(nes, t16)));
        nes->cycles += 5;
        return;
    }
    case 0x9F: {
        /* SHA Absolute,Y */
        t16 = cpu_read16(nes, nes->pc); nes->pc += 2;
        addr = (u16)(t16 + nes->y);
        cpu_read(nes, (t16 & 0xFF00) | (addr & 0xFF));
        sh_store(nes, t16, addr, (u8)(nes->a & nes->x & sh_h1(nes, t16)));
        nes->cycles += 5;
        return;
    }

    case 0xA0: nes->y=cpu_read(nes,IMM()); SET_ZN(nes->y); nes->cycles+=2; return;
    case 0xA1: nes->a=cpu_read(nes,IZX()); SET_ZN(nes->a); nes->cycles+=6; return;
    case 0xA2: nes->x=cpu_read(nes,IMM()); SET_ZN(nes->x); nes->cycles+=2; return;
    case 0xA3: nes->a=nes->x=cpu_read(nes,IZX()); SET_ZN(nes->a); nes->cycles+=6; return;
    case 0xA4: nes->y=cpu_read(nes,ZP()); SET_ZN(nes->y); nes->cycles+=3; return;
    case 0xA5: nes->a=cpu_read(nes,ZP()); SET_ZN(nes->a); nes->cycles+=3; return;
    case 0xA6: nes->x=cpu_read(nes,ZP()); SET_ZN(nes->x); nes->cycles+=3; return;
    case 0xA7: nes->a=nes->x=cpu_read(nes,ZP()); SET_ZN(nes->a); nes->cycles+=3; return;
    case 0xA8: (void)cpu_read(nes, nes->pc); nes->y=nes->a; SET_ZN(nes->y); nes->cycles+=2; return;
    case 0xA9: nes->a=cpu_read(nes,IMM()); SET_ZN(nes->a); nes->cycles+=2; return;
    case 0xAA: (void)cpu_read(nes, nes->pc); nes->x=nes->a; SET_ZN(nes->x); nes->cycles+=2; return;
    case 0xAB: nes->a=nes->x=cpu_read(nes,IMM()); SET_ZN(nes->a); nes->cycles+=2; return;
    case 0xAC: nes->y=cpu_read(nes,ABS()); SET_ZN(nes->y); nes->cycles+=4; return;
    case 0xAD: nes->a=cpu_read(nes,ABS()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0xAE: nes->x=cpu_read(nes,ABS()); SET_ZN(nes->x); nes->cycles+=4; return;
    case 0xAF: nes->a=nes->x=cpu_read(nes,ABS()); SET_ZN(nes->a); nes->cycles+=4; return;

    case 0xB0: BR(nes->flags&F_C); nes->cycles+=2; return;
    case 0xB1: nes->a=cpu_read(nes,IZY_R()); SET_ZN(nes->a); nes->cycles+=5+xp; return;
    case 0xB2: nes->pc--; nes->cycles+=2; return;
    case 0xB3: nes->a=nes->x=cpu_read(nes,IZY_R()); SET_ZN(nes->a); nes->cycles+=5+xp; return;
    case 0xB4: nes->y=cpu_read(nes,ZPX()); SET_ZN(nes->y); nes->cycles+=4; return;
    case 0xB5: nes->a=cpu_read(nes,ZPX()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0xB6: nes->x=cpu_read(nes,ZPY()); SET_ZN(nes->x); nes->cycles+=4; return;
    case 0xB7: nes->a=nes->x=cpu_read(nes,ZPY()); SET_ZN(nes->a); nes->cycles+=4; return;
    case 0xB8: (void)cpu_read(nes, nes->pc); nes->flags&=~F_V; nes->cycles+=2; return;
    case 0xB9: nes->a=cpu_read(nes,ABY_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0xBA: (void)cpu_read(nes, nes->pc); nes->x=nes->sp; SET_ZN(nes->x); nes->cycles+=2; return;
    case 0xBB: addr=ABY_R(); val=cpu_read(nes,addr)&nes->sp; nes->a=nes->x=nes->sp=val; SET_ZN(val); nes->cycles+=4+xp; return;
    case 0xBC: nes->y=cpu_read(nes,ABX_R()); SET_ZN(nes->y); nes->cycles+=4+xp; return;
    case 0xBD: nes->a=cpu_read(nes,ABX_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;
    case 0xBE: nes->x=cpu_read(nes,ABY_R()); SET_ZN(nes->x); nes->cycles+=4+xp; return;
    case 0xBF: nes->a=nes->x=cpu_read(nes,ABY_R()); SET_ZN(nes->a); nes->cycles+=4+xp; return;

    case 0xC0: val=cpu_read(nes,IMM()); do_cmp(nes,nes->y,val); nes->cycles+=2; return;
    case 0xC1: val=cpu_read(nes,IZX()); do_cmp(nes,nes->a,val); nes->cycles+=6; return;
    case 0xC2: IMM(); nes->cycles+=2; return;
    case 0xC3: addr=IZX(); rmw_dcp(nes,addr); nes->cycles+=8; return;
    case 0xC4: val=cpu_read(nes,ZP()); do_cmp(nes,nes->y,val); nes->cycles+=3; return;
    case 0xC5: val=cpu_read(nes,ZP()); do_cmp(nes,nes->a,val); nes->cycles+=3; return;
    case 0xC6: addr=ZP(); val=rmw_dec(nes,addr); nes->cycles+=5; return;
    case 0xC7: addr=ZP(); rmw_dcp(nes,addr); nes->cycles+=5; return;
    case 0xC8: (void)cpu_read(nes, nes->pc); nes->y++; SET_ZN(nes->y); nes->cycles+=2; return;
    case 0xC9: val=cpu_read(nes,IMM()); do_cmp(nes,nes->a,val); nes->cycles+=2; return;
    case 0xCA: (void)cpu_read(nes, nes->pc); nes->x--; SET_ZN(nes->x); nes->cycles+=2; return;
    case 0xCB: val=cpu_read(nes,IMM()); { u16 tmp=(u16)(nes->a&nes->x)-(u16)val; nes->flags=(nes->flags&~F_C)|(tmp<0x100?F_C:0); nes->x=(u8)tmp; SET_ZN(nes->x); } nes->cycles+=2; return;
    case 0xCC: val=cpu_read(nes,ABS()); do_cmp(nes,nes->y,val); nes->cycles+=4; return;
    case 0xCD: val=cpu_read(nes,ABS()); do_cmp(nes,nes->a,val); nes->cycles+=4; return;
    case 0xCE: addr=ABS(); val=rmw_dec(nes,addr); nes->cycles+=6; return;
    case 0xCF: addr=ABS(); rmw_dcp(nes,addr); nes->cycles+=6; return;

    case 0xD0: BR(!(nes->flags&F_Z)); nes->cycles+=2; return;
    case 0xD1: val=cpu_read(nes,IZY_R()); do_cmp(nes,nes->a,val); nes->cycles+=5+xp; return;
    case 0xD2: nes->pc--; nes->cycles+=2; return;
    case 0xD3: addr=IZY_W(); rmw_dcp(nes,addr); nes->cycles+=8; return;
    case 0xD4: cpu_read(nes,ZPX()); nes->cycles+=4; return;
    case 0xD5: val=cpu_read(nes,ZPX()); do_cmp(nes,nes->a,val); nes->cycles+=4; return;
    case 0xD6: addr=ZPX(); val=rmw_dec(nes,addr); nes->cycles+=6; return;
    case 0xD7: addr=ZPX(); rmw_dcp(nes,addr); nes->cycles+=6; return;
    case 0xD8: (void)cpu_read(nes, nes->pc); nes->flags&=~F_D; nes->cycles+=2; return;
    case 0xD9: val=cpu_read(nes,ABY_R()); do_cmp(nes,nes->a,val); nes->cycles+=4+xp; return;
    case 0xDA: (void)cpu_read(nes, nes->pc); nes->cycles+=2; return;
    case 0xDB: addr=ABY_W(); rmw_dcp(nes,addr); nes->cycles+=7; return;
    case 0xDC: addr=ABX_R(); cpu_read(nes,addr); nes->cycles+=4+xp; return;
    case 0xDD: val=cpu_read(nes,ABX_R()); do_cmp(nes,nes->a,val); nes->cycles+=4+xp; return;
    case 0xDE: addr=ABX_W(); val=rmw_dec(nes,addr); nes->cycles+=7; return;
    case 0xDF: addr=ABX_W(); rmw_dcp(nes,addr); nes->cycles+=7; return;

    case 0xE0: val=cpu_read(nes,IMM()); do_cmp(nes,nes->x,val); nes->cycles+=2; return;
    case 0xE1: val=cpu_read(nes,IZX()); do_sbc(nes,val); nes->cycles+=6; return;
    case 0xE2: IMM(); nes->cycles+=2; return;
    case 0xE3: addr=IZX(); rmw_isb(nes,addr); nes->cycles+=8; return;
    case 0xE4: val=cpu_read(nes,ZP()); do_cmp(nes,nes->x,val); nes->cycles+=3; return;
    case 0xE5: val=cpu_read(nes,ZP()); do_sbc(nes,val); nes->cycles+=3; return;
    case 0xE6: addr=ZP(); val=rmw_inc(nes,addr); nes->cycles+=5; return;
    case 0xE7: addr=ZP(); rmw_isb(nes,addr); nes->cycles+=5; return;
    case 0xE8: (void)cpu_read(nes, nes->pc); nes->x++; SET_ZN(nes->x); nes->cycles+=2; return;
    case 0xE9: val=cpu_read(nes,IMM()); do_sbc(nes,val); nes->cycles+=2; return;
    case 0xEA: (void)cpu_read(nes, nes->pc); nes->cycles+=2; return;
    case 0xEB: val=cpu_read(nes,IMM()); do_sbc(nes,val); nes->cycles+=2; return;
    case 0xEC: val=cpu_read(nes,ABS()); do_cmp(nes,nes->x,val); nes->cycles+=4; return;
    case 0xED: val=cpu_read(nes,ABS()); do_sbc(nes,val); nes->cycles+=4; return;
    case 0xEE: addr=ABS(); val=rmw_inc(nes,addr); nes->cycles+=6; return;
    case 0xEF: addr=ABS(); rmw_isb(nes,addr); nes->cycles+=6; return;

    case 0xF0: BR(nes->flags&F_Z); nes->cycles+=2; return;
    case 0xF1: val=cpu_read(nes,IZY_R()); do_sbc(nes,val); nes->cycles+=5+xp; return;
    case 0xF2: nes->pc--; nes->cycles+=2; return;
    case 0xF3: addr=IZY_W(); rmw_isb(nes,addr); nes->cycles+=8; return;
    case 0xF4: cpu_read(nes,ZPX()); nes->cycles+=4; return;
    case 0xF5: val=cpu_read(nes,ZPX()); do_sbc(nes,val); nes->cycles+=4; return;
    case 0xF6: addr=ZPX(); val=rmw_inc(nes,addr); nes->cycles+=6; return;
    case 0xF7: addr=ZPX(); rmw_isb(nes,addr); nes->cycles+=6; return;
    case 0xF8: (void)cpu_read(nes, nes->pc); nes->flags|=F_D; nes->cycles+=2; return;
    case 0xF9: val=cpu_read(nes,ABY_R()); do_sbc(nes,val); nes->cycles+=4+xp; return;
    case 0xFA: (void)cpu_read(nes, nes->pc); nes->cycles+=2; return;
    case 0xFB: addr=ABY_W(); rmw_isb(nes,addr); nes->cycles+=7; return;
    case 0xFC: addr=ABX_R(); cpu_read(nes,addr); nes->cycles+=4+xp; return;
    case 0xFD: val=cpu_read(nes,ABX_R()); do_sbc(nes,val); nes->cycles+=4+xp; return;
    case 0xFE: addr=ABX_W(); val=rmw_inc(nes,addr); nes->cycles+=7; return;
    case 0xFF: addr=ABX_W(); rmw_isb(nes,addr); nes->cycles+=7; return;

    default: nes->cycles+=2; return;
    }
}
