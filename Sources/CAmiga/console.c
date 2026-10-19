/* Consoles, written from the console.device and dos autodocs:
 *   - CON: and RAW: windows opened by dos Open() ("CON:x/y/w/h/title/opts")
 *   - console.device units attached to a program's window
 *   - keymap.library MapRawKey/MapANSI and console RawKeyConvert
 * One terminal engine serves all of them: a character grid drawn into the
 * window with the system font, the Amiga console control sequences (CSI
 * cursor motion, erase, insert/delete, SGR colours, window bounds report),
 * scrolling and resizing. Keys reach a console through a native hook on the
 * window, called by the input task: cooked consoles edit a line and echo it,
 * raw ones queue every key (cursor and function keys as CSI sequences). */
#include "intui.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include "console.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSI 0x9B

struct Con {
    uint32_t win;
    int own_window;                 /* opened by CON: (closed with it) */
    int refs;
    int cols, rows, cx, cy;
    int fw, fh, base;               /* a cell: the system font's width and height, its baseline */
    uint8_t *ch, *at;               /* grid: character, pens (fg << 4 | bg) */
    int fg, bg, inverse, cursor_on, cursor_drawn;
    /* input */
    char inq[2048];
    int inlen;
    char line[512];
    int linelen;
    int raw, echo, eof, closed;
    uint32_t reader;                /* task waiting in con_read */
    int sigbit;
    uint32_t owner;                 /* gets Ctrl-C/D/E/F */
    int wait_close;
    /* escape sequences */
    int esc;                        /* 0, 1 after ESC, 2 inside CSI */
    char seq[40];
    int seqlen;
    /* console.device */
    uint32_t unit;
    uint32_t pending[16];
    int npending;
    Con *next;
};

static Con *cons;

/* ---- drawing ------------------------------------------------------------------ */

static NWindow *nw_of(Con *k) { return nwin_of(k->win); }

static void origin(Con *k, int *x0, int *y0) {
    *x0 = rd8(k->win + Window_BorderLeft) + 2;
    *y0 = rd8(k->win + Window_BorderTop) + 1;
}

static void grid_size(Con *k, int *cols, int *rows) {
    int iw = (int16_t)rd16(k->win + Window_Width) - rd8(k->win + Window_BorderLeft) - rd8(k->win + Window_BorderRight) - 4;
    int ih = (int16_t)rd16(k->win + Window_Height) - rd8(k->win + Window_BorderTop) - rd8(k->win + Window_BorderBottom) - 2;
    *cols = iw / k->fw < 1 ? 1 : iw / k->fw > 255 ? 255 : iw / k->fw;
    *rows = ih / k->fh < 1 ? 1 : ih / k->fh > 200 ? 200 : ih / k->fh;
}

static void draw_cell(Con *k, int x, int y) {
    uint32_t rp = rd32(k->win + Window_RPort);
    int x0, y0;
    origin(k, &x0, &y0);
    int i = y * k->cols + x;
    uint8_t a = k->at[i];
    char c = (char)k->ch[i];
    uint32_t ofg = rd8(rp + RastPort_FgPen), obg = rd8(rp + RastPort_BgPen), om = rd8(rp + RastPort_DrawMode);
    wr8(rp + RastPort_FgPen, a >> 4);
    wr8(rp + RastPort_BgPen, a & 15);
    wr8(rp + RastPort_DrawMode, JAM2);
    rp_move(rp, x0 + x * k->fw, y0 + y * k->fh + k->base);
    rp_text(rp, &c, 1);
    wr8(rp + RastPort_FgPen, ofg);
    wr8(rp + RastPort_BgPen, obg);
    wr8(rp + RastPort_DrawMode, om);
}

static void redraw_all(Con *k) {
    for (int y = 0; y < k->rows; y++) for (int x = 0; x < k->cols; x++) draw_cell(k, x, y);
    gfx_touch();
}

static void cursor(Con *k, int show) {
    if (!k->win || show == k->cursor_drawn) return;
    if (show && !k->cursor_on) return;
    int x0, y0, cx = k->cx < k->cols ? k->cx : k->cols - 1;
    origin(k, &x0, &y0);
    rp_fill(rd32(k->win + Window_RPort), x0 + cx * k->fw, y0 + k->cy * k->fh, x0 + (cx + 1) * k->fw - 1, y0 + (k->cy + 1) * k->fh - 1, 0, 1);
    k->cursor_drawn = show;
    gfx_touch();
}

static uint8_t attr(Con *k) { return k->inverse ? (uint8_t)(k->bg << 4 | k->fg) : (uint8_t)(k->fg << 4 | k->bg); }

static void clear_cells(Con *k, int from, int to) {          /* [from, to) in grid order */
    for (int i = from; i < to && i < k->cols * k->rows; i++) {
        k->ch[i] = ' ';
        k->at[i] = (uint8_t)(1 << 4 | k->bg);
        draw_cell(k, i % k->cols, i / k->cols);
    }
}

