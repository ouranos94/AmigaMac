/* Sound: a small mixer on the Mac's audio output (AudioQueue), and
 * audio.device on top of it, written from the audio.device autodocs.
 *
 * Paula's four channels become voices of the mixer: 8-bit signed samples,
 * a rate taken from the period (PAL clock), volume 0..64, a number of cycles
 * (0 = until stopped). Writes queue per channel; their end time is known
 * from period, length and cycles, so the scheduler replies each request on
 * time without the audio thread touching the Amiga side.
 * AMIGAMAC_MUTE=1 keeps everything but the sound itself (tests). */
#include "os.h"
#include "sched.h"
#include "audio.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 44100
#define NVOICES 16
#define PAL_CLOCK 3546895.0

typedef struct {
    int8_t *pcm;
    int len, id;
    double pos, step;
    float vol;
    int cycles;                     /* remaining; 0 = forever */
    uint64_t start_us;
    int active;
} Voice;

static Voice voices[NVOICES];
static pthread_mutex_t mix = PTHREAD_MUTEX_INITIALIZER;

/* ---- Paula, driven by programs through its registers ($DFF0A0-$DFF0DF) ----------
 * Each channel plays 8-bit signed samples straight from Amiga memory while its
 * DMA is on; at the end of a block it takes the location and length registers
 * again (the hardware's looping), period and volume as written. */
typedef struct {
    uint32_t lc;                    /* AUDxLC, AUDxLEN (words), AUDxPER, AUDxVOL as written */
    uint16_t len, per, vol;
    uint32_t ptr, left;             /* playing: address, bytes to go */
    double pos;                     /* within the current sample */
    int on;
} PChan;
static PChan pch[4];
/* as the system leaves them after boot: DMA master switch and the display's DMA on,
 * interrupts master switch on (a program only turns its own channels on and off) */
static uint16_t dmacon = 0x03F0, intena = 0x4000, intreq;
static void start_output(void);

/* DMA turned on: the channel takes its block's address and length, and says so (AUDxINT) */
static void pchan_start(PChan *p) {
    p->ptr = p->lc;
    p->left = p->len ? (uint32_t)p->len * 2 : 131072;
    p->pos = 0;
    p->on = 1;
    intreq |= (uint16_t)(0x80 << (p - pch));
}

void paula_write(int reg, uint16_t v) {
    int wake = 0;
    pthread_mutex_lock(&mix);
    if (reg >= 0xA0 && reg < 0xE0) {
        PChan *p = &pch[(reg - 0xA0) >> 4];
        switch (reg & 0x0F) {
        case 0x0: p->lc = (p->lc & 0xFFFF) | (uint32_t)v << 16; break;
        case 0x2: p->lc = (p->lc & 0xFFFF0000u) | (v & 0xFFFE); break;
        case 0x4: p->len = v; break;
        case 0x6: p->per = v; break;
        case 0x8: p->vol = v & 0x7F; break;
        case 0xA:                                               /* AUDxDAT: a word played by hand, then AUDxINT */
            if (!p->on) { intreq |= (uint16_t)(0x80 << ((reg - 0xA0) >> 4)); wake = 1; }
            break;
        }
    } else if (reg == 0x96) {                                   /* DMACON: bit 15 sets, else clears */
        uint16_t old = dmacon;
        dmacon = (v & 0x8000) ? (dmacon | (v & 0x7FFF)) : (dmacon & ~v);
        for (int i = 0; i < 4; i++) {
            int was = (old & 0x200) && (old & (1 << i)), now = (dmacon & 0x200) && (dmacon & (1 << i));
            if (now && !was) {
                pchan_start(&pch[i]);
                static int logged;
                if (getenv("AMIGAMAC_HW") && logged++ < 12)
                    os_log("Paula : voie %d, %u octets à $%06X, période %u, volume %u", i, pch[i].left, pch[i].lc, pch[i].per, pch[i].vol);
            }
            if (!now) pch[i].on = 0;
        }
        pthread_mutex_unlock(&mix);
        if ((dmacon & 0x200) && (dmacon & 0xF)) start_output();   /* the Mac's output, from the first note */
        cia_wake();                                             /* the audio interrupts, now */
        return;
    } else if (reg == 0x9A) intena = (v & 0x8000) ? (intena | (v & 0x7FFF)) : (intena & ~v);
    else if (reg == 0x9C) intreq = (v & 0x8000) ? (intreq | (v & 0x7FFF)) : (intreq & ~v);
    if (reg == 0x9A && (v & 0x8000) && (v & 0x0780)) wake = 1;  /* audio interrupts enabled: pending ones go */
    pthread_mutex_unlock(&mix);
    if (wake) cia_wake();
}
uint32_t paula_dmacon(void) { return dmacon; }
uint32_t paula_intena(void) { return intena; }
uint32_t paula_intreq(void) { return intreq; }
void paula_raise(uint16_t bits) { intreq |= bits; }

