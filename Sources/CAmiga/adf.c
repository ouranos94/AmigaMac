/* Disk images (.adf): Amiga floppies in the drives DF0: to DF3:.
 *
 * An ADF is the disk's blocks one after the other: 512 bytes each, 1760 for a
 * double density disk (880 KB), 3520 for a high density one. AmigaDOS's file
 * system on it, as publicly described ("The .ADF format FAQ", the AmigaDOS
 * manual): the boot block ("DOS" and the variant: bit 0 fast file system,
 * bit 1 international names, bit 2 directory caches), the root block in the
 * middle of the disk, directories as hash tables of 72 entries chained by
 * name, files as a header block listing their data blocks (backwards, more in
 * extension blocks), each data block 488 bytes after a header (old file
 * system) or 512 plain bytes (fast file system), the free blocks in bitmap
 * blocks.
 *
 * AmigaMac's DOS works on folders, so a disk put in a drive is read whole
 * into a folder of its own (names, dates, protection bits and comments kept,
 * as for every Amiga file here), mounted as DF0: and as the disk's name. When
 * the disk comes out, or AmigaMac ends, a disk whose contents changed is
 * written back: the image is built anew from the folder, with the disk's own
 * boot block and file system.
 *
 * Disks that are not AmigaDOS disks (games with their own track loaders)
 * cannot be read: nothing here plays the floppy drive's hardware.
 */
#include "os.h"
#include "sched.h"
#include "amiga.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define BSIZE 512
#define HT 72                                   /* hash table entries in a 512-byte block */
#define T_HEADER 2
#define T_DATA 8
#define T_LIST 16
#define ST_SOFTLINK 3                           /* ST_ROOT, ST_USERDIR, ST_FILE: amiga.h */
#define ST_LINKDIR 4
#define ST_LINKFILE (-4)
#define MAXBLOCKS 3520

/* offsets in a header block */
#define O_TYPE 0
#define O_KEY 4
#define O_HIGHSEQ 8
#define O_FIRSTDATA 16
#define O_CHKSUM 20
#define O_TABLE 24
#define O_BMFLAG 312
#define O_BMPAGES 316
#define O_PROTECT 320
#define O_SIZE 324
#define O_COMMENT 328
#define O_DAYS 420
#define O_NAME 432
#define O_REAL 468
#define O_VDAYS 472
#define O_CDAYS 484
#define O_CHAIN 496
#define O_PARENT 500
#define O_EXT 504
#define O_SECTYPE 508

extern void dos_set_protection(const char *hp, uint32_t prot);
extern uint32_t dos_protection(const char *hp);
extern void dos_name_latin1(const char *host, char *out, int max);
extern void dos_name_host(const char *latin1, char *out, int max);
extern void dos_mount(const char *device, const char *volume, const char *path);
extern void dos_unmount(const char *device, const char *volume);
extern const char *dos_system_dir(void);

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint32_t block_sum(const uint8_t *b, int skip) {
    uint32_t s = 0;
    for (int i = 0; i < BSIZE; i += 4) if (i != skip) s += be32(b + i);
    return s;
}
static void set_checksum(uint8_t *b) { put32(b + O_CHKSUM, 0); put32(b + O_CHKSUM, (uint32_t)-block_sum(b, O_CHKSUM)); }

static void boot_checksum(uint8_t *img) {
    uint32_t s = 0;
    for (int i = 0; i < 1024; i += 4) {
        if (i == 4) continue;
        uint32_t v = be32(img + i), o = s;
        s += v;
        if (s < o) s++;                                         /* the carry goes round */
    }
    put32(img + 4, ~s);
}

static int upper(int c, int intl) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (intl && c >= 224 && c <= 254 && c != 247) return c - 32;
    return c;
}
static int name_hash(const char *name, int intl) {
    uint32_t h = (uint32_t)strlen(name);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = (h * 13 + (uint32_t)upper(*p, intl)) & 0x7FF;
    return (int)(h % HT);
}

