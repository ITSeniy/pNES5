#include "nes.h"
#include "mapper.h"
#include "tables.h"

/* Optional host harness hook (tests/dmc_phase_harness.c overrides). */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void dmc_phase_log_service(struct NES *nes, u16 halt, int stall) {
    (void)nes;
    (void)halt;
    (void)stall;
}

static void clock_envelope(struct NES *nes) {
    for (int ch = 0; ch < 2; ch++) {
        if (nes->pulse[ch].env_start) {
            nes->pulse[ch].env_start = 0;
            nes->pulse[ch].env_vol = 15;
            nes->pulse[ch].env_counter = nes->pulse[ch].vol_period;
        } else if (nes->pulse[ch].env_counter > 0) {
            nes->pulse[ch].env_counter--;
        } else {
            nes->pulse[ch].env_counter = nes->pulse[ch].vol_period;
            if (nes->pulse[ch].env_vol > 0) nes->pulse[ch].env_vol--;
            else if (nes->pulse[ch].halt) nes->pulse[ch].env_vol = 15;
        }
    }
    if (nes->noise.env_start) {
        nes->noise.env_start = 0;
        nes->noise.env_vol = 15;
        nes->noise.env_counter = nes->noise.vol_period;
    } else if (nes->noise.env_counter > 0) {
        nes->noise.env_counter--;
    } else {
        nes->noise.env_counter = nes->noise.vol_period;
        if (nes->noise.env_vol > 0) nes->noise.env_vol--;
        else if (nes->noise.halt) nes->noise.env_vol = 15;
    }
    if (nes->tri.linear_reload)
        nes->tri.linear_counter = nes->tri.linear_load;
    else if (nes->tri.linear_counter > 0)
        nes->tri.linear_counter--;
    if (!nes->tri.control)
        nes->tri.linear_reload = 0;
}

static void clock_length_sweep(struct NES *nes) {
    for (int ch = 0; ch < 2; ch++) {
        if (!nes->pulse[ch].halt && nes->pulse[ch].length > 0)
            nes->pulse[ch].length--;
    }
    if (!nes->tri.control && nes->tri.length > 0)
        nes->tri.length--;
    if (!nes->noise.halt && nes->noise.length > 0)
        nes->noise.length--;

    for (int ch = 0; ch < 2; ch++) {
        u16 delta = nes->pulse[ch].timer >> nes->pulse[ch].sweep_shift;
        u16 target;
        if (nes->pulse[ch].sweep_neg) {
            target = nes->pulse[ch].timer - delta;
            if (ch == 0) target--;
        } else {
            target = nes->pulse[ch].timer + delta;
        }
        int muted = (nes->pulse[ch].timer < 8
            || (nes->pulse[ch].sweep_shift && target > 0x7FF));

        if (nes->pulse[ch].sweep_reload) {
            u8 old = nes->pulse[ch].sweep_counter;
            nes->pulse[ch].sweep_counter = nes->pulse[ch].sweep_period;
            if (nes->pulse[ch].sweep_en && nes->pulse[ch].sweep_shift && old == 0 && !muted)
                nes->pulse[ch].timer = target;
            nes->pulse[ch].sweep_reload = 0;
        } else if (nes->pulse[ch].sweep_counter > 0) {
            nes->pulse[ch].sweep_counter--;
        } else {
            nes->pulse[ch].sweep_counter = nes->pulse[ch].sweep_period;
            if (nes->pulse[ch].sweep_en && nes->pulse[ch].sweep_shift && !muted)
                nes->pulse[ch].timer = target;
        }
    }
}

static int pulse_muted(struct pulse_ch *p, int ch) {
    if (p->timer < 8)
        return 1;
    if (p->sweep_shift) {
        u16 delta = p->timer >> p->sweep_shift;
        u16 target = p->sweep_neg
            ? (u16)(p->timer - delta - (ch == 0 ? 1 : 0))
            : (u16)(p->timer + delta);
        if (target > 0x7FF)
            return 1;
    }
    return 0;
}

static void update_apu_irq(struct NES *nes) {
    nes->apu_irq_pending = (nes->frame_irq_flag || nes->dmc.irq_flag) ? 1 : 0;
}

static void dmc_restart(struct NES *nes) {
    nes->dmc.cur_addr = nes->dmc.sample_addr;
    nes->dmc.bytes_left = nes->dmc.sample_len;
}