/* move rows [top, rows) up by n (n < 0: down), with the bitmap */
static void scroll(Con *k, int top, int n) {
    if (!n) return;
    int span = k->rows - top;
    if (n >= span || -n >= span) { clear_cells(k, top * k->cols, k->rows * k->cols); return; }
    NWindow *w = nw_of(k);
    int x0, y0;
    origin(k, &x0, &y0);
    BM b;
    if (w && bm_load(w->bm, &b)) {
        int fw = k->fw, fh = k->fh;
        if (n > 0) bm_copy(&b, x0, y0 + (top + n) * fh, &b, x0, y0 + top * fh, k->cols * fw, (span - n) * fh, 0xC0, 0xFF);
        else {
            /* overlapping downward copy: go through a temporary bitmap */
            uint32_t tmp = gfx_alloc_bitmap(k->cols * fw, (span + n) * fh, b.depth, 0);
            BM t;
            bm_load(tmp, &t);
            bm_copy(&b, x0, y0 + top * fh, &t, 0, 0, k->cols * fw, (span + n) * fh, 0xC0, 0xFF);
            bm_copy(&t, 0, 0, &b, x0, y0 + (top - n) * fh, k->cols * fw, (span + n) * fh, 0xC0, 0xFF);
            gfx_free_bitmap(tmp);
        }
    }
    size_t row = (size_t)k->cols;
    if (n > 0) {
        memmove(k->ch + top * row, k->ch + (top + n) * row, (size_t)(span - n) * row);
        memmove(k->at + top * row, k->at + (top + n) * row, (size_t)(span - n) * row);
        int f = (k->rows - n) * k->cols;
        for (int i = f; i < k->rows * k->cols; i++) { k->ch[i] = ' '; k->at[i] = (uint8_t)(1 << 4 | k->bg); }
        for (int y = k->rows - n; y < k->rows; y++) for (int x = 0; x < k->cols; x++) draw_cell(k, x, y);
    } else {
        int m = -n;
        memmove(k->ch + (top + m) * row, k->ch + top * row, (size_t)(span - m) * row);
        memmove(k->at + (top + m) * row, k->at + top * row, (size_t)(span - m) * row);
        for (int i = top * k->cols; i < (top + m) * k->cols; i++) { k->ch[i] = ' '; k->at[i] = (uint8_t)(1 << 4 | k->bg); }
        for (int y = top; y < top + m; y++) for (int x = 0; x < k->cols; x++) draw_cell(k, x, y);
    }
    gfx_touch();
}

static void newline(Con *k) {
    k->cx = 0;
    if (++k->cy >= k->rows) { scroll(k, 0, k->cy - k->rows + 1); k->cy = k->rows - 1; }
}

static void put_char(Con *k, uint8_t c) {
    if (k->cx >= k->cols) newline(k);
    int i = k->cy * k->cols + k->cx;
    k->ch[i] = c;
    k->at[i] = attr(k);
    draw_cell(k, k->cx, k->cy);
    k->cx++;
}

static void con_resize(Con *k) {
    int cols, rows;
    grid_size(k, &cols, &rows);
    uint8_t *ch = malloc((size_t)cols * rows), *at = malloc((size_t)cols * rows);
    memset(ch, ' ', (size_t)cols * rows);
    memset(at, 1 << 4, (size_t)cols * rows);
    /* keep the bottom of the old text */
    int shift = k->cy >= rows ? k->cy - rows + 1 : 0;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            int oy = y + shift;
            if (k->ch && oy < k->rows && x < k->cols) { ch[y * cols + x] = k->ch[oy * k->cols + x]; at[y * cols + x] = k->at[oy * k->cols + x]; }
        }
    free(k->ch);
    free(k->at);
    k->ch = ch;
    k->at = at;
    k->cols = cols;
    k->rows = rows;
    k->cy -= shift;
    if (k->cx > cols) k->cx = cols;
    k->cursor_drawn = 0;
    redraw_all(k);
}

/* ---- control sequences ------------------------------------------------------------- */

static void queue_input(Con *k, const char *s, int n);

static int param(const char *s, int idx, int def) {
    int i = 0;
    while (idx > 0 && *s) { if (*s == ';') idx--; s++; }
    if (!*s || *s == ';' || !isdigit((unsigned char)*s)) return def;
    for (; isdigit((unsigned char)*s); s++) i = i * 10 + (*s - '0');
    return i;
}

