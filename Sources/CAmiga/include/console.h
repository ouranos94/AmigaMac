/* consoles (console.c): CON:/RAW: windows, console.device, keymap */
#ifndef AMIGA_CONSOLE_H
#define AMIGA_CONSOLE_H
#include "os.h"

typedef struct Con Con;
Con *con_open(M68K *c, const char *spec, int raw);     /* "x/y/w/h/title/options" */
void con_close(M68K *c, Con *k);
void con_ref(Con *k);
void con_write(Con *k, const char *s, int n);
int con_read(Con *k, char *dst, int n);                 /* blocks; 0 = end of file */
int con_ready(Con *k);
void con_set_raw(Con *k, int raw);
int con_is_closed(Con *k);
void keymap_learn(int raw, int ch, int qual);
int keymap_convert(int raw, int qual, char *out, int max);
int keymap_raw_for(int ch, int *qual);
void console_init(void);
#endif