static void dmc_request_dma(struct NES *nes, int is_load) {
    if (!(nes->dmc.enabled && nes->dmc.bytes_left > 0 && !nes->dmc.sample_buf_full))
        return;
    /*
     * Always queue pending even during reentry (output unit may empty again
     * while a get is finishing). Load delay only for true load DMAs.
     */
    nes->dmc.dma_pending = 1;
    if (is_load && !nes->dmc.dma_reentry) {
        nes->dmc.dma_is_load = 1;
        /*
         * Load DMA attempts halt on the get of the 2nd APU cycle after
         * $4015 — about 3 CPU reads later (AccuracyCoin DMA+$2002).
         */
        nes->dmc.dma_halt_delay = 3;
    }
}

/*
 * One DMC timer tick (≈ one CPU cycle). May set dma_pending when the sample
 * buffer is emptied. Called from the CPU bus after each memory access so that
 * pending can rise mid-instruction (needed for LDA $4000 open-bus DMASync).
 *
 * Timer must NOT run while the channel is fully idle: early ticks before the
 * first $4010/$4015 (period_idx still 0 → 428) start a silence byte and push
 * the first reload hundreds of cycles late, breaking every DMASync residual.
 */
void dmc_tick(struct NES *nes) {
    nes->dmc.ticks_exec++;
    if (!nes->dmc.enabled && nes->dmc.bytes_left == 0 && !nes->dmc.sample_buf_full
        && !nes->dmc.dma_pending)
        return;
    if (nes->dmc.timer_count > 0) {
        nes->dmc.timer_count--;
        return;
    }
    {
        u16 dmc_period = nes->is_pal
            ? dmc_period_pal[nes->dmc.period_idx]
            : dmc_period_ntsc[nes->dmc.period_idx];
        nes->dmc.timer_count = dmc_period ? dmc_period - 1 : 0;
    }

    if (nes->dmc.bits_left == 0) {
        nes->dmc.bits_left = 8;
        if (nes->dmc.sample_buf_full) {
            nes->dmc.shift_reg = nes->dmc.sample_buf;
            nes->dmc.sample_buf_full = 0;
            nes->dmc.silence = 0;
            dmc_request_dma(nes, 0);
        } else {
            nes->dmc.silence = 1;
            dmc_request_dma(nes, 0);
        }
    }

    if (!nes->dmc.silence) {
        if (nes->dmc.shift_reg & 1) {
            if (nes->dmc.output_level <= 125)
                nes->dmc.output_level += 2;
        } else if (nes->dmc.output_level >= 2) {
            nes->dmc.output_level -= 2;
        }
    }
    nes->dmc.shift_reg >>= 1;
    nes->dmc.bits_left--;
}

/* Fetch one DMC sample byte; apply APU register bus conflicts. */
static u8 dmc_fetch_sample(struct NES *nes, u16 a) {
    u8 sample = 0;
    if (a >= 0x8000 && nes->prg && nes->prg_size > 0)
        sample = mapper_prg_read(nes, a);
    else if (a < 0x2000)
        sample = nes->ram[a & 0x7FF];
    else if (a >= 0x6000 && a < 0x8000)
        sample = mapper_cpu_read(nes, a);

    /*
     * APU is partially address-decoded: low 5 bits of the DMA address can
     * activate $4015/$4016/$4017 while PRG also drives the bus.
     * One physical cycle: put sample on the bus first so controller open-bus
     * bits 1–4 see the sample (AccuracyCoin DMC DMA Bus Conflicts).
     */
    {
        u16 apu = (u16)(0x4000 | (a & 0x1F));
        if (apu == 0x4015) {
            /* Internal read: clear frame IRQ; external bus keeps sample. */
            nes->frame_irq_flag = 0;
            nes->apu_irq_pending = nes->dmc.irq_flag ? 1 : 0;
        } else if (apu == 0x4016 || apu == 0x4017) {
            nes->cpu_data_bus = sample;
            u8 joy = cpu_read_nodma(nes, apu);
            /* Bits 5–7 = sample; bits 0–4 = controller port (bit0 + open bus). */
            sample = (u8)((sample & 0xE0) | (joy & 0x1F));
        }
    }
    return sample;
}