static void csi(Con *k, char final) {
    char *p = k->seq;
    int n1 = param(p, 0, 1);
    switch (final) {
    case 'A': k->cy -= n1; if (k->cy < 0) k->cy = 0; break;
    case 'B': k->cy += n1; if (k->cy >= k->rows) k->cy = k->rows - 1; break;
    case 'C': k->cx += n1; if (k->cx >= k->cols) k->cx = k->cols - 1; break;
    case 'D': k->cx -= n1; if (k->cx < 0) k->cx = 0; break;
    case 'E': k->cx = 0; k->cy += n1; if (k->cy >= k->rows) k->cy = k->rows - 1; break;
    case 'F': k->cx = 0; k->cy -= n1; if (k->cy < 0) k->cy = 0; break;
    case 'G': k->cx = n1 - 1; if (k->cx >= k->cols) k->cx = k->cols - 1; if (k->cx < 0) k->cx = 0; break;
    case 'H': case 'f':
        k->cy = param(p, 0, 1) - 1;
        k->cx = param(p, 1, 1) - 1;
        if (k->cy < 0) k->cy = 0;
        if (k->cy >= k->rows) k->cy = k->rows - 1;
        if (k->cx < 0) k->cx = 0;
        if (k->cx >= k->cols) k->cx = k->cols - 1;
        break;
    case 'J': {
        int mode = param(p, 0, 0);
        if (mode == 2) clear_cells(k, 0, k->rows * k->cols);
        else if (mode == 1) clear_cells(k, 0, k->cy * k->cols + k->cx + 1);
        else clear_cells(k, k->cy * k->cols + k->cx, k->rows * k->cols);
        break;
    }
    case 'K': {
        int mode = param(p, 0, 0), row = k->cy * k->cols;
        if (mode == 2) clear_cells(k, row, row + k->cols);
        else if (mode == 1) clear_cells(k, row, row + k->cx + 1);
        else clear_cells(k, row + k->cx, row + k->cols);
        break;
    }
    case '@': case 'P': {                                       /* insert / delete characters */
        int row = k->cy * k->cols, n = n1;
        if (n > k->cols - k->cx) n = k->cols - k->cx;
        if (final == '@') {
            memmove(k->ch + row + k->cx + n, k->ch + row + k->cx, (size_t)(k->cols - k->cx - n));
            memmove(k->at + row + k->cx + n, k->at + row + k->cx, (size_t)(k->cols - k->cx - n));
            for (int i = 0; i < n; i++) { k->ch[row + k->cx + i] = ' '; k->at[row + k->cx + i] = attr(k); }
        } else {
            memmove(k->ch + row + k->cx, k->ch + row + k->cx + n, (size_t)(k->cols - k->cx - n));
            memmove(k->at + row + k->cx, k->at + row + k->cx + n, (size_t)(k->cols - k->cx - n));
            for (int i = k->cols - n; i < k->cols; i++) { k->ch[row + i] = ' '; k->at[row + i] = attr(k); }
        }
        for (int x = k->cx; x < k->cols; x++) draw_cell(k, x, k->cy);
        break;
    }
    case 'L': scroll(k, k->cy, -n1); break;                     /* insert lines */
    case 'M': scroll(k, k->cy, n1); break;                      /* delete lines */
    case 'S': scroll(k, 0, n1); break;
    case 'T': scroll(k, 0, -n1); break;
    case 'm':
        for (int i = 0;; i++) {
            int v = param(p, i, -1);
            if (v < 0) { if (i == 0) { k->fg = 1; k->bg = 0; k->inverse = 0; } break; }
            if (v == 0) { k->fg = 1; k->bg = 0; k->inverse = 0; }
            else if (v == 7) k->inverse = 1;
            else if (v == 27) k->inverse = 0;
            else if (v >= 30 && v <= 37) k->fg = v - 30;
            else if (v == 39) k->fg = 1;
            else if (v >= 40 && v <= 47) k->bg = v - 40;
            else if (v == 49) k->bg = 0;
        }
        break;
    case 'p':                                                   /* " p": cursor on/off */
        if (strchr(p, ' ')) k->cursor_on = param(p, 0, 1) != 0;
        break;
    case 'q':                                                   /* " q": window status request */
        if (strchr(p, ' ')) {
            char r[40];
            int n = snprintf(r, sizeof r, "\x9b" "1;1;%d;%d r", k->rows, k->cols);
            queue_input(k, r, n);
        }
        break;
    case 'n':                                                   /* 6n: cursor position report */
        if (param(p, 0, 0) == 6) {
            char r[40];
            int n = snprintf(r, sizeof r, "\x9b%d;%dR", k->cy + 1, k->cx + 1);
            queue_input(k, r, n);
        }
        break;
    default: break;                                             /* t u x y { } ... */
    }
}

void con_write(Con *k, const char *s, int n) {
    if (!k || !k->win || !nw_of(k)) return;
    cursor(k, 0);
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (k->esc == 1) {
            if (c == '[') { k->esc = 2; k->seqlen = 0; k->seq[0] = 0; continue; }
            k->esc = 0;
            if (c == 'c') { k->fg = 1; k->bg = 0; k->inverse = 0; clear_cells(k, 0, k->rows * k->cols); k->cx = k->cy = 0; }
            continue;
        }
        if (k->esc == 2) {
            if ((c >= '0' && c <= '9') || c == ';' || c == ' ' || c == '?' || c == '>') {
                if (k->seqlen < (int)sizeof k->seq - 1) { k->seq[k->seqlen++] = (char)c; k->seq[k->seqlen] = 0; }
                continue;
            }
            k->esc = 0;
            csi(k, (char)c);
            continue;
        }
        switch (c) {
        case 0x1B: k->esc = 1; break;
        case CSI: k->esc = 2; k->seqlen = 0; k->seq[0] = 0; break;
        case '\n': case 0x85: newline(k); break;
        case '\r': k->cx = 0; break;
        case '\b': if (k->cx > 0) k->cx--; break;
        case '\t': k->cx = (k->cx + 8) & ~7; if (k->cx >= k->cols) newline(k); break;
        case '\f': clear_cells(k, 0, k->rows * k->cols); k->cx = k->cy = 0; break;
        case 0x0B: case 0x8D: if (k->cy > 0) k->cy--; else scroll(k, 0, -1); break;
        case 0x84: if (++k->cy >= k->rows) { scroll(k, 0, 1); k->cy = k->rows - 1; } break;
        case 7: if (host.beep) host.beep(); break;
        default:
            if (c >= 0x20 && (c < 0x7F || c >= 0xA0)) put_char(k, c);
        }
    }
    cursor(k, 1);
    gfx_touch();
}