/* Amiga dates (days since 1978, minutes, ticks of 1/50 s, local time) and the host's */
static time_t amiga_time(uint32_t days, uint32_t mins, uint32_t ticks) {
    time_t local = (time_t)252460800 + (time_t)days * 86400 + (time_t)mins * 60 + ticks / 50;
    struct tm lt;
    localtime_r(&local, &lt);
    return local - lt.tm_gmtoff;
}
static void put_date(uint8_t *p, time_t t) {
    struct tm lt;
    localtime_r(&t, &lt);
    long s = (long)(t + lt.tm_gmtoff) - 252460800L;
    if (s < 0) s = 0;
    put32(p, (uint32_t)(s / 86400));
    put32(p + 4, (uint32_t)(s % 86400 / 60));
    put32(p + 8, (uint32_t)(s % 60 * 50));
}

/* ---- reading a disk into a folder ------------------------------------------------- */

typedef struct {
    const uint8_t *img;
    int nblocks, ffs, intl;
    int files, dirs, errors;
    uint8_t seen[MAXBLOCKS];                                    /* against loops in broken disks */
} Rd;

static const uint8_t *blk(Rd *r, uint32_t n) { return n > 1 && n < (uint32_t)r->nblocks ? r->img + (size_t)n * BSIZE : 0; }

static void bstr(const uint8_t *p, int max, char *out) {
    int n = p[0] < max ? p[0] : max;
    memcpy(out, p + 1, (size_t)n);
    out[n] = 0;
}

static void set_meta(const char *hp, const uint8_t *h) {
    char com[80];
    bstr(h + O_COMMENT, 79, com);
    if (com[0]) setxattr(hp, "com.amigamac.comment", com, strlen(com), 0, 0);
    dos_set_protection(hp, be32(h + O_PROTECT));
    time_t t = amiga_time(be32(h + O_DAYS), be32(h + O_DAYS + 4), be32(h + O_DAYS + 8));
    struct timeval tv[2] = { { t, 0 }, { t, 0 } };
    utimes(hp, tv);
}

static int read_file(Rd *r, const uint8_t *h, const char *hp) {
    uint32_t size = be32(h + O_SIZE);
    FILE *f = fopen(hp, "wb");
    if (!f) return 0;
    uint32_t left = size;
    const uint8_t *list = h;
    for (int guard = 0; list && left && guard < MAXBLOCKS; guard++) {
        int n = (int)be32(list + O_HIGHSEQ);
        if (n > HT) n = HT;
        for (int i = 0; i < n && left; i++) {
            const uint8_t *d = blk(r, be32(list + O_TABLE + 4 * (HT - 1 - i)));
            if (!d) { r->errors++; left = 0; break; }
            const uint8_t *data = d;
            uint32_t len = BSIZE;
            if (!r->ffs) {
                data = d + 24; len = be32(d + 12);
                if (len > BSIZE - 24) len = BSIZE - 24;
                if (be32(d + O_KEY) != be32(h + O_KEY)) r->errors++;   /* a block of another file: a crossed link */
            }
            if (len > left) len = left;
            fwrite(data, 1, len, f);
            left -= len;
        }
        list = left ? blk(r, be32(list + O_EXT)) : 0;
    }
    fclose(f);
    if (left) r->errors++;
    return 1;
}

static void read_dir(Rd *r, const uint8_t *dir, const char *hp, int depth) {
    if (depth > 64) return;
    for (int i = 0; i < HT; i++) {
        uint32_t n = be32(dir + O_TABLE + 4 * i);
        for (int guard = 0; n && guard < MAXBLOCKS; guard++) {
            const uint8_t *h = blk(r, n);
            if (!h || r->seen[n]) { r->errors++; break; }
            r->seen[n] = 1;
            char l1[32], name[256], path[2048];
            bstr(h + O_NAME, 30, l1);
            dos_name_host(l1, name, sizeof name);
            snprintf(path, sizeof path, "%s/%s", hp, name);
            int32_t st = (int32_t)be32(h + O_SECTYPE);
            const uint8_t *real = h;
            if (st == ST_LINKFILE || st == ST_LINKDIR) {            /* hard links: what they point to */
                real = blk(r, be32(h + O_REAL));
                if (!real) { r->errors++; real = h; st = 0; }
                else st = (int32_t)be32(real + O_SECTYPE);
            }
            if (st == ST_USERDIR) {
                mkdir(path, 0755);
                if (real == h) read_dir(r, h, path, depth + 1);
                set_meta(path, h);
                r->dirs++;
            } else if (st == ST_FILE) {
                if (read_file(r, real, path)) { set_meta(path, h); r->files++; }
            } else if (st == ST_SOFTLINK) {
                char target[400];
                snprintf(target, sizeof target, "%.*s", BSIZE - 24 - 200, (const char *)(h + O_TABLE));
                os_log("ADF : lien symbolique %s -> %s ignoré", l1, target);
            }
            n = be32(h + O_CHAIN);
        }
    }
}

