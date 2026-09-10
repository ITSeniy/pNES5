#ifndef CORE_H
#define CORE_H

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef long           s64;
typedef int            s32;
typedef short          s16;
typedef signed char    s8;

#define GADGET_OFFSET    0x31AA9
#define LIBKERNEL_HANDLE 0x2001
#define EBOOT_GS_THREAD  0x057F89B0
#define EBOOT_VIDOUT     0x02d695d0

#define SCR_W       1920
#define SCR_H       1080
#define FB_SIZE     (SCR_W * SCR_H * 4)
#define FB_ALIGNED  ((FB_SIZE + 0x1FFFFF) & ~0x1FFFFF)
#define FB_COUNT    3
#define FB_TOTAL    (FB_ALIGNED * FB_COUNT)

#define NES_W  256
#define NES_H  240
/* Legacy defaults (scale mode 0 = pixel-perfect max integer ≈ 4× on 1080p). */
#define SCALE  4
#define OFF_X  ((SCR_W - NES_W * SCALE) / 2)
#define OFF_Y  ((SCR_H - NES_H * SCALE) / 2)

/* Host display scale modes (settings menu). */
#define SCALE_MODE_PIXEL   0  /* max integer fit — pixel perfect */
#define SCALE_MODE_STRETCH 1  /* nearest-neighbour fill 1920×1080 */
#define SCALE_MODE_2X      2
#define SCALE_MODE_3X      3
#define SCALE_MODE_4X      4
#define SCALE_MODE_COUNT   5

#define SAMPLE_RATE     48000
/*
 * Grain size for sceAudioOutOpen / Output. 256 (~5.3 ms) underruns easily when
 * the main loop is vsync-locked; 512 (~10.7 ms) is far more stable on PS5.
 */
#define SAMPLES_PER_BUF 512
#define AUDIO_RING_FRAMES 4096
/* Silence buffers queued before gameplay so the AudioOut ring has headroom. */
#define AUDIO_PRIME_BUFS 4
#define AUDIO_S16_STEREO 1
/*
 * Sample-clock divisors for the APU → 48 kHz resampler (see apu_step).
 *
 * NTSC true master is 1 789 773 Hz (~60.0988 fps). Locked to a 60 Hz display
 * that only yields ~798.7 samples/frame and slowly drains the audio queue
 * (~80 samples/s short) → crackling underruns. 1 786 830 = 29780.5×60 emits
 * ~800 samples/frame at 60 Hz host (pitch +0.17%, inaudible).
 *
 * PAL already averages ~48 kHz when the loop runs 50 NES frames per 60 flips.
 */
#define NTSC_SAMPLE_DIV 1786830
#define PAL_SAMPLE_DIV  1662607

__attribute__((naked))
static u64 native_call(void *gadget, void *fn,
                       u64 a1, u64 a2, u64 a3,
                       u64 a4, u64 a5, u64 a6)
{
    __asm__ volatile (
        "pushq %%rbx\n\t"
        "movq %%rsi, %%rbx\n\t"
        "movq %%rdi, %%rax\n\t"
        "movq %%rdx, %%rdi\n\t"
        "movq %%rcx, %%rsi\n\t"
        "movq %%r8,  %%rdx\n\t"
        "movq %%r9,  %%rcx\n\t"
        "movq 16(%%rsp), %%r8\n\t"
        "movq 24(%%rsp), %%r9\n\t"
        "callq *%%rax\n\t"
        "popq %%rbx\n\t"
        "retq" ::: "memory"
    );
}

static void *resolve_sym(void *gadget, void *dlsym_fn, s32 handle, const char *name) {
    void *addr = 0;
    native_call(gadget, dlsym_fn, (u64)handle, (u64)name, (u64)&addr, 0, 0, 0);
    return addr;
}

#define NC  native_call
#define SYM resolve_sym

#endif
