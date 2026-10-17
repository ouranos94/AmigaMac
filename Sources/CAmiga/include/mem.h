/* 68k address space: one flat big-endian byte array.
 *
 * The OS itself is native, but every structure an Amiga program can see
 * (ExecBase, Task, Library, Window...) lives here in Amiga layout, so the
 * 68k code and the native libraries share the same memory.
 */
#ifndef AMIGA_MEM_H
#define AMIGA_MEM_H

#include <stdint.h>

extern uint8_t *mem;
extern uint32_t mem_size;

void mem_init(uint32_t size);
void mem_fault(uint32_t addr, int write, int size);

static inline int mem_ok(uint32_t a, uint32_t n) { return a < mem_size && n <= mem_size - a; }

static inline uint32_t rd8(uint32_t a) {
    if (!mem_ok(a, 1)) { mem_fault(a, 0, 1); return 0; }
    return mem[a];
}
static inline uint32_t rd16(uint32_t a) {
    if (!mem_ok(a, 2)) { mem_fault(a, 0, 2); return 0; }
    return (uint32_t)mem[a] << 8 | mem[a + 1];
}
static inline uint32_t rd32(uint32_t a) {
    if (!mem_ok(a, 4)) { mem_fault(a, 0, 4); return 0; }
    return (uint32_t)mem[a] << 24 | (uint32_t)mem[a + 1] << 16 | (uint32_t)mem[a + 2] << 8 | mem[a + 3];
}
static inline void wr8(uint32_t a, uint32_t v) {
    if (!mem_ok(a, 1)) { mem_fault(a, 1, 1); return; }
    mem[a] = (uint8_t)v;
}
static inline void wr16(uint32_t a, uint32_t v) {
    if (!mem_ok(a, 2)) { mem_fault(a, 1, 2); return; }
    mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v;
}
static inline void wr32(uint32_t a, uint32_t v) {
    if (!mem_ok(a, 4)) { mem_fault(a, 1, 4); return; }
    mem[a] = (uint8_t)(v >> 24); mem[a + 1] = (uint8_t)(v >> 16);
    mem[a + 2] = (uint8_t)(v >> 8); mem[a + 3] = (uint8_t)v;
}

/* host pointer to a 68k address range (NULL if out of range) */
static inline void *mem_ptr(uint32_t a, uint32_t n) { return mem_ok(a, n) ? mem + a : 0; }

#endif