static int rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s/%s", p, e->d_name);
            rm_tree(c);
        }
        if (d) closedir(d);
        return rmdir(p) == 0;
    }
    return unlink(p) == 0;
}

/* the Mac's own files in a folder, not the disk's: an Amiga ".info" or ".fastdir" stays */
static int host_only(const char *n) {
    return !strcmp(n, ".") || !strcmp(n, "..") || !strcmp(n, ".DS_Store") || !strncmp(n, "._", 2) ||
           !strcmp(n, ".localized") || !strcmp(n, ".Trashes") || !strcmp(n, ".fseventsd") || strstr(n, ".amigamac-tmp");
}

/* ---- writing a folder as a disk ---------------------------------------------------- */

typedef struct {
    uint8_t *img;
    int nblocks, root, next, ffs, intl, full;
    uint8_t used[MAXBLOCKS];
    char too_long[64];
} Wr;

static int alloc_block(Wr *w) {
    for (int k = 0; k < w->nblocks; k++) {
        int n = w->next + k;
        if (n >= w->nblocks) n = 2 + (n - w->nblocks);
        if (n < 2 || n >= w->nblocks || w->used[n]) continue;
        w->used[n] = 1;
        w->next = n + 1;
        memset(w->img + (size_t)n * BSIZE, 0, BSIZE);
        return n;
    }
    w->full = 1;
    return 0;
}

static uint8_t *wblk(Wr *w, int n) { return w->img + (size_t)n * BSIZE; }

static void put_name(uint8_t *p, const char *l1, int max) {
    int n = (int)strlen(l1);
    if (n > max) n = max;
    p[0] = (uint8_t)n;
    memcpy(p + 1, l1, (size_t)n);
}

/* into the parent's hash table, at the end of its chain */
static void link_entry(Wr *w, int parent, int key, const char *l1) {
    uint8_t *pb = wblk(w, parent);
    int i = name_hash(l1, w->intl);
    uint32_t n = be32(pb + O_TABLE + 4 * i);
    if (!n) { put32(pb + O_TABLE + 4 * i, (uint32_t)key); return; }
    for (int guard = 0; guard < MAXBLOCKS; guard++) {
        uint8_t *e = wblk(w, (int)n);
        uint32_t next = be32(e + O_CHAIN);
        if (!next) { put32(e + O_CHAIN, (uint32_t)key); set_checksum(e); return; }   /* its checksum: done before */
        n = next;
    }
}

static void header_meta(uint8_t *h, const char *hp, const char *l1, struct stat *st) {
    put32(h + O_PROTECT, dos_protection(hp));
    char com[80] = "";
    ssize_t cl = getxattr(hp, "com.amigamac.comment", com, sizeof com - 1, 0, 0);
    com[cl > 0 ? cl : 0] = 0;
    put_name(h + O_COMMENT, com, 79);
    put_date(h + O_DAYS, st->st_mtime);
    put_name(h + O_NAME, l1, 30);
}

