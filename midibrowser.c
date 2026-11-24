/* midibrowser - SPDX-License-Identifier: Zlib - see LICENSE */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

// Text: xbmfont (xbmfont/), bitmap fonts built into the program, so the browser
// looks the same on every X server. 6x13 looks like X's own "fixed" font.
#define XBMFONT_IMPLEMENTATION
#include "xbmfont.h"
#include "fonts/xbf_fixed_6x13.h"
#ifndef MIDIBROWSER_SMALL_FONTS
#include "fonts/xbf_fixed_10x20.h"   // sharper text on large screens (about 17 KB)
#endif

// One record = one line of the database file, stored once. The delimiters are
// replaced by '\0', so column k is found by skipping k-1 strings.
typedef struct {
    char *line;
    int nfields;
} Entry;

static Entry *database = NULL;
static int db_count = 0;
static int db_capacity = 0;
static int db_max_fields = 0;

static int *filtered_indices = NULL;
static int filtered_count = 0;

static char search_query[512] = "";
static int selected_index = 0;
static int scroll_offset = 0;

static Display *dpy;
static Window win;
static GC gc;
static Pixmap pixmap = 0;
static xbf_ctx *txt = NULL;     // text font, uploaded once to the X server

// PID (and process group id) of the running player, 0 if none.
// Written from the SIGCHLD handler, hence volatile.
static volatile pid_t current_player_pid = 0;

// ---------------------------------------------------------------------------
// Command line options (see usage())
// ---------------------------------------------------------------------------
#define MAX_COLS 32
static const char *opt_file      = "midi_database_kompakt.txt";   // -f
static char        opt_delim     = '\t';                          // -d
static int         opt_search[MAX_COLS] = { 2 };                   // -s
static int         opt_nsearch   = 1;
static const char *opt_show      = "[{1:1}/{1}] {2}";              // -c
static const char *opt_arg       = NULL;                           // -p (NULL = MIDI md5 layout)
static const char *opt_cmd       = NULL;                           // -x (NULL = playmidi.sh)
static const char *opt_title     = "MIDI DB Browser";              // -T
static const char *opt_action    = "Play";                         // -a
static int         opt_keep      = 0;                              // -k
static char        show_buf[MAX_COLS * 8];   // template built from "-c 2,1"

// configuration and style
static int win_width = 800;
static int win_height = 400;

// colors
static unsigned long col_top_bg      = 0x2D3748;
static unsigned long col_top_fg      = 0xFFFFFF;
static unsigned long col_mid_bg      = 0x1A202C;
static unsigned long col_mid_fg      = 0xE2E8F0;
static unsigned long col_selected_bg = 0x3182CE;
static unsigned long col_selected_fg = 0xFFFFFF;
static unsigned long col_bot_bg      = 0x2D3748;
static unsigned long col_bot_fg      = 0xA0AEC0;

// paddings (for the base size; they grow with the text on large screens)
static int padding_top_bottom_bars = 4;  // padding over/under text in header and footer
static int padding_rows            = 2;  // padding over/under text in db-view section
static int padding_left            = 15; // left/right text margin

// Text sizes. The size follows the window (the largest whose scaled 800x600
// layout fits), unless chosen with Ctrl + '+' / '-'; Ctrl + 0 goes back to auto.
typedef struct { const xbf_font *font; int scale; } text_size;
static const text_size sizes[] = {
#ifndef MIDIBROWSER_SMALL_FONTS
    { &xbf_fixed_6x13, 1 }, { &xbf_fixed_10x20, 1 }, { &xbf_fixed_6x13, 2 },
    { &xbf_fixed_10x20, 2 }, { &xbf_fixed_10x20, 3 }, { &xbf_fixed_10x20, 4 },
#else
    { &xbf_fixed_6x13, 1 }, { &xbf_fixed_6x13, 2 }, { &xbf_fixed_6x13, 3 },
    { &xbf_fixed_6x13, 4 }, { &xbf_fixed_6x13, 6 },
#endif
};
#define N_SIZES ((int)(sizeof sizes / sizeof sizes[0]))
static int size_now = -1;       // index in sizes
static int size_manual = -1;    // chosen with Ctrl +/-, or -1 = follow the window

