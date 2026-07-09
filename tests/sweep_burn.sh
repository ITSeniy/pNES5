#!/bin/bash
cd /mnt/c/Users/ITSeniy/source/EmuC0re-main
for b in 350 355 358 360 362 364 365 366 368 370 372 375 380; do
  out=$(./tests/dmc_phase_harness 116 $b 2>&1)
  echo "======== BURN=$b ========"
  echo "$out" | grep -E 'T0=|HIT|NO |relBIT=\+0|V='
  echo "$out" | grep 'halt=06' | tail -2
done