static int write_file(Wr *w, const char *hp, int parent, const char *l1, struct stat *st) {
    int hk = alloc_block(w);
    if (!hk) return 0;
    uint8_t *h = wblk(w, hk);
    put32(h + O_TYPE, T_HEADER);
    put32(h + O_KEY, (uint32_t)hk);
    put32(h + O_SIZE, (uint32_t)st->st_size);
    header_meta(h, hp, l1, st);
    put32(h + O_PARENT, (uint32_t)parent);
    put32(h + O_SECTYPE, (uint32_t)ST_FILE);
    FILE *f = fopen(hp, "rb");
    int per = w->ffs ? BSIZE : BSIZE - 24;
    long nblk = ((long)st->st_size + per - 1) / per;
    int list = hk, inlist = 0, prev_data = 0;
    for (long i = 0; i < nblk && !w->full; i++) {
        if (inlist == HT) {                                     /* this list is full: an extension block */
            int x = alloc_block(w);
            if (!x) break;
            uint8_t *xb = wblk(w, x);
            put32(xb + O_TYPE, T_LIST);
            put32(xb + O_KEY, (uint32_t)x);
            put32(xb + O_PARENT, (uint32_t)hk);
            put32(xb + O_SECTYPE, (uint32_t)ST_FILE);
            put32(wblk(w, list) + O_EXT, (uint32_t)x);
            if (list != hk) set_checksum(wblk(w, list));
            list = x;
            inlist = 0;
        }
        int d = alloc_block(w);
        if (!d) break;
        uint8_t *db = wblk(w, d);
        size_t got;
        if (w->ffs) got = f ? fread(db, 1, BSIZE, f) : 0;
        else {
            got = f ? fread(db + 24, 1, (size_t)per, f) : 0;
            put32(db + O_TYPE, T_DATA);
            put32(db + O_KEY, (uint32_t)hk);
            put32(db + 8, (uint32_t)(i + 1));
            put32(db + 12, (uint32_t)got);
            if (prev_data) { put32(wblk(w, prev_data) + 16, (uint32_t)d); set_checksum(wblk(w, prev_data)); }
            set_checksum(db);
            prev_data = d;
        }
        (void)got;
        uint8_t *lb = wblk(w, list);
        put32(lb + O_TABLE + 4 * (HT - 1 - inlist), (uint32_t)d);
        inlist++;
        put32(lb + O_HIGHSEQ, (uint32_t)inlist);
        if (i == 0) put32(h + O_FIRSTDATA, (uint32_t)d);
    }
    if (f) fclose(f);
    if (list != hk) set_checksum(wblk(w, list));
    link_entry(w, parent, hk, l1);
    set_checksum(h);
    return !w->full;
}

static int write_dir(Wr *w, const char *hp, int key, int depth) {
    if (depth > 64) return 1;
    DIR *d = opendir(hp);
    struct dirent *e;
    while (d && (e = readdir(d)) && !w->full) {
        if (host_only(e->d_name)) continue;
        char path[2048], l1[256];
        snprintf(path, sizeof path, "%s/%s", hp, e->d_name);
        struct stat st;
        if (lstat(path, &st)) continue;
        dos_name_latin1(e->d_name, l1, sizeof l1);
        if (strlen(l1) > 30) { snprintf(w->too_long, sizeof w->too_long, "%s", l1); l1[30] = 0; }
        if (S_ISDIR(st.st_mode)) {
            int k = alloc_block(w);
            if (!k) break;
            uint8_t *b = wblk(w, k);
            put32(b + O_TYPE, T_HEADER);
            put32(b + O_KEY, (uint32_t)k);
            header_meta(b, path, l1, &st);
            put32(b + O_PARENT, (uint32_t)key);
            put32(b + O_SECTYPE, ST_USERDIR);
            link_entry(w, key, k, l1);
            write_dir(w, path, k, depth + 1);
            set_checksum(wblk(w, k));
        } else if (S_ISREG(st.st_mode)) write_file(w, path, key, l1, &st);
    }
    if (d) closedir(d);
    return !w->full;
}

/* the disk image of a folder: the old image gives the boot block and the file system;
   0 when it does not fit */