/* one output sample of Paula's channels, left and right */
static void paula_mix(float *l, float *r) {
    for (int i = 0; i < 4; i++) {
        PChan *p = &pch[i];
        if (!p->on) continue;
        uint32_t a = p->ptr + (uint32_t)p->pos;
        float s = (a < mem_size ? (int8_t)mem[a] : 0) / 128.0f * (p->vol > 64 ? 64 : p->vol) / 64.0f;
        if (i == 0 || i == 3) *l += s; else *r += s;          /* channels 0 and 3 left, 1 and 2 right */
        p->pos += PAL_CLOCK / (p->per < 124 ? 124 : p->per) / OUT_RATE;
        while (p->pos >= p->left) {                             /* end of the block: the registers again */
            p->pos -= p->left;
            p->ptr = p->lc;
            p->left = p->len ? (uint32_t)p->len * 2 : 131072;
            intreq |= (uint16_t)(0x80 << i);                    /* AUDxINT, as the chip raises it */
        }
    }
}
static AudioQueueRef queue;
static int started, muted, next_id = 1;

static void fill(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)user;
    int16_t *out = buf->mAudioData;
    int frames = (int)(buf->mAudioDataBytesCapacity / 4);
    uint64_t now = now_us();
    pthread_mutex_lock(&mix);
    for (int f = 0; f < frames; f++) {
        float l = 0, r = 0;
        uint64_t t = now + (uint64_t)f * 1000000u / OUT_RATE;
        for (int i = 0; i < NVOICES; i++) {
            Voice *v = &voices[i];
            if (!v->active || t < v->start_us) continue;
            float s = v->pcm[(int)v->pos] / 128.0f * v->vol;
            /* Paula: channels 0 and 3 left, 1 and 2 right; others in the middle */
            int ch = v->id >> 24;
            if (ch == 1 || ch == 4) l += s; else if (ch == 2 || ch == 3) r += s; else { l += s * 0.7f; r += s * 0.7f; }
            v->pos += v->step;
            if (v->pos >= v->len) {
                v->pos -= v->len;
                if (v->cycles && --v->cycles == 0) { v->active = 0; free(v->pcm); v->pcm = 0; }
            }
        }
        if (dmacon & 0x200) paula_mix(&l, &r);
        if (l > 1) l = 1; if (l < -1) l = -1;
        if (r > 1) r = 1; if (r < -1) r = -1;
        out[2 * f] = (int16_t)(l * 32000);
        out[2 * f + 1] = (int16_t)(r * 32000);
    }
    pthread_mutex_unlock(&mix);
    buf->mAudioDataByteSize = (UInt32)(frames * 4);
    AudioQueueEnqueueBuffer(q, buf, 0, 0);
}

static void start_output(void) {
    if (started) return;
    started = 1;
    muted = getenv("AMIGAMAC_MUTE") != 0;
    if (muted) return;
    AudioStreamBasicDescription fmt = { 0 };
    fmt.mSampleRate = OUT_RATE;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = 4;
    if (AudioQueueNewOutput(&fmt, fill, 0, 0, 0, 0, &queue)) { muted = 1; return; }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(queue, 1024 * 4, &b) == 0) { memset(b->mAudioData, 0, 1024 * 4); b->mAudioDataByteSize = 1024 * 4; AudioQueueEnqueueBuffer(queue, b, 0, 0); }
    }
    AudioQueueStart(queue, 0);
}