/* ---- input --------------------------------------------------------------------------- */

static void wake_reader(Con *k) {
    if (k->reader) sched_signal(k->reader, 1u << k->sigbit);
}

static void serve_pending(Con *k);

static void queue_input(Con *k, const char *s, int n) {
    if (k->inlen + n > (int)sizeof k->inq) n = (int)sizeof k->inq - k->inlen;
    if (n <= 0) return;
    memcpy(k->inq + k->inlen, s, (size_t)n);
    k->inlen += n;
    wake_reader(k);
    serve_pending(k);
}

/* ---- the keymap: US layout, refined by what the Mac keyboard really types ---------------- */

static const char us_plain[0x40] = "`1234567890-=\\\0" "0qwertyuiop[]\0" "123asdfghjkl;'\0\0" "456\0zxcvbnm,./\0" ".789";
static const char us_shift[0x40] = "~!@#$%^&*()_+|\0" "0QWERTYUIOP{}\0" "123ASDFGHJKL:\"\0\0" "456\0ZXCVBNM<>?\0" ".789";
static uint8_t learned[128][2];                                   /* [raw][shifted] */

void keymap_learn(int raw, int ch, int qual) {
    if (raw & 0x80 || raw >= 128 || !ch || (qual & 0xF8 & ~0x40)) return;   /* only plain or shifted */
    learned[raw][(qual & 3) != 0] = (uint8_t)ch;
}

/* one raw key to its characters; returns how many */
int keymap_convert(int raw, int qual, char *out, int max) {
    if (raw & 0x80 || max < 1) return 0;
    const char *seq = 0;
    switch (raw) {
    case 0x40: out[0] = ' '; return 1;
    case 0x41: out[0] = 8; return 1;
    case 0x42: if (qual & 3) seq = "\x9bZ"; else { out[0] = 9; return 1; } break;
    case 0x43: case 0x44: out[0] = 13; return 1;
    case 0x45: out[0] = 27; return 1;
    case 0x46: out[0] = 127; return 1;
    case 0x4C: seq = (qual & 3) ? "\x9bT" : "\x9b" "A"; break;
    case 0x4D: seq = (qual & 3) ? "\x9bS" : "\x9b" "B"; break;
    case 0x4E: seq = (qual & 3) ? "\x9b \x40" : "\x9b" "C"; break;
    case 0x4F: seq = (qual & 3) ? "\x9b A" : "\x9b" "D"; break;
    case 0x5F: seq = "\x9b?~"; break;
    case 0x4A: out[0] = '-'; return 1;
    case 0x5A: out[0] = '('; return 1;
    case 0x5B: out[0] = ')'; return 1;
    case 0x5C: out[0] = '/'; return 1;
    case 0x5D: out[0] = '*'; return 1;
    case 0x5E: out[0] = '+'; return 1;
    }
    if (raw >= 0x50 && raw <= 0x59) {                       /* F1-F10 */
        char b[8];
        int n = snprintf(b, sizeof b, "\x9b%d%s~", raw - 0x50 + ((qual & 3) ? 10 : 0), "");
        if (n > max) n = max;
        memcpy(out, b, (size_t)n);
        return n;
    }
    if (seq) {
        int n = (int)strlen(seq);
        if (n > max) n = max;
        memcpy(out, seq, (size_t)n);
        return n;
    }
    if (raw >= 0x40) return 0;
    int shift = (qual & 3) != 0;
    int c = learned[raw][shift];
    if (!c) c = (uint8_t)(shift ? us_shift[raw] : us_plain[raw]);
    if (!c) return 0;
    if ((qual & 4) && !shift && c >= 'a' && c <= 'z') c -= 32;    /* caps lock */
    if (qual & 8) c &= 0x1F;                                        /* control */
    out[0] = (char)c;
    return 1;
}

/* the raw key (and shift) that types a character, for injected keys */
int keymap_raw_for(int ch, int *qual) {
    *qual = 0;
    if (ch == ' ') return 0x40;
    if (ch == 13 || ch == 10) return 0x44;
    if (ch == 8) return 0x41;
    if (ch == 9) return 0x42;
    if (ch == 27) return 0x45;
    for (int s = 0; s < 2; s++)
        for (int r = 0; r < 0x40; r++) {
            int c = learned[r][s] ? learned[r][s] : (uint8_t)(s ? us_shift[r] : us_plain[r]);
            if (c == ch && !(r >= 0x0F && r <= 0x0F)) { if (s) *qual = 1; return r; }
        }
    return 0x40;
}