static int build_image(const char *hp, const uint8_t *old, int nblocks, const char *label, uint8_t *img) {
    Wr *w = calloc(1, sizeof *w);
    w->img = img;
    w->nblocks = nblocks;
    w->root = nblocks / 2;
    memset(img, 0, (size_t)nblocks * BSIZE);
    memcpy(img, old, 1024);                                     /* the boot block, its code included */
    uint8_t chk[1024];
    memcpy(chk, old, 1024);
    boot_checksum(chk);
    int bootable = !memcmp(chk + 4, old + 4, 4);                /* its checksum was right: keep it right */
    if (img[3] & 4) img[3] = (uint8_t)((img[3] & 3) | 2);       /* directory caches are not kept: international names */
    w->ffs = img[3] & 1;
    w->intl = (img[3] & 2) != 0;
    if (bootable) boot_checksum(img);
    w->used[0] = w->used[1] = 1;
    w->used[w->root] = 1;
    int bm = w->root + 1;
    w->used[bm] = 1;
    w->next = w->root + 2;
    uint8_t *r = wblk(w, w->root);
    const uint8_t *orr = old + (size_t)w->root * BSIZE;
    put32(r + O_TYPE, T_HEADER);
    put32(r + 12, HT);
    put32(r + O_BMFLAG, 0xFFFFFFFFu);
    put32(r + O_BMPAGES, (uint32_t)bm);
    time_t now = time(0);
    put_date(r + O_DAYS, now);                                  /* last change of the root, of the disk */
    put_date(r + O_VDAYS, now);
    memcpy(r + O_CDAYS, orr + O_CDAYS, 12);                     /* its creation: as it was */
    put_name(r + O_NAME, label, 30);
    put32(r + O_SECTYPE, ST_ROOT);
    int ok = write_dir(w, hp, w->root, 0);
    set_checksum(wblk(w, w->root));
    uint8_t *b = wblk(w, bm);                                   /* the bitmap: a set bit is a free block */
    memset(b, 0, BSIZE);
    for (int n = 2; n < nblocks; n++)
        if (!w->used[n]) { int i = n - 2; b[4 + (i / 32) * 4 + 3 - (i % 32) / 8] |= (uint8_t)(1 << (i % 8)); }
    put32(b, 0);
    put32(b, (uint32_t)-block_sum(b, 0));
    if (w->too_long[0]) os_log("ADF : nom trop long pour une disquette, coupé à 30 caractères : %s", w->too_long);
    free(w);
    return ok;
}

/* ---- the drives ------------------------------------------------------------------- */

int adf_eject(int unit, char *msg, int max);