/* play len bytes of 8-bit signed sound; channel 1-4 = Paula's, 0 = any */
int host_audio_play(const int8_t *pcm, int len, int rate, int volume, int cycles, int channel, uint64_t start_us) {
    if (len <= 0 || rate <= 0) return 0;
    start_output();
    if (muted) return next_id++;
    pthread_mutex_lock(&mix);
    int slot = -1;
    for (int i = 0; i < NVOICES; i++) if (!voices[i].active) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&mix); return 0; }
    Voice *v = &voices[slot];
    v->pcm = malloc((size_t)len);
    memcpy(v->pcm, pcm, (size_t)len);
    v->len = len;
    v->pos = 0;
    v->step = (double)rate / OUT_RATE;
    v->vol = (volume > 64 ? 64 : volume < 0 ? 0 : volume) / 64.0f;
    v->cycles = cycles;
    v->start_us = start_us;
    v->id = (channel & 0xFF) << 24 | (next_id++ & 0xFFFFFF);
    v->active = 1;
    int id = v->id;
    pthread_mutex_unlock(&mix);
    return id;
}

void host_audio_stop(int id) {
    pthread_mutex_lock(&mix);
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].active && (voices[i].id == id || (id < 0 && (voices[i].id >> 24) == -id))) {
            voices[i].active = 0;
            free(voices[i].pcm);
            voices[i].pcm = 0;
        }
    pthread_mutex_unlock(&mix);
}

/* ---- audio.device ------------------------------------------------------------------- */

#define IO_DEVICE 20
#define IO_UNIT 24
#define IO_COMMAND 28
#define IO_FLAGS 30
#define IO_ERROR 31
#define IOA_ALLOCKEY 32
#define IOA_DATA 34
#define IOA_LENGTH 38
#define IOA_PERIOD 42
#define IOA_VOLUME 44
#define IOA_CYCLES 46
#define IOF_QUICK 1
#define ADCMD_FREE 9
#define ADCMD_SETPREC 10
#define ADCMD_FINISH 11
#define ADCMD_PERVOL 12
#define ADCMD_LOCK 13
#define ADCMD_WAITCYCLE 14
#define ADCMD_ALLOCATE 32
#define ADIOERR_ALLOCFAILED (-11)

static uint32_t AudioBase;
static int owner_key[4];                       /* allocation key of each channel, 0 = free */
static int key_seq = 1;
static uint64_t chan_end[4];                   /* when the last queued write of a channel ends */
static int chan_voice[4];

static void complete(uint32_t ior) { if (!(rd8(ior + IO_FLAGS) & IOF_QUICK)) exec_reply_msg(ior); }

/* allocation: ioa_Data points to ioa_Length channel masks, best first */
static int allocate(uint32_t ior) {
    uint32_t arr = rd32(ior + IOA_DATA);
    int n = (int)rd32(ior + IOA_LENGTH);
    int key = (int16_t)rd16(ior + IOA_ALLOCKEY);
    if (!key) key = key_seq++;
    for (int i = 0; i < n; i++) {
        int mask = rd8(arr + (uint32_t)i) & 15, ok = 1;
        for (int ch = 0; ch < 4; ch++) if ((mask & (1 << ch)) && owner_key[ch] && owner_key[ch] != key) ok = 0;
        if (!ok) continue;
        for (int ch = 0; ch < 4; ch++) if (mask & (1 << ch)) owner_key[ch] = key;
        wr32(ior + IO_UNIT, (uint32_t)mask);
        wr16(ior + IOA_ALLOCKEY, (uint32_t)key);
        return 1;
    }
    return 0;
}

static void free_channels(uint32_t ior) {
    uint32_t mask = rd32(ior + IO_UNIT);
    int key = (int16_t)rd16(ior + IOA_ALLOCKEY);
    for (int ch = 0; ch < 4; ch++)
        if ((mask & (1u << ch)) && owner_key[ch] == key) { owner_key[ch] = 0; host_audio_stop(-(ch + 1)); chan_end[ch] = 0; }
}

