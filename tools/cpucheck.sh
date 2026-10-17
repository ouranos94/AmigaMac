#!/bin/sh
# Build and run the CPU cross-check against Musashi (test oracle only).
set -e
cd "$(dirname "$0")"
M=oracle/Musashi
cc -O2 -I../Sources/CAmiga/include -I$M -o cpucheck cpucheck.c ../Sources/CAmiga/cpu68k.c \
   $M/m68kcpu.o $M/m68kops.o $M/m68kdasm.o $M/softfloat.o -lm
./cpucheck "$@"