static void special_key(Con *k, int raw, int qual) {
    char b[8];
    int n = keymap_convert(raw, qual, b, sizeof b);
    if (n > 1 || (n == 1 && (uint8_t)b[0] == CSI)) queue_input(k, b, n);
}

/* called by the input task for keys typed in a console's window */
static void con_key(Con *k, int raw, int ch, int qual) {
    if (raw & 0x80) return;
    if (!ch) {                                               /* a key the host did not translate */
        char b[8];
        int n = keymap_convert(raw, qual, b, sizeof b);
        if (n == 1 && (uint8_t)b[0] != CSI) ch = (uint8_t)b[0];
        else { if (k->raw) special_key(k, raw, qual); return; }
    }
    if (k->raw) {
        char c = (char)ch;
        queue_input(k, &c, 1);
        return;
    }
    uint32_t brk = 0;
    switch (ch) {
    case 3: brk = SIGBREAKF_CTRL_C; break;
    case 4: brk = SIGBREAKF_CTRL_C << 1; break;
    case 5: brk = SIGBREAKF_CTRL_C << 2; break;
    case 6: brk = SIGBREAKF_CTRL_C << 3; break;
    }
    if (brk) { if (k->owner) sched_signal(k->owner, brk); if (ch == 3) con_write(k, "^C\n", 3); return; }
    if (ch == 0x1C) { k->eof = 1; wake_reader(k); return; }                 /* Ctrl-\: end of file */
    if (ch == 8) {
        if (k->linelen > 0) { k->linelen--; if (k->echo) con_write(k, "\b\x9bP", 3); }
        return;
    }
    if (ch == 0x18) {                                                       /* Ctrl-X: clear the line */
        while (k->linelen > 0) { k->linelen--; if (k->echo) con_write(k, "\b\x9bP", 3); }
        return;
    }
    if (ch == 13 || ch == 10) {
        if (k->echo) con_write(k, "\n", 1);
        k->line[k->linelen++] = '\n';
        queue_input(k, k->line, k->linelen);
        k->linelen = 0;
        return;
    }
    if (ch >= 0x20 && k->linelen < (int)sizeof k->line - 2) {
        k->line[k->linelen++] = (char)ch;
        char c = (char)ch;
        if (k->echo) con_write(k, &c, 1);
    }
}

static void win_hook(NWindow *w, int kind, int a, int b, int c) {
    Con *k = 0;
    for (Con *x = cons; x; x = x->next) if (x->win == w->win) k = x;
    if (!k) return;
    switch (kind) {
    case HOOK_KEY: con_key(k, a, b, c); break;
    case HOOK_CLOSE: k->closed = 1; if (k->own_window) { k->eof = 1; wake_reader(k); } break;
    case HOOK_NEWSIZE: con_resize(k); cursor(k, 1); break;
    }
}

int con_read(Con *k, char *dst, int n) {
    for (;;) {
        if (k->inlen) {
            int m = n < k->inlen ? n : k->inlen;
            if (!k->raw) {                                      /* cooked: one line per read */
                char *nl = memchr(k->inq, '\n', (size_t)m);
                if (nl) m = (int)(nl - k->inq) + 1;
            }
            memcpy(dst, k->inq, (size_t)m);
            memmove(k->inq, k->inq + m, (size_t)(k->inlen - m));
            k->inlen -= m;
            return m;
        }
        if (k->eof) { k->eof = 0; return 0; }
        NTask *me = sched_current();
        if (!me) return 0;
        int bit = (int)(int8_t)lib_call(&me->cpu, SysBase, "exec", "AllocSignal", 0xFFFFFFFFu);
        if (bit < 0) return 0;
        k->reader = me->task;
        k->sigbit = bit;
        exec_wait(1u << bit);
        k->reader = 0;
        lib_call(&me->cpu, SysBase, "exec", "FreeSignal", (uint32_t)bit);
    }
}

int con_ready(Con *k) { return k->inlen > 0 || k->eof; }
void con_set_raw(Con *k, int raw) { k->raw = raw; k->echo = !raw; }
int con_is_closed(Con *k) { return k->closed; }

/* ---- creating consoles ------------------------------------------------------------------ */

static Con *con_attach(uint32_t win, int own) {
    Con *k = calloc(1, sizeof *k);
    k->win = win;
    k->own_window = own;
    k->refs = 1;
    k->fg = 1;
    k->bg = 0;
    k->cursor_on = 1;
    k->echo = 1;
    /* its text: the system's default font (fixed width), as console.device does */
    uint32_t tf = gfx_default_font();
    rp_set_font(rd32(win + Window_RPort), tf);
    k->fw = rd16(tf + TextFont_tf_XSize);
    k->fh = rd16(tf + TextFont_tf_YSize);
    k->base = rd16(tf + TextFont_tf_Baseline);
    if (k->fw < 1) k->fw = 8;
    if (k->fh < 1) k->fh = 8;
    NWindow *w = nwin_of(win);
    if (w) w->hook = win_hook;
    k->next = cons;
    cons = k;
    con_resize(k);
    cursor(k, 1);
    return k;
}

