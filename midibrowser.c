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

static const char *font_name = "fixed";  // or 9x15

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
        e->paths = strdup(tab + 1);
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

static void update_filter(void) {
    filtered_count = 0;
    for (int i = 0; i < db_count; i++) {
        if (search_query[0] == '\0' || strcasestr(database[i].paths, search_query) != NULL) {
            filtered_indices[filtered_count++] = i;
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

// Draw text at (x, y), truncated with "..." so it fits in max_width pixels.
// Measures with the real font, so it works for any font and any window width.
static void draw_text_fit(Drawable d, int x, int y, const char *text, int max_width) {
    int len = (int)strlen(text);
    if (max_width <= 0 || len == 0) return;

    if (XTextWidth(font_info, text, len) <= max_width) {
        XDrawString(dpy, d, gc, x, y, text, len);
        return;
    }

    const char *ellipsis = "...";
    int ell_w = XTextWidth(font_info, ellipsis, 3);
    int avail = max_width - ell_w;
    if (avail <= 0) return;   // not even room for "..."

    // Binary search for the longest prefix that fits.
    int lo = 0, hi = len;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (XTextWidth(font_info, text, mid) <= avail) lo = mid;
        else hi = mid - 1;
    }
    XDrawString(dpy, d, gc, x, y, text, lo);
    XDrawString(dpy, d, gc, x + XTextWidth(font_info, text, lo), y, ellipsis, 3);
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
    update_filter();
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

    // Load the font and retrieve its metrics.
    font_info = XLoadQueryFont(dpy, font_name);
    if (font_info) {
        font_loaded = 1;
        XSetFont(dpy, gc, font_info->fid);
    } else {
        fprintf(stderr, "Could not load the font '%s', falling back to the default.\n", font_name);
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
                    search_query[len - 1] = '\0';
                    update_filter();
                    draw_ui();
                }
            }
            else {
                char buf[8];
                int num = XLookupString(&event.xkey, buf, sizeof(buf) - 1, NULL, NULL);
                if (num > 0 && len + num < (int)sizeof(search_query) - 1) {
                    buf[num] = '\0';
                    if ((unsigned char)buf[0] >= 32 && (unsigned char)buf[0] <= 126) {
                        strcat(search_query, buf);
                        update_filter();
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
