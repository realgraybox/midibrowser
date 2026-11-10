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

typedef struct {
    char md5[33];
    char *paths;            // heap-allocated, exact length
} MidiEntry;

static MidiEntry *database = NULL;
static int db_count = 0;
static int db_capacity = 0;

static int *filtered_indices = NULL;
static int filtered_count = 0;

static char search_query[512] = "";
static int selected_index = 0;
static int scroll_offset = 0;

static Display *dpy;
static Window win;
static GC gc;
static Pixmap pixmap = 0;
static XFontStruct *font_info = NULL;
static int font_loaded = 0;     // 1 = XLoadQueryFont (XFreeFont), 0 = XQueryFont (XFreeFontInfo)

// PID (and process group id) of the running player, 0 if none.
// Written from the SIGCHLD handler, hence volatile.
static volatile pid_t current_player_pid = 0;

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

// paddings
static int padding_top_bottom_bars = 4;  // padding over/under text in header and footer
static int padding_rows            = 2;  // padding over/under text in db-view section
static int padding_left            = 15; // left/right text margin

// Fonts are tried in order. The first is the Unicode (iso10646) version of the
// classic "fixed" font from xfonts-base; "fixed" (Latin-1) is the fallback.
// For a larger font try "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso10646-1" (9x15).
static const char *font_names[] = {
    "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1",
    "fixed",
};

// Dynamic layout sizes (calculated from font + padding)
static int font_height = 0;
static int font_ascent = 0;
static int row_height = 0;
static int header_height = 0;
static int footer_height = 0;

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

static void load_database(void) {
    FILE *fp = fopen("midi_database_kompakt.txt", "r");
    if (!fp) {
        perror("Could not open database \"midi_database_kompakt.txt\"");
        exit(1);
    }

    // getline() handles lines of any length (no silent splitting of long lines).
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        char *tab = strchr(line, '\t');
        if (!tab || tab == line) continue;   // need a non-empty key before the tab
        *tab = '\0';

        if (db_count == db_capacity) {
            db_capacity = db_capacity ? db_capacity * 2 : 4096;
            database = xrealloc(database, (size_t)db_capacity * sizeof(*database));
        }
        MidiEntry *e = &database[db_count];
        snprintf(e->md5, sizeof(e->md5), "%s", line);
        e->paths = dup_as_utf8(tab + 1);
        if (!e->paths) {
            perror("Out of memory");
            exit(1);
        }
        db_count++;
    }
    free(line);
    fclose(fp);

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

    if (narrow) {
        int n = 0;
        for (int k = 0; k < filtered_count; k++) {
            int i = filtered_indices[k];
            if (contains_folded(database[i].paths, needle, ascii)) filtered_indices[n++] = i;
        }
        filtered_count = n;
    } else {
        filtered_count = 0;
        for (int i = 0; i < db_count; i++) {
            if (contains_folded(database[i].paths, needle, ascii)) filtered_indices[filtered_count++] = i;
        }
    }
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

static void stop_midi(void) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &old);

    pid_t pid = current_player_pid;
    if (pid > 0) {
        // Negative pid = whole process group: also stops players started by a
        // playmidi.sh that does not 'exec' its player.
        kill(-pid, SIGTERM);
        current_player_pid = 0;
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
    // The child is reaped by the SIGCHLD handler, so no zombies are left behind.
}

static const char *player_command(void) {
    // Prefer a playmidi.sh in the current directory (as documented in the README),
    // otherwise fall back to a lookup in $PATH.
    if (access("./playmidi.sh", X_OK) == 0) return "./playmidi.sh";
    return "playmidi.sh";
}

