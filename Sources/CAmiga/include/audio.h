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

/* Paula's registers (offset from $DFF000), for programs that drive the chip */
void paula_write(int reg, uint16_t value);
uint32_t paula_dmacon(void);
uint32_t paula_intena(void);
uint32_t paula_intreq(void);
void paula_raise(uint16_t bits);
void cia_wake(void);               /* the interrupt task runs at once (pending audio interrupts) */   /* INTREQ bits the hardware sets (VERTB) */
/* the CIAs (0: CIA-A, 1: CIA-B), register 0-15 */
uint32_t cia_read(int which, int reg);
void cia_write(int which, int reg, uint8_t value);