static uint32_t a_Open(M68K *c) {
    uint32_t ior = c->a[1];
    wr8(ior + IO_ERROR, 0);
    wr32(ior + IO_UNIT, 0);
    if (rd32(ior + IOA_LENGTH) && !allocate(ior)) wr8(ior + IO_ERROR, (uint8_t)ADIOERR_ALLOCFAILED);
    return 0;
}
static uint32_t a_Close(M68K *c) { free_channels(c->a[1]); return 0; }
static uint32_t a_Expunge(M68K *c) { (void)c; return 0; }

static uint32_t a_BeginIO(M68K *c) {
    (void)c;
    uint32_t ior = c->a[1];
    uint32_t mask = rd32(ior + IO_UNIT);
    wr8(ior + IO_ERROR, 0);
    switch (rd16(ior + IO_COMMAND)) {
    case ADCMD_ALLOCATE:
        if (!allocate(ior)) wr8(ior + IO_ERROR, (uint8_t)ADIOERR_ALLOCFAILED);
        complete(ior);
        return 0;
    case ADCMD_FREE: free_channels(ior); complete(ior); return 0;
    case 3: {                                                 /* CMD_WRITE */
        uint32_t data = rd32(ior + IOA_DATA), len = rd32(ior + IOA_LENGTH);
        int period = rd16(ior + IOA_PERIOD), vol = rd16(ior + IOA_VOLUME), cycles = rd16(ior + IOA_CYCLES);
        if (period < 64) period = 64;
        int rate = (int)(PAL_CLOCK / period);
        int ch = 0;
        while (ch < 3 && !(mask & (1u << ch))) ch++;
        const int8_t *p = len ? (const int8_t *)mem_ptr(data, len) : 0;
        uint64_t now = now_us(), start = chan_end[ch] > now ? chan_end[ch] : now;
        if (p) chan_voice[ch] = host_audio_play(p, (int)len, rate, vol, cycles, ch + 1, start);
        if (cycles == 0) {                                    /* forever: replied by a flush or reset */
            chan_end[ch] = UINT64_MAX / 2;
            wr8(ior + IO_FLAGS, rd8(ior + IO_FLAGS) & ~IOF_QUICK);
            return 0;
        }
        uint64_t dur = (uint64_t)len * (uint64_t)cycles * 1000000u / (uint64_t)rate;
        chan_end[ch] = start + dur;
        wr8(ior + IO_FLAGS, rd8(ior + IO_FLAGS) & ~IOF_QUICK);
        sched_timer_add(chan_end[ch], TIMER_REPLY, ior, 0);
        return 0;
    }
    case 1: case 8: case 6:                                   /* CMD_RESET, CMD_FLUSH, CMD_STOP */
    case ADCMD_FINISH:
        for (int ch = 0; ch < 4; ch++) if (mask & (1u << ch)) { host_audio_stop(-(ch + 1)); chan_end[ch] = 0; }
        complete(ior);
        return 0;
    case ADCMD_WAITCYCLE: {
        int ch = 0;
        while (ch < 3 && !(mask & (1u << ch))) ch++;
        uint64_t now = now_us();
        if (chan_end[ch] > now && chan_end[ch] < UINT64_MAX / 2) {
            wr8(ior + IO_FLAGS, rd8(ior + IO_FLAGS) & ~IOF_QUICK);
            sched_timer_add(chan_end[ch], TIMER_REPLY, ior, 0);
            return 0;
        }
        complete(ior);
        return 0;
    }
    default:                                                  /* PERVOL, LOCK, SETPREC, START... */
        complete(ior);
        return 0;
    }
}

static uint32_t a_AbortIO(M68K *c) {
    uint32_t ior = c->a[1];
    if (sched_timer_remove(TIMER_REPLY, ior)) {
        wr8(ior + IO_ERROR, (uint8_t)-2);
        exec_reply_msg(ior);
    }
    return 0;
}

static const NativeEntry audio_natives[] = {
    { "Open", a_Open }, { "Close", a_Close }, { "Expunge", a_Expunge },
    { "BeginIO", a_BeginIO }, { "AbortIO", a_AbortIO },
    { 0, 0 }
};

void audio_init(void) {
    AudioBase = lib_create("audio.device", 0, LIB_SIZE, audio_natives, 40, 1, "audio 40.1 (AmigaMac)");
    wr8(AudioBase + LN_TYPE, NT_DEVICE);
    list_enqueue(SysBase + EB_DEVICELIST, AudioBase);
}