// Dynamic layout sizes (calculated from font + padding)
static int font_ascent = 0;
static int row_height = 0;
static int header_height = 0;
static int footer_height = 0;
static int pad_bars = 0, pad_rows = 0, pad_left = 0;   // paddings at the current size

static void set_window_icon(Display *mydpy, Window mywin) {
    // _NET_WM_ICON is an array of CARDINAL, which Xlib passes as 'long' on all platforms.
    static const unsigned long icon_data[] = {
        8, 8, // width, height
        0xFF222222, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF222222, 0xFF222222, 0xFF222222, 0xFF222222,
        0xFFFFFFFF, 0xFF222222, 0xFF222222, 0xFF222222, 0xFFFFFFFF, 0xFF222222, 0xFF222222, 0xFF222222,
        0xFFFFFFFF, 0xFF222222, 0xFF222222, 0xFF222222, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF222222, 0xFF222222,
        0xFFFFFFFF, 0xFF222222, 0xFF222222, 0xFF222222, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF222222,
        0xFF222222, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFF222222, 0xFF222222, 0xFFFFFFFF, 0xFF222222, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF222222,
        0xFF222222, 0xFF222222, 0xFF222222, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF222222, 0xFF222222,
        0xFF222222, 0xFF222222, 0xFF222222, 0xFF222222, 0xFFFFFFFF, 0xFF222222, 0xFF222222, 0xFF222222
    };
    int data_length = sizeof(icon_data) / sizeof(icon_data[0]);

    Atom wm_icon = XInternAtom(mydpy, "_NET_WM_ICON", False);
    Atom cardinal = XInternAtom(mydpy, "CARDINAL", False);
    XChangeProperty(mydpy, mywin, wm_icon, cardinal, 32, PropModeReplace,
                    (const unsigned char *)icon_data, data_length);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        perror("Out of memory");
        exit(1);
    }
    return q;
}

// ---------------------------------------------------------------------------
// UTF-8 helpers (no locale or extra libraries needed)
// ---------------------------------------------------------------------------

// Length of the valid UTF-8 sequence at s, or 0 if it is not valid UTF-8.
static int utf8_seq_len(const unsigned char *s) {
    unsigned char c = s[0];
    int n, i;
    if (c < 0x80) return 1;
    else if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    else return 0;
    for (i = 1; i < n; i++)
        if ((s[i] & 0xC0) != 0x80) return 0;
    return n;
}

// Decode one character and advance *p. Bytes that are not valid UTF-8 are
// taken as Latin-1, so nothing is ever skipped.
static unsigned int utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    int n = utf8_seq_len(s);
    unsigned int cp;
    if (n <= 1) { *p = s + 1; return s[0]; }
    if (n == 2)      cp = ((s[0] & 0x1Fu) << 6)  |  (s[1] & 0x3Fu);
    else if (n == 3) cp = ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6)  |  (s[2] & 0x3Fu);
    else             cp = ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
    *p = s + n;
    return cp;
}

static int utf8_encode(unsigned int cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));  out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));         out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Returns a malloc'ed UTF-8 copy of s. Lines that are not valid UTF-8 are
// assumed to be Latin-1 (old DOS/Windows file names) and converted.
static char *dup_as_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    int valid = 1;
    while (*p) {
        int n = utf8_seq_len(p);
        if (!n) { valid = 0; break; }
        p += n;
    }
    if (valid) return strdup(s);

    char *out = malloc(strlen(s) * 2 + 1), *o = out;
    if (!out) return NULL;
    for (p = (const unsigned char *)s; *p; p++) o += utf8_encode(*p, o);
    *o = '\0';
    return out;
}

// Case folding for search: ASCII A-Z and the Latin-1 letters U+00C0-U+00DE
// (ÀÁÂ...ÆØÅ... except U+00D7) map to lower case. Works byte by byte on UTF-8:
// those letters are 0xC3 followed by 0x80-0x9E, lower case is +0x20.
static unsigned char fold_byte(unsigned char prev, unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (prev == 0xC3 && c >= 0x80 && c <= 0x9E && c != 0x97) return c + 0x20;
    return c;
}