static void con_detach(Con *k) {
    Con **pp = &cons;
    while (*pp && *pp != k) pp = &(*pp)->next;
    if (*pp) *pp = k->next;
    int other = 0;
    for (Con *x = cons; x; x = x->next) if (x->win == k->win) other = 1;
    NWindow *w = nwin_of(k->win);
    if (w && !other) w->hook = 0;
    free(k->ch);
    free(k->at);
    free(k);
}

/* "x/y/w/h/title/options" */
Con *con_open(M68K *c, const char *spec, int raw) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    char *f[16];
    int nf = 0;
    for (char *p = buf; nf < 16;) {
        f[nf++] = p;
        char *s = strchr(p, '/');
        if (!s) break;
        *s = 0;
        p = s + 1;
    }
    NScreen *s = workbench_screen(1);
    if (!s) return 0;
    int x = nf > 0 && f[0][0] ? atoi(f[0]) : 0, y = nf > 1 && f[1][0] ? atoi(f[1]) : rd8(s->scr + Screen_BarHeight) + 1;
    int w = nf > 2 && f[2][0] ? atoi(f[2]) : s->w, h = nf > 3 && f[3][0] ? atoi(f[3]) : 120;
    const char *title = nf > 4 ? f[4] : "";
    int close = 0, waitc = 0, nosize = 0, nodrag = 0, noborder = 0, backdrop = 0;
    for (int i = 5; i < nf; i++) {
        if (!strcasecmp(f[i], "CLOSE")) close = 1;
        else if (!strcasecmp(f[i], "WAIT")) waitc = 1;
        else if (!strcasecmp(f[i], "NOSIZE")) nosize = 1;
        else if (!strcasecmp(f[i], "NODRAG")) nodrag = 1;
        else if (!strcasecmp(f[i], "NOBORDER")) noborder = 1;
        else if (!strcasecmp(f[i], "BACKDROP")) backdrop = 1;
    }
    if (w < 80) w = 80;
    if (h < 30) h = 30;
    if (x + w > s->w) x = s->w - w;
    if (y + h > s->h) y = s->h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    uint32_t tags = exec_allocmem(8 * 20, MEMF_PUBLIC | MEMF_CLEAR), t = tags;
    uint32_t tstr = str_alloc(title);
#define TAG(a, b) do { wr32(t, (a)); wr32(t + 4, (b)); t += 8; } while (0)
    TAG(0x80000064, (uint32_t)x); TAG(0x80000065, (uint32_t)y); TAG(0x80000066, (uint32_t)w); TAG(0x80000067, (uint32_t)h);
    TAG(0x8000006E, tstr);                                       /* WA_Title */
    TAG(0x80000070, s->scr);                                     /* WA_CustomScreen */
    TAG(0x80000082, !nodrag && !noborder);                       /* WA_DragBar */
    TAG(0x80000083, !noborder);                                  /* WA_DepthGadget */
    TAG(0x80000084, close || waitc);                             /* WA_CloseGadget */
    TAG(0x80000081, !nosize && !noborder);                       /* WA_SizeGadget */
    TAG(0x80000085, backdrop);                                   /* WA_Backdrop */
    TAG(0x80000088, noborder);                                   /* WA_Borderless */
    TAG(0x80000089, 1);                                          /* WA_Activate */
    TAG(0x80000072, 120); TAG(0x80000073, 40);                   /* WA_MinWidth/Height */
    TAG(0x80000074, 0xFFFF); TAG(0x80000075, 0xFFFF);
#undef TAG
    uint32_t win = lib_call(c, IntuitionBase, "intuition", "OpenWindowTagList", 0, tags);
    exec_freemem(tags, 8 * 20);
    if (!win) return 0;
    Con *k = con_attach(win, 1);
    k->wait_close = waitc;
    con_set_raw(k, raw);
    k->owner = this_task();
    return k;
}

void con_ref(Con *k) { k->refs++; }

void con_close(M68K *c, Con *k) {
    if (--k->refs > 0) return;
    if (k->own_window && k->wait_close && !k->closed) {
        /* WAIT: stay until the user closes the window */
        NTask *me = sched_current();
        while (!k->closed && me) {
            int bit = (int)(int8_t)lib_call(c, SysBase, "exec", "AllocSignal", 0xFFFFFFFFu);
            if (bit < 0) break;
            k->reader = me->task;
            k->sigbit = bit;
            if (!k->closed) exec_wait(1u << bit);
            k->reader = 0;
            lib_call(c, SysBase, "exec", "FreeSignal", (uint32_t)bit);
            k->inlen = 0;
            k->eof = 0;
        }
    }
    uint32_t win = k->own_window ? k->win : 0;
    con_detach(k);
    if (win) lib_call(c, IntuitionBase, "intuition", "CloseWindow", win);
}

/* ---- console.device ------------------------------------------------------------------- */