/*
 * DMC memory reader (DMA). Only from a CPU *read* path (RDY cannot halt on
 * writes). Normal: halt+dummy[+align] re-read halt_addr, then sample get.
 * Load=3 cyc, reload=4. Abort=1 cyc. During OAM≈2 cyc steal.
 */
void dmc_service_dma(struct NES *nes) {
    if (!nes->dmc.dma_pending || nes->dmc.dma_reentry)
        return;

    /*
     * Aborted DMA (explicit/implicit stop): 1 halt cycle, no sample get.
     * May run even when channel is already disabled / bytes_left == 0.
     */
    if (nes->dmc.dma_abort) {
        nes->dmc.dma_reentry = 1;
        nes->dmc.dma_pending = 0;
        nes->dmc.dma_abort = 0;
        nes->dmc.dma_is_load = 0;
        nes->dmc.dma_halt_delay = 0;
        nes->dmc.sh_h_suppress = 1;
        nes->dmc.sh_suppress_until = nes->total_cycles + nes->cycles + 120;
        nes->cycles += 1;
        if (nes->dmc.halt_addr != 0xFFFF)
            (void)cpu_read_nodma(nes, nes->dmc.halt_addr);
        dmc_tick(nes);
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
    nes->dmc.sh_suppress_until = nes->total_cycles + nes->cycles + 120;

    u16 halt = nes->dmc.halt_addr;
    u16 a = nes->dmc.cur_addr;
    int was_load = nes->dmc.dma_is_load;
    int race = nes->dmc.dma_reload_race;
    nes->dmc.dma_reload_race = 0;

    /*
     * DMC during OAM DMA: common mid-transfer cost is 2 cycles (DMC get +
     * OAM realign); no-op cycles overlap OAM put/get (nesdev wiki).
     */
    int stall;
    if (nes->dmc.oam_dma_active) {
        stall = 2;
        nes->cycles += 2;
        {
            u8 sample = dmc_fetch_sample(nes, a);
            nes->cpu_data_bus = sample;
            nes->dmc.sample_buf = sample;
            nes->dmc.sample_buf_full = 1;
            nes->dmc.bus_hold_ttl = 2;
            if (halt == 0x4016 || halt == 0x4017)
                nes->dmc.joy_oe_hold = 1;
        }
        nes->dmc.dma_is_load = 0;
    } else {
        stall = was_load ? 3 : 4;
        nes->dmc.dma_is_load = 0;
        int dummies = stall - 1;

        nes->cycles += stall;
        for (int i = 0; i < dummies; i++) {
            if (halt != 0xFFFF)
                (void)cpu_read_nodma(nes, halt);
            dmc_tick(nes);
        }
        {
            u8 sample = dmc_fetch_sample(nes, a);
            nes->cpu_data_bus = sample;
            nes->dmc.sample_buf = sample;
            nes->dmc.sample_buf_full = 1;
            nes->dmc.bus_hold_ttl = 2;
            if (halt == 0x4016 || halt == 0x4017)
                nes->dmc.joy_oe_hold = 1;
        }
    }

    /*
     * Advance the reader before the get-cycle timer tick so a same-cycle
     * buffer→shift load cannot request another DMA with a stale bytes_left
     * (1-byte non-loop left a phantom pending and skewed DMASync residual).
     */
    nes->dmc.cur_addr++;
    if (nes->dmc.cur_addr == 0)
        nes->dmc.cur_addr = 0x8000;

    nes->dmc.bytes_left--;
    if (nes->dmc.bytes_left == 0) {
        if (nes->dmc.loop) {
            dmc_restart(nes);
        } else {
            if (nes->dmc.irq_enable) {
                nes->dmc.irq_flag = 1;
                update_apu_irq(nes);
            }
            /*
             * Implicit abort: load DMA of a finishing non-loop sample (esp.
             * 1-byte) can leave a 1-cycle aborted reload (AccuracyCoin).
             * Reload sample ends must NOT do this — that broke DMASync phase.
             */
            if (was_load || race) {
                nes->dmc.dma_pending = 1;
                nes->dmc.dma_abort = 1;
                nes->dmc.dma_halt_delay = 0;
            }
        }
    }

    /* Get-cycle (and OAM second cycle) timer clocks after reader update. */
    if (nes->dmc.oam_dma_active) {
        dmc_tick(nes);
        dmc_tick(nes);
    } else {
        dmc_tick(nes);
    }

    dmc_phase_log_service(nes, halt, stall);
    nes->dmc.dma_reentry = 0;
}

static void frame_reset(struct NES *nes) {
    nes->frame_counter = 0;
    nes->frame_mode = nes->frame_reset_mode;
    if (nes->frame_mode) {
        clock_envelope(nes);
        clock_length_sweep(nes);
    }
}

void apu_write_reg(struct NES *nes, u16 addr, u8 val) {
    int ch;
    switch (addr) {
    case 0x4000: case 0x4004:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].duty = (val >> 6) & 3;
        nes->pulse[ch].halt = (val >> 5) & 1;
        nes->pulse[ch].const_vol = (val >> 4) & 1;
        nes->pulse[ch].vol_period = val & 0xF;
        break;
    case 0x4001: case 0x4005:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].sweep_en = (val >> 7) & 1;
        nes->pulse[ch].sweep_period = (val >> 4) & 7;
        nes->pulse[ch].sweep_neg = (val >> 3) & 1;
        nes->pulse[ch].sweep_shift = val & 7;
        nes->pulse[ch].sweep_reload = 1;
        break;
    case 0x4002: case 0x4006:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].timer = (nes->pulse[ch].timer & 0x700) | val;
        break;
    case 0x4003: case 0x4007:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].timer = (nes->pulse[ch].timer & 0xFF) | ((val & 7) << 8);
        if (nes->pulse[ch].enabled)
            nes->pulse[ch].length = length_table[(val >> 3) & 0x1F];
        nes->pulse[ch].env_start = 1;
        nes->pulse[ch].duty_pos = 0;
        break;
    case 0x4008:
        nes->tri.control = (val >> 7) & 1;
        nes->tri.linear_load = val & 0x7F;
        break;
    case 0x400A:
        nes->tri.timer = (nes->tri.timer & 0x700) | val;
        break;
    case 0x400B:
        nes->tri.timer = (nes->tri.timer & 0xFF) | ((val & 7) << 8);
        if (nes->tri.enabled)
            nes->tri.length = length_table[(val >> 3) & 0x1F];
        nes->tri.linear_reload = 1;
        break;
    case 0x400C:
        nes->noise.halt = (val >> 5) & 1;
        nes->noise.const_vol = (val >> 4) & 1;
        nes->noise.vol_period = val & 0xF;
        break;
    case 0x400E:
        nes->noise.mode = (val >> 7) & 1;
        nes->noise.period_idx = val & 0xF;
        break;
    case 0x400F:
        if (nes->noise.enabled)
            nes->noise.length = length_table[(val >> 3) & 0x1F];
        nes->noise.env_start = 1;
        break;
    case 0x4010:
        nes->dmc.irq_enable = (val >> 7) & 1;
        nes->dmc.loop = (val >> 6) & 1;
        nes->dmc.period_idx = val & 0x0F;
        if (!nes->dmc.irq_enable) {
            nes->dmc.irq_flag = 0;
            update_apu_irq(nes);
        }
        break;
    case 0x4011:
        nes->dmc.output_level = val & 0x7F;
        break;
    case 0x4012:
        nes->dmc.sample_addr = 0xC000 | ((u16)val << 6);
        break;
    case 0x4013:
        nes->dmc.sample_len = ((u16)val << 4) | 1;
        break;
    case 0x4015:
        nes->dmc.irq_flag = 0;
        update_apu_irq(nes);
        nes->pulse[0].enabled = val & 1;
        nes->pulse[1].enabled = (val >> 1) & 1;
        nes->tri.enabled = (val >> 2) & 1;
        nes->noise.enabled = (val >> 3) & 1;
        nes->dmc.enabled = (val >> 4) & 1;
        if (!nes->pulse[0].enabled) nes->pulse[0].length = 0;
        if (!nes->pulse[1].enabled) nes->pulse[1].length = 0;
        if (!nes->tri.enabled) nes->tri.length = 0;
        if (!nes->noise.enabled) nes->noise.length = 0;
        if (!nes->dmc.enabled) {
            /*
             * Explicit stop: only a *already-pending* DMA becomes a 1-cycle
             * abort (cannot invent aborts when nothing was scheduled — that
             * stole a cycle on every $4015 clear and broke DMA phase).
             * Abort cannot halt on this write; it runs on the next read.
             */
            if (nes->dmc.dma_pending) {
                nes->dmc.dma_abort = 1;
                nes->dmc.dma_halt_delay = 0;
            } else {
                nes->dmc.dma_abort = 0;
            }
            nes->dmc.bytes_left = 0;
            nes->dmc.dma_is_load = 0;
            /* dma_pending kept if abort, else clear */
            if (!nes->dmc.dma_abort)
                nes->dmc.dma_pending = 0;
        } else if (nes->dmc.bytes_left == 0) {
            /*
             * Load DMA: schedule only — halt after dma_halt_delay reads.
             * If a reload was already pending (1-byte non-loop race), the
             * pending get becomes an abort after this load completes — see
             * dmc_service_dma / dma_reload_race.
             *
             * Arm the timer for a full period at the *current* rate without
             * processing a bit. timer_count==0 would expire on the next
             * dmc_tick and start a silence byte before the load DMA fills
             * the buffer, delaying the first reload by ~7 bit-periods.
             */
            u8 had_reload = nes->dmc.dma_pending && !nes->dmc.dma_is_load;
            {
                u16 dmc_period = nes->is_pal
                    ? dmc_period_pal[nes->dmc.period_idx]
                    : dmc_period_ntsc[nes->dmc.period_idx];
                nes->dmc.timer_count = dmc_period ? dmc_period - 1 : 0;
                nes->dmc.bits_left = 0;
                nes->dmc.silence = 1;
            }
            dmc_restart(nes);
            dmc_request_dma(nes, 1);
            if (had_reload)
                nes->dmc.dma_reload_race = 1;
        }
        break;
    case 0x4017:
        nes->frame_mode = (val >> 7) & 1;
        nes->frame_irq_inhibit = (val >> 6) & 1;
        if (nes->frame_irq_inhibit) {
            nes->frame_irq_flag = 0;
            update_apu_irq(nes);
        }
        nes->frame_reset_mode = nes->frame_mode;
        if ((nes->total_cycles + 1) & 1) nes->frame_reset_delay = 1;
        else frame_reset(nes);
        break;
    }
}