static void fold_string(char *s) {
    unsigned char prev = 0;
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        *p = fold_byte(prev, c);
        prev = c;
    }
}

static int is_ascii(const char *s) {
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

// Case-insensitive substring search; 'needle' must already be folded.
// An ASCII needle can only match ASCII bytes, so the (fast) C library
// strcasestr gives the same result; the loop below handles ÆØÅ etc.
static int contains_folded(const char *hay, const char *needle, int needle_ascii) {
    const unsigned char *h = (const unsigned char *)hay;
    const unsigned char *n = (const unsigned char *)needle;
    if (!n[0]) return 1;
    if (needle_ascii) return strcasestr(hay, needle) != NULL;
    int jump = !(n[0] >= 'a' && n[0] <= 'z');       // first byte has no case variant
    for (; *h; h++) {
        if (jump) {
            h = (const unsigned char *)strchr((const char *)h, n[0]);
            if (!h) return 0;
        }
        if ((*h & 0xC0) == 0x80) continue;          // only start at character boundaries
        if (fold_byte(0, *h) != n[0]) continue;      // lead bytes never depend on prev
        int i = 1;
        while (n[i] && h[i] && fold_byte(h[i - 1], h[i]) == n[i]) i++;
        if (!n[i]) return 1;
    }
    return 0;
}

// Column 'col' (1-based) of an entry; "" if the line has fewer columns.
static const char *field(const Entry *e, int col) {
    if (col < 1 || col > e->nfields) return "";
    const char *f = e->line;
    while (--col > 0) f += strlen(f) + 1;
    return f;
}

// Expand a template such as "[{1:1}/{1}] {2}" for one entry.
// {N} is column N, {N:K} its first K characters. Anything else is copied as is.
// The result lives in a buffer owned by the caller (*buf, *cap), reused between calls.
static const char *expand(const char *tpl, const Entry *e, char **buf, size_t *cap) {
    size_t len = 0;
    const char *t = tpl;
    while (*t) {
        const char *src = t;
        size_t n = 1;
        if (*t == '{' && t[1] >= '1' && t[1] <= '9') {
            char *end;
            long col = strtol(t + 1, &end, 10);
            long chars = -1;
            if (*end == ':') chars = strtol(end + 1, &end, 10);
            if (*end == '}' && chars != 0) {
                src = field(e, (int)col);
                n = strlen(src);
                if (chars > 0) {    // first 'chars' UTF-8 characters
                    const unsigned char *q = (const unsigned char *)src;
                    while (*q && chars-- > 0) utf8_next(&q);
                    n = (size_t)((const char *)q - src);
                }
                t = end + 1;
            } else t++;
        } else t++;

        if (len + n + 1 > *cap) {
            *cap = (len + n + 1) * 2;
            *buf = xrealloc(*buf, *cap);
        }
        memcpy(*buf + len, src, n);
        len += n;
    }
    if (!*buf) *buf = xrealloc(NULL, *cap = 64);
    (*buf)[len] = '\0';
    return *buf;
}

static void load_database(void) {
    FILE *fp = strcmp(opt_file, "-") ? fopen(opt_file, "r") : stdin;
    if (!fp) {
        fprintf(stderr, "Could not open database \"%s\": %s\n", opt_file, strerror(errno));
        exit(1);
    }

    // getline() handles lines of any length (no silent splitting of long lines).
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;   // skip empty lines

        if (db_count == db_capacity) {
            db_capacity = db_capacity ? db_capacity * 2 : 4096;
            database = xrealloc(database, (size_t)db_capacity * sizeof(*database));
        }
        Entry *e = &database[db_count];
        e->line = dup_as_utf8(line);
        if (!e->line) {
            perror("Out of memory");
            exit(1);
        }
        // Split into columns in place.
        e->nfields = 1;
        for (char *c = e->line; *c; c++) {
            if (*c == opt_delim) { *c = '\0'; e->nfields++; }
        }
        if (e->nfields > db_max_fields) db_max_fields = e->nfields;
        db_count++;
    }
    free(line);
    if (fp != stdin) fclose(fp);

    filtered_indices = xrealloc(NULL, (size_t)(db_count ? db_count : 1) * sizeof(int));
    printf("Read %d records in memory.\n", db_count);
}