static void play_midi(int index) {
    if (index < 0 || index >= filtered_count) return;

    stop_midi();

    const MidiEntry *e = &database[filtered_indices[index]];
    char md5_path[512];

    snprintf(md5_path, sizeof(md5_path), "%c/%s.mid", e->md5[0], e->md5);
    if (access(md5_path, F_OK) == -1) {
        snprintf(md5_path, sizeof(md5_path), "%c/%s.midi", e->md5[0], e->md5);
        if (access(md5_path, F_OK) == -1) {
            snprintf(md5_path, sizeof(md5_path), "%c/%s", e->md5[0], e->md5);
        }
    }

    printf("Playing: %s\n", e->paths);
    fflush(stdout);

    const char *cmd = player_command();

    // Block SIGCHLD until current_player_pid is set, so a player that exits
    // immediately is still recognised by the handler.
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &old);

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);                       // own process group, see stop_midi()
        sigprocmask(SIG_SETMASK, &old, NULL); // signal mask survives exec: restore it
        execlp(cmd, cmd, md5_path, (char *)NULL);
        fprintf(stderr, "Could not run %s: %s\n", cmd, strerror(errno));
        _exit(127);                          // _exit: don't flush the parent's stdio buffers
    } else if (pid > 0) {
        setpgid(pid, pid);                   // also in the parent, avoids a race with kill()
        current_player_pid = pid;
    } else {
        perror("fork");
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
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

// Convert UTF-8 to the 16-bit characters used by XDrawString16. Characters
// the font cannot have (above U+00FF for a Latin-1 font, above U+FFFF for
// any core font) become '?'. Returns the number of characters.
static int to_xchar2b(const char *text, XChar2b **buf, int *cap) {
    int need = (int)strlen(text);   // never more characters than bytes
    if (need > *cap) {
        *cap = need + 64;
        *buf = xrealloc(*buf, (size_t)*cap * sizeof(XChar2b));
    }
    int unicode_font = font_info->max_byte1 > 0;
    int n = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned int cp = utf8_next(&p);
        if (cp > 0xFFFF || (!unicode_font && cp > 0xFF)) cp = '?';
        (*buf)[n].byte1 = (unsigned char)(cp >> 8);
        (*buf)[n].byte2 = (unsigned char)(cp & 0xFF);
        n++;
    }
    return n;
}

// Draw UTF-8 text at (x, y), truncated with "..." so it fits in max_width pixels.
// Measures with the real font, so it works for any font and any window width.
static void draw_text_fit(Drawable d, int x, int y, const char *text, int max_width) {
    static XChar2b *buf = NULL;
    static int cap = 0;
    static const XChar2b ellipsis[3] = { {0, '.'}, {0, '.'}, {0, '.'} };

    int len = to_xchar2b(text, &buf, &cap);
    if (max_width <= 0 || len == 0) return;

    if (XTextWidth16(font_info, buf, len) <= max_width) {
        XDrawString16(dpy, d, gc, x, y, buf, len);
        return;
    }

    int avail = max_width - XTextWidth16(font_info, ellipsis, 3);
    if (avail <= 0) return;   // not even room for "..."

    // Binary search for the longest prefix that fits.
    int lo = 0, hi = len;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (XTextWidth16(font_info, buf, mid) <= avail) lo = mid;
        else hi = mid - 1;
    }
    XDrawString16(dpy, d, gc, x, y, buf, lo);
    XDrawString16(dpy, d, gc, x + XTextWidth16(font_info, buf, lo), y, ellipsis, 3);
}