void apu_step(struct NES *nes, int cycles) {
    for (int c = 0; c < cycles; c++) {
        if (nes->frame_reset_delay) {
            nes->frame_reset_delay--;
            if (!nes->frame_reset_delay) {
                frame_reset(nes);
            }
        }

        /*
         * Do NOT run dmc_service_dma here. DMA must occur on a CPU memory
         * access (see cpu_read) so that:
         *  - LDA $4000 open-bus sync sees the sample on the data bus
         *  - SH* can observe RDY/H-suppress on the correct instruction
         */

        if ((nes->total_cycles + c) & 1) {
            for (int ch = 0; ch < 2; ch++) {
                if (nes->pulse[ch].timer_count > 0) nes->pulse[ch].timer_count--;
                else {
                    nes->pulse[ch].timer_count = nes->pulse[ch].timer;
                    nes->pulse[ch].duty_pos = (nes->pulse[ch].duty_pos + 1) & 7;
                }
            }
        }

        if (nes->tri.timer_count > 0) nes->tri.timer_count--;
        else {
            nes->tri.timer_count = nes->tri.timer;
            if (nes->tri.length > 0 && nes->tri.linear_counter > 0)
                nes->tri.step = (nes->tri.step + 1) & 31;
        }

        if (nes->noise.timer_count > 0) nes->noise.timer_count--;
        else {
            nes->noise.timer_count = nes->is_pal
                ? noise_period_pal[nes->noise.period_idx]
                : noise_period_ntsc[nes->noise.period_idx];
            int fb = nes->noise.mode
                ? ((nes->noise.shift_reg & 1) ^ ((nes->noise.shift_reg >> 6) & 1))
                : ((nes->noise.shift_reg & 1) ^ ((nes->noise.shift_reg >> 1) & 1));
            nes->noise.shift_reg = (nes->noise.shift_reg >> 1) | (fb << 14);
        }

        /*
         * DMC timer is clocked from the CPU bus (dmc_tick) on each memory
         * access. Do not tick here or DMASync open-bus timing double-counts.
         */

        nes->frame_counter++;
        int *steps = nes->fc_step[nes->frame_mode];
        if      (nes->frame_counter == steps[0]) clock_envelope(nes);
        else if (nes->frame_counter == steps[1]) { clock_envelope(nes); clock_length_sweep(nes); }
        else if (nes->frame_counter == steps[2]) clock_envelope(nes);
        else if (nes->frame_counter == steps[4]) { clock_envelope(nes); clock_length_sweep(nes); }
        /*
         * Frame IRQ: set on the three step-4 clocks (not level-reasserted
         * every cycle). Once cleared by $4015 read it must stay clear until
         * the next step-4 (AccuracyCoin DMA+$4015 BIT after dummies).
         */
        if (!nes->frame_mode && !nes->frame_irq_inhibit
            && (nes->frame_counter == steps[3]
                || nes->frame_counter == steps[4]
                || nes->frame_counter == steps[5]))
            nes->frame_irq_flag = 1;
        if (nes->frame_counter >= steps[5])
            nes->frame_counter = 3;

        update_apu_irq(nes);

        nes->sample_acc += SAMPLE_RATE;
        if (nes->sample_acc >= nes->cpu_freq) {
            nes->sample_acc -= nes->cpu_freq;

            int p1 = 0, p2 = 0, tri_out = 0, noi = 0, dmc = nes->dmc.output_level;
            for (int ch = 0; ch < 2; ch++) {
                if (!nes->pulse[ch].length || nes->pulse[ch].timer < 8) continue;
                if (pulse_muted(&nes->pulse[ch], ch)) continue;
                if (!duty_wave[nes->pulse[ch].duty][nes->pulse[ch].duty_pos]) continue;
                u8 vol = nes->pulse[ch].const_vol ? nes->pulse[ch].vol_period : nes->pulse[ch].env_vol;
                if (ch == 0) p1 = vol; else p2 = vol;
            }
            if (nes->tri.length > 0 && nes->tri.linear_counter > 0 && nes->tri.timer >= 2)
                tri_out = tri_wave[nes->tri.step];
            if (nes->noise.length > 0 && !(nes->noise.shift_reg & 1))
                noi = nes->noise.const_vol ? nes->noise.vol_period : nes->noise.env_vol;

            int ps = p1 + p2; if (ps > 30) ps = 30;
            int ts = 3 * tri_out + 2 * noi + dmc; if (ts > 202) ts = 202;
            s32 raw = (s32)mix_pulse(ps) + (s32)mix_tnd(ts);

            s32 lp = (raw * 6 + nes->lpf_prev * 10) / 16;
            nes->lpf_prev = (s16)lp;
            s32 hp = lp - nes->hpf_in + (nes->hpf_out * 255 / 256);
            nes->hpf_in = lp;
            nes->hpf_out = hp;

            s16 sample = (s16)(hp > 32767 ? 32767 : (hp < -32768 ? -32768 : hp));
            if (nes->audio_pos < 2048) {
                nes->audio_buf[nes->audio_pos * 2] = sample;
                nes->audio_buf[nes->audio_pos * 2 + 1] = sample;
                nes->audio_pos++;
            }
        }
    }
    nes->dma_read_pending = 0;
}

void apu_flush(struct NES *nes) {
    if (nes->audio_handle < 0 || !nes->audio_out_fn) return;
    while (nes->audio_pos >= SAMPLES_PER_BUF) {
        NC(nes->gadget, nes->audio_out_fn,
           (u64)nes->audio_handle, (u64)nes->audio_buf, 0, 0, 0, 0);
        int rem = nes->audio_pos - SAMPLES_PER_BUF;
        for (int i = 0; i < rem * 2; i++)
            nes->audio_buf[i] = nes->audio_buf[SAMPLES_PER_BUF * 2 + i];
        nes->audio_pos = rem;
    }
}

void apu_prime(struct NES *nes, int buffers) {
    if (nes->audio_handle < 0 || !nes->audio_out_fn) return;

    for (int i = 0; i < SAMPLES_PER_BUF * 2; i++)
        nes->audio_buf[i] = 0;

    for (int i = 0; i < buffers; i++) {
        NC(nes->gadget, nes->audio_out_fn,
           (u64)nes->audio_handle, (u64)nes->audio_buf, 0, 0, 0, 0);
    }

    nes->audio_pos = 0;
}