#define IO_DEVICE 20
#define IO_UNIT 24
#define IO_COMMAND 28
#define IO_FLAGS 30
#define IO_ERROR 31
#define IO_ACTUAL 32
#define IO_LENGTH 36
#define IO_DATA 40
#define IOF_QUICK 1
#define CONUNIT_SIZE 296
#define CU_WINDOW 34
#define CU_XCP 38
#define CU_YCP 40
#define CU_XMAX 42
#define CU_YMAX 44
#define CU_XRSIZE 46
#define CU_YRSIZE 48
#define CU_XRORIGIN 50
#define CU_YRORIGIN 52

static uint32_t ConsoleBase;

static Con *con_of_unit(uint32_t unit) { for (Con *k = cons; k; k = k->next) if (k->unit && k->unit == unit) return k; return 0; }

static void sync_unit(Con *k) {
    if (!k->unit) return;
    int x0, y0;
    origin(k, &x0, &y0);
    wr16(k->unit + CU_XCP, (uint32_t)k->cx);
    wr16(k->unit + CU_YCP, (uint32_t)k->cy);
    wr16(k->unit + CU_XMAX, (uint32_t)(k->cols - 1));
    wr16(k->unit + CU_YMAX, (uint32_t)(k->rows - 1));
    wr16(k->unit + CU_XRSIZE, 8);
    wr16(k->unit + CU_YRSIZE, 8);
    wr16(k->unit + CU_XRORIGIN, (uint32_t)x0);
    wr16(k->unit + CU_YRORIGIN, (uint32_t)y0);
}

static void complete(uint32_t ior) { if (!(rd8(ior + IO_FLAGS) & IOF_QUICK)) exec_reply_msg(ior); }

static int read_into(Con *k, uint32_t ior) {
    if (!k->inlen) return 0;
    int len = (int)rd32(ior + IO_LENGTH), m = len < k->inlen ? len : k->inlen;
    uint32_t dst = rd32(ior + IO_DATA);
    for (int i = 0; i < m; i++) wr8(dst + (uint32_t)i, (uint8_t)k->inq[i]);
    memmove(k->inq, k->inq + m, (size_t)(k->inlen - m));
    k->inlen -= m;
    wr32(ior + IO_ACTUAL, (uint32_t)m);
    return 1;
}

static void serve_pending(Con *k) {
    while (k->npending && k->inlen) {
        uint32_t ior = k->pending[0];
        memmove(k->pending, k->pending + 1, sizeof(uint32_t) * (size_t)(--k->npending));
        read_into(k, ior);
        exec_reply_msg(ior);
    }
}