// ---------------------------------------------------------------------------
// Layout / scrolling helpers
// ---------------------------------------------------------------------------

static int visible_rows(void) {
    int mid_zone_height = win_height - header_height - footer_height;
    int rows = row_height > 0 ? mid_zone_height / row_height : 0;
    return rows > 0 ? rows : 1;
}

// Clamp selection and scroll position into valid ranges.
static void clamp_scroll(void) {
    int rows = visible_rows();
    int max_scroll = filtered_count - rows;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    if (scroll_offset < 0) scroll_offset = 0;

    if (selected_index >= filtered_count) selected_index = filtered_count - 1;
    if (selected_index < 0) selected_index = 0;
}

// Scroll just enough to make the selected row visible.
static void ensure_visible(void) {
    int rows = visible_rows();
    if (selected_index < scroll_offset) scroll_offset = selected_index;
    if (selected_index >= scroll_offset + rows) scroll_offset = selected_index - rows + 1;
    clamp_scroll();
}

// narrow = 1 when the query was only extended: then only the current results
// can match, so search those instead of the whole database.
static void update_filter(int narrow) {
    char needle[sizeof(search_query)];
    snprintf(needle, sizeof(needle), "%s", search_query);
    fold_string(needle);
    int ascii = is_ascii(needle);

    int count = narrow ? filtered_count : db_count;
    int n = 0;
    for (int k = 0; k < count; k++) {
        int i = narrow ? filtered_indices[k] : k;
        for (int c = 0; c < opt_nsearch; c++) {
            if (contains_folded(field(&database[i], opt_search[c]), needle, ascii)) {
                filtered_indices[n++] = i;
                break;
            }
        }
    }
    filtered_count = n;
    // New result list: start at the top so the highlight is never off screen.
    selected_index = 0;
    scroll_offset = 0;
}

// ---------------------------------------------------------------------------
// Player process handling
// ---------------------------------------------------------------------------

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    pid_t p;
    while ((p = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (p == current_player_pid) current_player_pid = 0;
    }
    errno = saved_errno;
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

static void stop_player(void) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &old);

    pid_t pid = current_player_pid;
    if (pid > 0) {
        // Negative pid = whole process group: also stops players started by a
        // script that does not 'exec' its player.
        kill(-pid, SIGTERM);
        current_player_pid = 0;
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
    // The child is reaped by the SIGCHLD handler, so no zombies are left behind.
}

// Run the configured command with 'arg' as its only argument.
static void run_command(const char *arg) {
    // Block SIGCHLD until current_player_pid is set, so a player that exits
    // immediately is still recognised by the handler.
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &old);

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);                        // own process group, see stop_player()
        sigprocmask(SIG_SETMASK, &old, NULL); // signal mask survives exec: restore it
        if (!opt_cmd) {
            // Default: playmidi.sh in the current directory, otherwise from $PATH.
            const char *cmd = access("./playmidi.sh", X_OK) == 0 ? "./playmidi.sh" : "playmidi.sh";
            execlp(cmd, cmd, arg, (char *)NULL);
            fprintf(stderr, "Could not run %s: %s\n", cmd, strerror(errno));
        } else if (strpbrk(opt_cmd, " \t|;&<>()$`'\"")) {
            // A command line with options, pipes etc.: let the shell run it.
            // The value is passed as "$1", so it is never parsed by the shell.
            size_t len = strlen(opt_cmd) + 8;
            char *script = malloc(len);
            if (script) {
                snprintf(script, len, "%s \"$1\"", opt_cmd);
                execl("/bin/sh", "sh", "-c", script, "sh", arg, (char *)NULL);
            }
            fprintf(stderr, "Could not run /bin/sh: %s\n", strerror(errno));
        } else {
            execlp(opt_cmd, opt_cmd, arg, (char *)NULL);
            fprintf(stderr, "Could not run %s: %s\n", opt_cmd, strerror(errno));
        }
        _exit(127);                           // _exit: don't flush the parent's stdio buffers
    } else if (pid > 0) {
        setpgid(pid, pid);                    // also in the parent, avoids a race with kill()
        if (!opt_keep) current_player_pid = pid;
    } else {
        perror("fork");
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
}