static void draw_ui(void) {
    if (!pixmap) return;

    int text_width = win_width - 2 * padding_left;

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
            XSetForeground(dpy, gc, col_selected_fg);
        } else {
            XSetForeground(dpy, gc, col_mid_fg);
        }

        const MidiEntry *e = &database[filtered_indices[current_idx]];
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "[%c/%s] ", e->md5[0], e->md5);

        // Draw "[x/md5] paths" without building a big temporary buffer.
        int y_text = y_pos + padding_rows + font_ascent;
        int prefix_w = XTextWidth(font_info, prefix, (int)strlen(prefix));
        if (prefix_w >= text_width) {
            draw_text_fit(pixmap, padding_left, y_text, prefix, text_width);
        } else {
            XDrawString(dpy, pixmap, gc, padding_left, y_text, prefix, (int)strlen(prefix));
            draw_text_fit(pixmap, padding_left + prefix_w, y_text, e->paths, text_width - prefix_w);
        }
    }

    // 2. Search bar (top)
    XSetForeground(dpy, gc, col_top_bg);
    XFillRectangle(dpy, pixmap, gc, 0, 0, win_width, header_height);
    XSetForeground(dpy, gc, col_top_fg);
    char search_label[600];
    snprintf(search_label, sizeof(search_label), "Search: %s_", search_query);
    draw_text_fit(pixmap, padding_left, padding_top_bottom_bars + font_ascent, search_label, text_width);

    // 3. Status bar (bottom)
    int footer_y = win_height - footer_height;
    XSetForeground(dpy, gc, col_bot_bg);
    XFillRectangle(dpy, pixmap, gc, 0, footer_y, win_width, footer_height);
    XSetForeground(dpy, gc, col_bot_fg);
    char status_text[256];
    snprintf(status_text, sizeof(status_text),
             "Found: %d/%d  |  [Enter/Click]: Play  |  [Ctrl]: Stop Music  |  [ESC]: Quit",
             filtered_count, db_count);
    draw_text_fit(pixmap, padding_left, footer_y + padding_top_bottom_bars + font_ascent, status_text, text_width);

    // 4. Copy pixmap to the window
    XCopyArea(dpy, pixmap, win, gc, 0, 0, win_width, win_height, 0, 0);
    XFlush(dpy);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void) {
    load_database();
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

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    set_window_icon(dpy, win);

    // Handle the window manager's close button (X) ourselves
    Atom wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    XStoreName(dpy, win, "MIDI DB Browser");
    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);

    // Load the first available font and retrieve its metrics.
    for (size_t i = 0; i < sizeof(font_names) / sizeof(font_names[0]) && !font_info; i++)
        font_info = XLoadQueryFont(dpy, font_names[i]);
    if (font_info) {
        font_loaded = 1;
        XSetFont(dpy, gc, font_info->fid);
        if (font_info->max_byte1 == 0)
            fprintf(stderr, "Using a Latin-1 font: characters above U+00FF are shown as '?'.\n");
    } else {
        fprintf(stderr, "Could not load any configured font, falling back to the default.\n");
        font_info = XQueryFont(dpy, XGContextFromGC(gc));
        if (!font_info) {
            fprintf(stderr, "Could not query the default font.\n");
            return 1;
        }
    }

    // Dynamic layout calculation (based on the font)
    font_ascent   = font_info->ascent;
    font_height   = font_info->ascent + font_info->descent;
    row_height    = font_height + (padding_rows * 2);
    header_height = font_height + (padding_top_bottom_bars * 2);
    footer_height = font_height + (padding_top_bottom_bars * 2);

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
                stop_midi();
                printf("Music stopped via CTRL.\n");
            }
            else if (keysym == XK_Escape) {
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
                play_midi(selected_index);
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

        case ButtonPress: {
            int mouse_y = event.xbutton.y;
            int rows = visible_rows();
            if (event.xbutton.button == 1) {
                // Click within the drawn rows of the central area
                if (mouse_y >= header_height && mouse_y < (win_height - footer_height)) {
                    int clicked_row = (mouse_y - header_height) / row_height;
                    int target_idx = clicked_row + scroll_offset;
                    if (clicked_row < rows && target_idx < filtered_count) {
                        selected_index = target_idx;
                        draw_ui();
                        play_midi(selected_index);
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

    stop_midi();
    if (pixmap) XFreePixmap(dpy, pixmap);
    if (font_loaded) XFreeFont(dpy, font_info);
    else XFreeFontInfo(NULL, font_info, 1);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    for (int i = 0; i < db_count; i++) free(database[i].paths);
    free(database);
    free(filtered_indices);
    return 0;
}