static uint32_t cd_Open(M68K *c) {
    uint32_t ior = c->a[1];
    int32_t unit = (int32_t)c->d[0];
    wr8(ior + IO_ERROR, 0);
    if (unit < 0) { wr32(ior + IO_UNIT, 0); return 0; }       /* CONU_LIBRARY */
    uint32_t win = rd32(ior + IO_DATA);
    if (!win || !nwin_of(win)) { wr8(ior + IO_ERROR, (uint8_t)-1); return 0; }
    Con *k = con_attach(win, 0);
    con_set_raw(k, 1);
    k->owner = this_task();
    k->unit = exec_allocmem(CONUNIT_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(k->unit + CU_WINDOW, win);
    sync_unit(k);
    wr32(ior + IO_UNIT, k->unit);
    (void)c;
    return 0;
}

static uint32_t cd_Close(M68K *c) {
    uint32_t unit = rd32(c->a[1] + IO_UNIT);
    Con *k = unit ? con_of_unit(unit) : 0;
    if (k) {
        for (int i = 0; i < k->npending; i++) { wr8(k->pending[i] + IO_ERROR, (uint8_t)-2); exec_reply_msg(k->pending[i]); }
        con_detach(k);
        exec_freemem(unit, CONUNIT_SIZE);
    }
    return 0;
}

static uint32_t cd_Expunge(M68K *c) { (void)c; return 0; }

static uint32_t cd_BeginIO(M68K *c) {
    uint32_t ior = c->a[1];
    Con *k = con_of_unit(rd32(ior + IO_UNIT));
    wr8(ior + IO_ERROR, 0);
    if (!k) { wr8(ior + IO_ERROR, (uint8_t)-1); complete(ior); return 0; }
    switch (rd16(ior + IO_COMMAND)) {
    case 3: {                                                 /* CMD_WRITE */
        int32_t len = (int32_t)rd32(ior + IO_LENGTH);
        uint32_t src = rd32(ior + IO_DATA);
        if (len < 0) { len = 0; while (rd8(src + (uint32_t)len)) len++; }
        char *p = len ? mem_ptr(src, (uint32_t)len) : 0;
        if (p) con_write(k, p, len);
        wr32(ior + IO_ACTUAL, (uint32_t)len);
        sync_unit(k);
        complete(ior);
        return 0;
    }
    case 2:                                                   /* CMD_READ */
        if (read_into(k, ior)) { complete(ior); return 0; }
        if (k->npending < 16) {
            wr8(ior + IO_FLAGS, rd8(ior + IO_FLAGS) & ~IOF_QUICK);
            k->pending[k->npending++] = ior;
            return 0;
        }
        wr8(ior + IO_ERROR, (uint8_t)-1);
        complete(ior);
        return 0;
    case 5: k->inlen = 0; complete(ior); return 0;            /* CMD_CLEAR */
    case 10: case 11: case 12: case 13: complete(ior); return 0;  /* CD_ASK/SET(DEFAULT)KEYMAP */
    default:
        wr8(ior + IO_ERROR, (uint8_t)-3);
        complete(ior);
        return 0;
    }
}

static uint32_t cd_AbortIO(M68K *c) {
    uint32_t ior = c->a[1];
    for (Con *k = cons; k; k = k->next)
        for (int i = 0; i < k->npending; i++)
            if (k->pending[i] == ior) {
                memmove(k->pending + i, k->pending + i + 1, sizeof(uint32_t) * (size_t)(k->npending - i - 1));
                k->npending--;
                wr8(ior + IO_ERROR, (uint8_t)-2);
                exec_reply_msg(ior);
                return 0;
            }
    return 0;
}

/* RawKeyConvert(events, buffer, length, keymap) */
static uint32_t cd_RawKeyConvert(M68K *c) {
    uint32_t ev = c->a[0], buf = c->a[1];
    int len = (int)c->d[1], total = 0;
    for (; ev && total < len; ev = rd32(ev)) {
        if (rd8(ev + InputEvent_ie_Class) != 1) continue;     /* IECLASS_RAWKEY */
        char b[16];
        int n = keymap_convert((int)rd16(ev + InputEvent_ie_Code), (int)rd16(ev + InputEvent_ie_Qualifier), b, sizeof b);
        for (int i = 0; i < n && total < len; i++) wr8(buf + (uint32_t)total++, (uint8_t)b[i]);
    }
    return (uint32_t)total;
}
static uint32_t cd_CDInputHandler(M68K *c) { return c->a[0]; }

static const NativeEntry console_natives[] = {
    { "Open", cd_Open }, { "Close", cd_Close }, { "Expunge", cd_Expunge },
    { "BeginIO", cd_BeginIO }, { "AbortIO", cd_AbortIO },
    { "RawKeyConvert", cd_RawKeyConvert }, { "CDInputHandler", cd_CDInputHandler },
    { 0, 0 }
};

/* ---- keymap.library ---------------------------------------------------------------------- */

static uint32_t KeymapBase, default_keymap;

static uint32_t km_MapRawKey(M68K *c) {
    uint32_t ev = c->a[0], buf = c->a[1];
    int len = (int)(int16_t)c->d[1];
    if (rd8(ev + InputEvent_ie_Class) != 1) return 0;
    char b[16];
    int n = keymap_convert((int)rd16(ev + InputEvent_ie_Code), (int)rd16(ev + InputEvent_ie_Qualifier), b, sizeof b);
    if (n > len) return 0xFFFFFFFFu;                             /* buffer overflow */
    for (int i = 0; i < n; i++) wr8(buf + (uint32_t)i, (uint8_t)b[i]);
    return (uint32_t)n;
}

/* MapANSI(string, count, buffer, length, keymap): code/qualifier pairs */
static uint32_t km_MapANSI(M68K *c) {
    uint32_t str = c->a[0], buf = c->a[1];
    int count = (int)c->d[0], len = (int)c->d[1], out = 0;
    for (int i = 0; i < count; i++) {
        int ch = rd8(str + (uint32_t)i), found = -1, q = 0;
        for (int r = 0; r < 0x40 && found < 0; r++)
            for (int s = 0; s < 2; s++) {
                int lc = learned[r][s] ? learned[r][s] : (uint8_t)(s ? us_shift[r] : us_plain[r]);
                if (lc == ch) { found = r; q = s ? 1 : 0; break; }
            }
        if (ch == ' ') { found = 0x40; q = 0; }
        if (ch == 13 || ch == 10) { found = 0x44; q = 0; }
        if (found < 0) return 0;
        if (out + 2 > 2 * len) return 0xFFFFFFFFu;
        wr8(buf + (uint32_t)out++, (uint32_t)found);
        wr8(buf + (uint32_t)out++, (uint32_t)q);
    }
    return (uint32_t)(out / 2);
}
static uint32_t km_SetKeyMapDefault(M68K *c) { (void)c; return 0; }
static uint32_t km_AskKeyMapDefault(M68K *c) { (void)c; return default_keymap; }

static const NativeEntry keymap_natives[] = {
    { "MapRawKey", km_MapRawKey }, { "MapANSI", km_MapANSI },
    { "SetKeyMapDefault", km_SetKeyMapDefault }, { "AskKeyMapDefault", km_AskKeyMapDefault },
    { 0, 0 }
};

void console_init(void) {
    ConsoleBase = lib_create("console.device", "console", LIB_SIZE, console_natives, 40, 1, "console 40.1 (AmigaMac)");
    wr8(ConsoleBase + LN_TYPE, NT_DEVICE);
    list_enqueue(SysBase + EB_DEVICELIST, ConsoleBase);
    KeymapBase = lib_create("keymap.library", "keymap", 64, keymap_natives, 40, 1, "keymap 40.1 (AmigaMac)");
    lib_add(KeymapBase);
    default_keymap = exec_allocmem(64, MEMF_PUBLIC | MEMF_CLEAR);   /* struct KeyMap stand-in */
}
