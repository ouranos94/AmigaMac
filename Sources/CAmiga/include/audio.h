/* the Mac's sound output (audio.c) */
#ifndef AMIGA_AUDIO_H
#define AMIGA_AUDIO_H
#include <stdint.h>
/* 8-bit signed sound; channel 1-4 = Paula's (0 = any); returns a voice id */
int host_audio_play(const int8_t *pcm, int len, int rate, int volume, int cycles, int channel, uint64_t start_us);
void host_audio_stop(int id);          /* -n: everything on channel n */
void audio_init(void);
void datatypes_init(void);
#endif
