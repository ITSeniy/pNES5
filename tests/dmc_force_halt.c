#include <stdio.h>
#include <string.h>
#include nes.h
#include mapper.h
/* Measure with stream_defer disabled via patching: rebuild with modified bus */
static int g_n; static s32 g_st; static u16 g_h;
void dmc_phase_log_service(struct NES *n,u16 h,int s){
 g_n++; g_h=h; g_st=n->cycles-s;
 printf(DMA#%d