typedef struct {
    int in;
    char file[1024];                                            /* the image on the Mac */
    char dir[1024];                                             /* its contents, here */
    char label[64];                                             /* the disk's name (Latin-1) */
    int nblocks;
    uint8_t boot[1024];
    uint8_t *root;                                              /* the original root block */
    uint64_t sig;                                               /* the folder as read */
} Drive;
static Drive drives[4];
static pthread_mutex_t drive_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t tree_sig(const char *p, uint64_t h, int depth) {
    DIR *d = opendir(p);
    struct dirent *e;
    char names[512][256];
    int n = 0;
    while (d && (e = readdir(d)) && n < 512) if (!host_only(e->d_name)) snprintf(names[n++], 256, "%s", e->d_name);
    if (d) closedir(d);
    qsort(names, (size_t)n, 256, (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i < n; i++) {
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, names[i]);
        struct stat st;
        if (lstat(c, &st)) continue;
        for (const char *s = names[i]; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
        uint64_t v[3] = { (uint64_t)st.st_size, (uint64_t)st.st_mtime, dos_protection(c) };
        for (int k = 0; k < 3; k++) h = (h ^ v[k]) * 1099511628211ull;
        char com[80];
        ssize_t cl = getxattr(c, "com.amigamac.comment", com, sizeof com, 0, 0);
        for (ssize_t k = 0; k < cl; k++) h = (h ^ (uint8_t)com[k]) * 1099511628211ull;
        if (S_ISDIR(st.st_mode) && depth < 64) h = tree_sig(c, h, depth + 1);
    }
    return h;
}

static void disks_dir(char *out, int max) {
    snprintf(out, (size_t)max, "%s/../Disks", dos_system_dir());
    mkdir(out, 0755);
}

extern void workbench_disks_changed(void);
extern void intuition_disk_event(int inserted);

/* puts the image `file` in DFn:; 0 and a message if it cannot */
int adf_insert(int unit, const char *file, char *msg, int max) {
    if (unit < 0 || unit > 3) { snprintf(msg, (size_t)max, "lecteur inconnu"); return 0; }
    FILE *f = fopen(file, "rb");
    if (!f) { snprintf(msg, (size_t)max, "%s : %s", file, strerror(errno)); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size != 1760L * BSIZE && size != 3520L * BSIZE) {
        fclose(f);
        snprintf(msg, (size_t)max, "ce n'est pas une image de disquette Amiga (%ld octets ; 901 120 ou 1 802 240 attendus)", size);
        return 0;
    }
    uint8_t *img = malloc((size_t)size);
    size_t got = fread(img, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(img); snprintf(msg, (size_t)max, "lecture incomplète"); return 0; }
    if (memcmp(img, "DOS", 3) || img[3] > 7) {
        free(img);
        snprintf(msg, (size_t)max, "disquette non AmigaDOS (NDOS) : un jeu à chargeur de pistes, qu'AmigaMac ne lit pas");
        return 0;
    }
    if (drives[unit].in) adf_eject(unit, msg, max);
    Rd *r = calloc(1, sizeof *r);
    r->img = img;
    r->nblocks = (int)(size / BSIZE);
    r->ffs = img[3] & 1;
    r->intl = (img[3] & 6) != 0;
    const uint8_t *root = img + (size_t)(r->nblocks / 2) * BSIZE;
    if (be32(root + O_TYPE) != T_HEADER || (int32_t)be32(root + O_SECTYPE) != ST_ROOT) {
        free(r); free(img);
        snprintf(msg, (size_t)max, "bloc racine introuvable : disquette illisible");
        return 0;
    }
    Drive *d = &drives[unit];
    pthread_mutex_lock(&drive_lock);
    char base[1100];
    disks_dir(base, sizeof base);
    snprintf(d->dir, sizeof d->dir, "%s/DF%d", base, unit);
    rm_tree(d->dir);
    mkdir(d->dir, 0755);
    read_dir(r, root, d->dir, 0);
    bstr(root + O_NAME, 30, d->label);
    if (!d->label[0]) snprintf(d->label, sizeof d->label, "DF%d", unit);
    snprintf(d->file, sizeof d->file, "%s", file);
    d->nblocks = r->nblocks;
    memcpy(d->boot, img, 1024);
    free(d->root);
    d->root = malloc(BSIZE);
    memcpy(d->root, root, BSIZE);
    d->sig = tree_sig(d->dir, 14695981039346656037ull, 0);
    d->in = 1;
    pthread_mutex_unlock(&drive_lock);
    char dev[8], vol[300];
    snprintf(dev, sizeof dev, "DF%d", unit);
    dos_name_host(d->label, vol, sizeof vol);
    dos_mount(dev, vol, d->dir);
    snprintf(msg, (size_t)max, "%s: « %s » : %d fichiers, %d tiroirs, %s%s%s", dev, vol, r->files, r->dirs,
             r->ffs ? "FFS" : "OFS", r->intl ? " international" : "", r->errors ? ", avec des blocs illisibles" : "");
    os_log("ADF : %s", msg);
    free(r);
    free(img);
    intuition_disk_event(1);
    workbench_disks_changed();
    return 1;
}

/* the drive's contents back into its image, if they changed */
static int flush(Drive *d, char *msg, int max) {
    uint64_t sig = tree_sig(d->dir, 14695981039346656037ull, 0);
    if (sig == d->sig) return 1;
    size_t size = (size_t)d->nblocks * BSIZE;
    uint8_t *img = malloc(size), *old = calloc(1, size);
    memcpy(old, d->boot, 1024);
    memcpy(old + (size_t)(d->nblocks / 2) * BSIZE, d->root, BSIZE);
    int ok = build_image(d->dir, old, d->nblocks, d->label, img);
    free(old);
    if (!ok) {
        free(img);
        snprintf(msg, (size_t)max, "« %s » ne tient plus sur la disquette : image non réécrite, contenu gardé dans %s", d->label, d->dir);
        os_log("ADF : %s", msg);
        return 0;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.amigamac-tmp", d->file);
    FILE *f = fopen(tmp, "wb");
    int written = f && fwrite(img, 1, size, f) == size;
    if (f) fclose(f);
    free(img);
    if (!written || rename(tmp, d->file)) {
        unlink(tmp);
        snprintf(msg, (size_t)max, "%s : écriture impossible (%s), contenu gardé dans %s", d->file, strerror(errno), d->dir);
        os_log("ADF : %s", msg);
        return 0;
    }
    d->sig = sig;
    os_log("ADF : « %s » réécrite dans %s", d->label, d->file);
    return 1;
}

int adf_eject(int unit, char *msg, int max) {
    if (unit < 0 || unit > 3 || !drives[unit].in) { snprintf(msg, (size_t)max, "DF%d: est vide", unit < 0 ? 0 : unit); return 0; }
    Drive *d = &drives[unit];
    pthread_mutex_lock(&drive_lock);
    int ok = flush(d, msg, max);
    d->in = 0;
    pthread_mutex_unlock(&drive_lock);
    char dev[8], vol[300];
    snprintf(dev, sizeof dev, "DF%d", unit);
    dos_name_host(d->label, vol, sizeof vol);
    dos_unmount(dev, vol);
    if (ok) { rm_tree(d->dir); snprintf(msg, (size_t)max, "DF%d: éjectée", unit); }
    intuition_disk_event(0);
    workbench_disks_changed();
    return ok;
}

/* AmigaMac ends: the disks' changes go back to their images */
void adf_flush_all(void) {
    char msg[1200];
    pthread_mutex_lock(&drive_lock);
    for (int i = 0; i < 4; i++) if (drives[i].in) flush(&drives[i], msg, sizeof msg);
    pthread_mutex_unlock(&drive_lock);
}

int adf_unit_in(int unit) { return unit >= 0 && unit < 4 && drives[unit].in; }

/* the disk's name as the Workbench shows it (host text), its blocks: used, all */
int adf_unit_info(int unit, char *label, int max, int *used, int *total) {
    if (!adf_unit_in(unit)) return 0;
    Drive *d = &drives[unit];
    if (label) dos_name_host(d->label, label, max);
    if (used || total) {
        uint8_t *img = malloc((size_t)d->nblocks * BSIZE), *old = calloc(1, (size_t)d->nblocks * BSIZE);
        memcpy(old, d->boot, 1024);
        memcpy(old + (size_t)(d->nblocks / 2) * BSIZE, d->root, BSIZE);
        int fits = build_image(d->dir, old, d->nblocks, d->label, img);
        int u = 0;
        const uint8_t *bm = img + (size_t)(d->nblocks / 2 + 1) * BSIZE;
        for (int n = 2; n < d->nblocks; n++) { int i = n - 2; if (!(bm[4 + (i / 32) * 4 + 3 - (i % 32) / 8] & (1 << (i % 8)))) u++; }
        if (used) *used = fits ? u + 2 : d->nblocks;
        if (total) *total = d->nblocks;
        free(img); free(old);
    }
    return 1;
}

/* which drive holds this host path, -1 none */
int adf_unit_of_path(const char *hp) {
    for (int i = 0; i < 4; i++) {
        size_t l = strlen(drives[i].dir);
        if (drives[i].in && !strncmp(hp, drives[i].dir, l) && (hp[l] == 0 || hp[l] == '/')) return i;
    }
    return -1;
}

/* ---- requests from the Mac ----------------------------------------------------------- */

static char pending[4][1024];

/* any thread: the disk goes in through input.device, as the Amiga's disk changes do */
void amiga_disk_insert(int unit, const char *file) {
    if (unit < 0 || unit > 3) return;
    pthread_mutex_lock(&drive_lock);
    snprintf(pending[unit], sizeof pending[unit], "%s", file);
    pthread_mutex_unlock(&drive_lock);
    InputEv e = { IEV_DISK, 0, 1, 0, unit, 0, 0 };
    amiga_input_event(&e);
}
void amiga_disk_eject(int unit) {
    if (unit < 0 || unit > 3) return;
    InputEv e = { IEV_DISK, 0, 0, 0, unit, 0, 0 };
    amiga_input_event(&e);
}
int amiga_disk_present(int unit) { return adf_unit_in(unit); }
void amiga_disk_flush_all(void) { adf_flush_all(); }

/* the input task: the request done, and said */
void adf_input_event(const InputEv *e) {
    char msg[1200], file[1024];
    int unit = e->code;
    if (unit < 0 || unit > 3) return;
    if (e->x) {
        pthread_mutex_lock(&drive_lock);
        snprintf(file, sizeof file, "%s", pending[unit]);
        pthread_mutex_unlock(&drive_lock);
        if (!adf_insert(unit, file, msg, sizeof msg)) os_log("ADF : %s", msg);
    } else adf_eject(unit, msg, sizeof msg);
}