static void activate(int index) {
    static char *buf = NULL;
    static size_t cap = 0;
    if (index < 0 || index >= filtered_count) return;

    if (!opt_keep) stop_player();

    const Entry *e = &database[filtered_indices[index]];
    const char *arg;

    if (opt_arg) {
        arg = expand(opt_arg, e, &buf, &cap);
    } else {
        // Default MIDI layout: <first char>/<key>.mid, .midi or no extension.
        arg = expand("{1:1}/{1}.mid", e, &buf, &cap);
        if (access(arg, F_OK) == -1) {
            arg = expand("{1:1}/{1}.midi", e, &buf, &cap);
            if (access(arg, F_OK) == -1) arg = expand("{1:1}/{1}", e, &buf, &cap);
        }
    }

    printf("%s: %s\n", opt_action, arg);
    fflush(stdout);
    run_command(arg);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void check_pixmap(void) {
    if (pixmap) {
        XFreePixmap(dpy, pixmap);
    }
    pixmap = XCreatePixmap(dpy, win, win_width, win_height, DefaultDepth(dpy, DefaultScreen(dpy)));
}

// Draw UTF-8 text at (x, y) in colour `fg`, ending in an ellipsis if it is
// wider than max_width pixels. Characters the font lacks are drawn as '?'.
static void draw_text_fit(Drawable d, int x, int y, const char *text, int max_width,
                          unsigned long fg) {
    xbf_draw_fit(txt, d, x, y, fg, text, max_width, XBF_CUT_END);
}

// A base-size measure at the current text size (13 px = base)
static int scaled(int v) {
    int line = sizes[size_now].font->height * sizes[size_now].scale;
    return (v * line + 6) / 13;
}

// Use text size i: upload its font and recalculate the layout. False if X
// could not take the font (the current size is kept).
static int set_size(int i) {
    if (txt && i == size_now) return 1;
    xbf_ctx *t = xbf_create(dpy, win, sizes[i].font, sizes[i].scale);
    if (!t) return txt != NULL;
    xbf_free(txt);
    txt = t;
    size_now = i;

    pad_bars = scaled(padding_top_bottom_bars);
    pad_rows = scaled(padding_rows);
    pad_left = scaled(padding_left);
    font_ascent   = xbf_ascent(txt);
    row_height    = xbf_line_height(txt) + (pad_rows * 2);
    header_height = xbf_line_height(txt) + (pad_bars * 2);
    footer_height = xbf_line_height(txt) + (pad_bars * 2);
    return 1;
}

// The size to use now: the chosen one, or the largest that fits the window
static int wanted_size(void) {
    if (size_manual >= 0) return size_manual;
    int best = 0;
    for (int i = 1; i < N_SIZES; i++) {
        int line = sizes[i].font->height * sizes[i].scale;
        if (line * 800 <= win_width * 13 && line * 600 <= win_height * 13) best = i;
    }
    return best;
}

static void draw_ui(void) {
    static char *row_buf = NULL;
    static size_t row_cap = 0;
    if (!pixmap) return;

    int text_width = win_width - 2 * pad_left;

    // 1. Clear pixmap with the list background colour
    XSetForeground(dpy, gc, col_mid_bg);
    XFillRectangle(dpy, pixmap, gc, 0, 0, win_width, win_height);

    // Middle section text/rows
    int rows = visible_rows();
    for (int i = 0; i < rows && (i + scroll_offset) < filtered_count; i++) {
        int current_idx = i + scroll_offset;
        int y_pos = header_height + (i * row_height);
        if (y_pos + row_height > win_height - footer_height) break;

        if (current_idx == selected_index) {
            XSetForeground(dpy, gc, col_selected_bg);
            XFillRectangle(dpy, pixmap, gc, 0, y_pos, win_width, row_height);
        }

        const Entry *e = &database[filtered_indices[current_idx]];
        draw_text_fit(pixmap, pad_left, y_pos + pad_rows + font_ascent,
                      expand(opt_show, e, &row_buf, &row_cap), text_width,
                      current_idx == selected_index ? col_selected_fg : col_mid_fg);
    }

    // 2. Search bar (top)
    XSetForeground(dpy, gc, col_top_bg);
    XFillRectangle(dpy, pixmap, gc, 0, 0, win_width, header_height);
    char search_label[600];
    snprintf(search_label, sizeof(search_label), "Search: %s_", search_query);
    draw_text_fit(pixmap, pad_left, pad_bars + font_ascent, search_label, text_width, col_top_fg);

    // 3. Status bar (bottom)
    int footer_y = win_height - footer_height;
    XSetForeground(dpy, gc, col_bot_bg);
    XFillRectangle(dpy, pixmap, gc, 0, footer_y, win_width, footer_height);
    char status_text[256];
    snprintf(status_text, sizeof(status_text),
             opt_keep ? "Found: %d/%d  |  [Enter/Click]: %s  |  [Ctrl +/-]: Size  |  [ESC]: Quit"
                      : "Found: %d/%d  |  [Enter/Click]: %s  |  [Ctrl]: Stop  |  [Ctrl +/-]: Size  |  [ESC]: Quit",
             filtered_count, db_count, opt_action);
    draw_text_fit(pixmap, pad_left, footer_y + pad_bars + font_ascent, status_text, text_width, col_bot_fg);

    // 4. Copy pixmap to the window
    XCopyArea(dpy, pixmap, win, gc, 0, 0, win_width, win_height, 0, 0);
    XFlush(dpy);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void usage(const char *prog) {
    printf(
"Usage: %s [options]\n"
"\n"
"  -f FILE      database file, '-' for standard input (default: midi_database_kompakt.txt)\n"
"  -d CHAR      column delimiter, 'tab' or a single character (default: tab)\n"
"  -s COLS      column(s) to search, e.g. 2 or 1,3 (default: 2)\n"
"  -c COLS      column(s) to show, e.g. 2,1, or a template (default: '[{1:1}/{1}] {2}')\n"
"  -p TEMPLATE  argument for the command, e.g. '{1}' or 'data/{2}.wav'\n"
"               (default: MIDI layout {1:1}/{1}.mid, also .midi or no extension)\n"
"  -x COMMAND   command to run; may include options, e.g. 'aplay -q'\n"
"               (default: ./playmidi.sh, or playmidi.sh from $PATH)\n"
"  -T TITLE     window title (default: MIDI DB Browser)\n"
"  -a LABEL     action shown in the status bar and log (default: Play)\n"
"  -k           keep programs running: don't stop the previous one, no Ctrl stop\n"
"  -h           show this help\n"
"\n"
"Templates: {N} is column N, {N:K} its first K characters.\n"
"A file with only one column is shown, searched and passed as is.\n", prog);
}

// Parse "2" or "1,3" into cols[]; returns the count, 0 on error.
static int parse_cols(const char *s, int *cols) {
    int n = 0;
    while (*s) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s || v < 1 || v > 999 || n == MAX_COLS) return 0;
        cols[n++] = (int)v;
        if (*end == ',') end++;
        else if (*end) return 0;
        s = end;
    }
    return n;
}

int main(int argc, char **argv) {
    int opt, given_s = 0, given_c = 0;
    while ((opt = getopt(argc, argv, "f:d:s:c:p:x:T:a:kh")) != -1) {
        switch (opt) {
        case 'f': opt_file = optarg; break;
        case 'd':
            if (!strcmp(optarg, "tab") || !strcmp(optarg, "\\t")) opt_delim = '\t';
            else if (strlen(optarg) == 1) opt_delim = optarg[0];
            else { fprintf(stderr, "-d: give a single character or 'tab'\n"); return 2; }
            break;
        case 's':
            opt_nsearch = parse_cols(optarg, opt_search);
            if (!opt_nsearch) { fprintf(stderr, "-s: give column numbers like 2 or 1,3\n"); return 2; }
            given_s = 1;
            break;
        case 'c':
            if (strchr(optarg, '{')) {
                opt_show = optarg;
            } else {
                int cols[MAX_COLS], n = parse_cols(optarg, cols);
                if (!n) { fprintf(stderr, "-c: give column numbers like 2,1 or a template\n"); return 2; }
                show_buf[0] = '\0';
                for (int i = 0; i < n; i++) {
                    size_t l = strlen(show_buf);
                    snprintf(show_buf + l, sizeof(show_buf) - l, i ? "  {%d}" : "{%d}", cols[i]);
                }
                opt_show = show_buf;
            }
            given_c = 1;
            break;
        case 'p': opt_arg = optarg; break;
        case 'x': opt_cmd = optarg; break;
        case 'T': opt_title = optarg; break;
        case 'a': opt_action = optarg; break;
        case 'k': opt_keep = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc) { usage(argv[0]); return 2; }

    load_database();

    // A plain list (one column): show, search and pass the line itself,
    // unless the options say otherwise.
    if (db_max_fields == 1) {
        if (!given_s) { opt_search[0] = 1; opt_nsearch = 1; }
        if (!given_c) opt_show = "{1}";
        if (!opt_arg) opt_arg = "{1}";
    }

    update_filter(0);
    setup_signals();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Could not open X display\n");
        return 1;
    }
    // Don't leak the X connection into the player process.
    fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);

    int screen = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 10, 10, win_width, win_height, 1,
                              BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    XSetWindowBackgroundPixmap(dpy, win, None);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | StructureNotifyMask);
    set_window_icon(dpy, win);

    // Handle the window manager's close button (X) ourselves
    Atom wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    // Title: XStoreName for old window managers, _NET_WM_NAME (UTF-8) for current ones.
    XStoreName(dpy, win, opt_title);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", False),
                    XInternAtom(dpy, "UTF8_STRING", False), 8, PropModeReplace,
                    (const unsigned char *)opt_title, (int)strlen(opt_title));
    XClassHint class_hint = { (char *)"midibrowser", (char *)"Midibrowser" };
    XSetClassHint(dpy, win, &class_hint);
    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);

    // Upload the text font for the window size and calculate the layout.
    if (!set_size(wanted_size())) {
        fprintf(stderr, "Could not upload the font to the X server.\n");
        return 1;
    }
    int ctrl_alone = 0;   // Ctrl is down and no other key was pressed with it

    check_pixmap();

    int running = 1;
    XEvent event;

    while (running) {
        XNextEvent(dpy, &event);

        switch (event.type) {
        case ClientMessage:
            // Close via the window's "X" button
            if ((Atom)event.xclient.data.l[0] == wm_delete_window) running = 0;
            break;

        case ConfigureNotify:
            if (event.xconfigure.width != win_width || event.xconfigure.height != win_height) {
                win_width = event.xconfigure.width;
                win_height = event.xconfigure.height;
                set_size(wanted_size());
                check_pixmap();
                ensure_visible();
                draw_ui();
            }
            break;

        case MappingNotify:
            // Keyboard layout changed (e.g. setxkbmap): refresh Xlib's cached keymap.
            XRefreshKeyboardMapping(&event.xmapping);
            break;

        case Expose:
            if (event.xexpose.count == 0) draw_ui();
            break;

        case KeyPress: {
            KeySym keysym = XLookupKeysym(&event.xkey, 0);
            int len = (int)strlen(search_query);

            if (keysym == XK_Control_L || keysym == XK_Control_R) {
                ctrl_alone = 1;    // stops the player when released (below)
                break;
            }
            ctrl_alone = 0;        // Ctrl + key: not the stop key
            if (event.xkey.state & ControlMask) {
                // Ctrl + '+' / '-': bigger / smaller text; Ctrl + 0: follow the window
                int s = -2;
                if (keysym == XK_plus || keysym == XK_equal || keysym == XK_KP_Add)
                    s = size_now < N_SIZES - 1 ? size_now + 1 : size_now;
                else if (keysym == XK_minus || keysym == XK_KP_Subtract)
                    s = size_now > 0 ? size_now - 1 : 0;
                else if (keysym == XK_0 || keysym == XK_KP_0 || keysym == XK_KP_Insert)
                    s = -1;
                if (s != -2) {
                    size_manual = s;
                    set_size(wanted_size());
                    ensure_visible();
                    draw_ui();
                    break;
                }
            }
            if (keysym == XK_Escape) {
                running = 0;
            }
            else if (keysym == XK_Up) {
                if (selected_index > 0) selected_index--;
                ensure_visible();
                draw_ui();
            }
            else if (keysym == XK_Down) {
                if (selected_index < filtered_count - 1) selected_index++;
                ensure_visible();
                draw_ui();
            }
            else if (keysym == XK_Return || keysym == XK_KP_Enter) {
                ensure_visible();
                draw_ui();
                activate(selected_index);
            }
            else if (keysym == XK_BackSpace) {
                if (len > 0) {
                    // Remove one whole UTF-8 character (continuation bytes first).
                    while (len > 0 && ((unsigned char)search_query[len - 1] & 0xC0) == 0x80) len--;
                    if (len > 0) len--;
                    search_query[len] = '\0';
                    update_filter(0);
                    draw_ui();
                }
            }
            else {
                // Map the key (with Shift/AltGr applied) to a Unicode character.
                // Latin-1 keysyms equal their code point, and other Unicode
                // keysyms are 0x01000000 + code point. No locale or input method needed.
                char buf[8], utf8[4];
                KeySym ks = NoSymbol;
                int num = XLookupString(&event.xkey, buf, sizeof(buf) - 1, &ks, NULL);
                unsigned int cp = 0;
                if ((ks >= 0x20 && ks <= 0x7E) || (ks >= 0xA0 && ks <= 0xFF)) cp = (unsigned int)ks;
                else if ((ks & 0xFF000000) == 0x01000000) cp = (unsigned int)(ks & 0x00FFFFFF);
                else if (num == 1 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] <= 126) cp = (unsigned char)buf[0]; // e.g. keypad digits

                if (event.xkey.state & ControlMask) cp = 0;   // Ctrl+key is not text

                if (cp >= 0x20 && !(cp >= 0x7F && cp < 0xA0) && cp <= 0x10FFFF) {
                    int n = utf8_encode(cp, utf8);
                    if (len + n < (int)sizeof(search_query)) {
                        memcpy(search_query + len, utf8, (size_t)n);
                        search_query[len + n] = '\0';
                        update_filter(1);
                        draw_ui();
                    }
                }
            }
            break;
        }

        case KeyRelease:
            // Ctrl pressed and released on its own stops the player
            if (ctrl_alone && (XLookupKeysym(&event.xkey, 0) == XK_Control_L ||
                               XLookupKeysym(&event.xkey, 0) == XK_Control_R)) {
                ctrl_alone = 0;
                if (!opt_keep && current_player_pid) {
                    stop_player();
                    printf("Stopped via CTRL.\n");
                }
            }
            break;

        case ButtonPress: {
            int mouse_y = event.xbutton.y;
            ctrl_alone = 0;
            int rows = visible_rows();
            if (event.xbutton.button == 1) {
                // Click within the drawn rows of the central area
                if (mouse_y >= header_height && mouse_y < (win_height - footer_height)) {
                    int clicked_row = (mouse_y - header_height) / row_height;
                    int target_idx = clicked_row + scroll_offset;
                    if (clicked_row < rows && target_idx < filtered_count) {
                        selected_index = target_idx;
                        draw_ui();
                        activate(selected_index);
                    }
                }
            }
            else if (event.xbutton.button == 4) {
                if (scroll_offset > 0) {
                    scroll_offset--;
                    draw_ui();
                }
            }
            else if (event.xbutton.button == 5) {
                if (scroll_offset + rows < filtered_count) {
                    scroll_offset++;
                    draw_ui();
                }
            }
            break;
        }
        }
    }

    if (!opt_keep) stop_player();
    if (pixmap) XFreePixmap(dpy, pixmap);
    xbf_free(txt);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    for (int i = 0; i < db_count; i++) free(database[i].line);
    free(database);
    free(filtered_indices);
    return 0;
}
