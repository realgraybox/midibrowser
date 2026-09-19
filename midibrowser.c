#define _GNU_SOURCE 
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_LINES 200000 
#define MAX_LINE_LEN 4096

typedef struct {
    char md5[33];
    char paths[MAX_LINE_LEN];
} MidiEntry;

MidiEntry database[MAX_LINES];
int db_count = 0;

int filtered_indices[MAX_LINES];
int filtered_count = 0;

char search_query[512] = "";
int selected_index = 0;
int scroll_offset = 0;

Display *dpy;
Window win;
GC gc;
Pixmap pixmap = 0;

pid_t current_player_pid = 0;

// configuration and style
int win_width = 800;
int win_height = 400;


// colors
unsigned long col_top_bg      = 0x2D3748; 
unsigned long col_top_fg      = 0xFFFFFF; 
unsigned long col_mid_bg      = 0x1A202C; 
unsigned long col_mid_fg      = 0xE2E8F0; 
unsigned long col_selected_bg = 0x3182CE; 
unsigned long col_selected_fg = 0xFFFFFF; 
unsigned long col_bot_bg      = 0x2D3748; 
unsigned long col_bot_fg      = 0xA0AEC0;

// paddings
int padding_top_bottom_bars = 4; // padding over/under text in header and footer
int padding_rows            = 2; // padding over/under text in db-view section


const char *font_name = "fixed";	//or 9x15

// Dynamic layout sizes (Calculated based on font + padding)
int font_height = 0;
int font_ascent = 0;
int row_height = 0;
int header_height = 0;
int footer_height = 0;

static void set_window_icon(Display *mydpy, Window mywin) {
	unsigned long icon_data[] = {
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
   
    int data_length = 2 + (8 * 8); // 2 (measure) + (width * height)

    Atom wm_icon = XInternAtom(mydpy, "_NET_WM_ICON", False);
    Atom cardinal = XInternAtom(mydpy, "CARDINAL", False);
    XChangeProperty(mydpy, mywin, wm_icon, cardinal, 32, PropModeReplace, (unsigned char*)icon_data, data_length);
    XFlush(mydpy); 
}

void load_database() {
    FILE *fp = fopen("midi_database_kompakt.txt", "r");
    if (!fp) {
        perror("Could not open database \"midi_database_kompakt.txt\"");
        exit(1);
    }

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp) && db_count < MAX_LINES) {
        line[strcspn(line, "\n")] = 0;

        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            strncpy(database[db_count].md5, line, 32);
            database[db_count].md5[32] = '\0';
            strncpy(database[db_count].paths, tab + 1, MAX_LINE_LEN - 1);
            db_count++;
        }
    }
    fclose(fp);
    printf("Read %d records in memory.\n", db_count);
}

void update_filter() {
    filtered_count = 0;
    for (int i = 0; i < db_count; i++) {
        if (search_query[0] == '\0' || strcasestr(database[i].paths, search_query) != NULL) {
            filtered_indices[filtered_count] = i;
            filtered_count++;
        }
    }
    
    if (selected_index >= filtered_count) {
        selected_index = filtered_count - 1;
    }
    if (selected_index < 0) selected_index = 0;
    scroll_offset = 0;
}

void stop_midi() {
    if (current_player_pid > 0) {
        kill(current_player_pid, SIGTERM);
        waitpid(current_player_pid, NULL, WNOHANG);
        current_player_pid = 0;
    }
}

void play_midi(int index) {
    if (index < 0 || index >= filtered_count) return;
    
    stop_midi();
    
    int db_idx = filtered_indices[index];
    char first_char = database[db_idx].md5[0];
    char md5_path[512];
    
    snprintf(md5_path, sizeof(md5_path), "%c/%s.mid", first_char, database[db_idx].md5);
    if (access(md5_path, F_OK) == -1) {
        snprintf(md5_path, sizeof(md5_path), "%c/%s.midi", first_char, database[db_idx].md5);
        if (access(md5_path, F_OK) == -1) {
            snprintf(md5_path, sizeof(md5_path), "%c/%s", first_char, database[db_idx].md5);
        }
    }

    printf("Playing: %s\n", database[db_idx].paths);

    pid_t pid = fork();
    if (pid == 0) {
        //close(1);
        execlp("playmidi.sh", "playmidi.sh", md5_path, NULL);
        exit(1);
    } else if (pid > 0) {
        current_player_pid = pid;
    }
}

void check_pixmap() {
    if (pixmap) {
        XFreePixmap(dpy, pixmap);
    }
    pixmap = XCreatePixmap(dpy, win, win_width, win_height, DefaultDepth(dpy, DefaultScreen(dpy)));
}

void draw_ui() {
    if (!pixmap) return;

    // 1. CLEAR PIXMAP WITH COLOR FROM DBVIEW BACKGROUND COLOR
    XSetForeground(dpy, gc, col_mid_bg);
    XFillRectangle(dpy, pixmap, gc, 0, 0, win_width, win_height);

    // Calculate how many rows there is actually room for in the central section now.
    int mid_zone_height = win_height - header_height - footer_height;
    int max_visible_rows = mid_zone_height / row_height;
    int max_chars = (win_width - 30) / 7; 

    // Middle section text/rows
    for (int i = 0; i < max_visible_rows && (i + scroll_offset) < filtered_count; i++) {
        int current_idx = i + scroll_offset;
        int y_pos = header_height + (i * row_height);

        if (current_idx == selected_index) {
            XSetForeground(dpy, gc, col_selected_bg);
            XFillRectangle(dpy, pixmap, gc, 0, y_pos, win_width, row_height);
            XSetForeground(dpy, gc, col_selected_fg);
        } else {
            XSetForeground(dpy, gc, col_mid_fg);
        }

        MidiEntry entry = database[filtered_indices[current_idx]];
        char row_text[MAX_LINE_LEN + 50];
        snprintf(row_text, sizeof(row_text), "[%c/%s] %s", entry.md5[0], entry.md5, entry.paths);
        
        if ((int)strlen(row_text) > max_chars) {
            row_text[max_chars - 3] = '\0';
            strcat(row_text, "...");
        }
        // The baseline y-position is calculated based on the row's top + padding + the font's ascent.
        XDrawString(dpy, pixmap, gc, 15, y_pos + padding_rows + font_ascent, row_text, strlen(row_text));
    }

    // 2. SEARCH BAR (Top)
    XSetForeground(dpy, gc, col_top_bg);
    XFillRectangle(dpy, pixmap, gc, 0, 0, win_width, header_height);
    XSetForeground(dpy, gc, col_top_fg);
    char search_label[600];
    snprintf(search_label, sizeof(search_label), "Search: %s_", search_query);
    XDrawString(dpy, pixmap, gc, 15, padding_top_bottom_bars + font_ascent, search_label, strlen(search_label));

    // 3. BOTTOM BEAM
    int footer_y = win_height - footer_height;
    XSetForeground(dpy, gc, col_bot_bg);
    XFillRectangle(dpy, pixmap, gc, 0, footer_y, win_width, footer_height);
    XSetForeground(dpy, gc, col_bot_fg);
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Found: %d/%d  |  [Enter/Click]: Play  |  [Ctrl]: Stop Music  |  [ESC]: Quit", filtered_count, db_count);
    XDrawString(dpy, pixmap, gc, 15, footer_y + padding_top_bottom_bars + font_ascent, status_text, strlen(status_text));

    // 4. COPY PIXMAP TO THE WINDOW
    XCopyArea(dpy, pixmap, win, gc, 0, 0, win_width, win_height, 0, 0);
    XFlush(dpy);
}

int main() {

    load_database();
    update_filter();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Could not open X display\\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 10, 10, win_width, win_height, 1,
                                     BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    XSetWindowBackgroundPixmap(dpy, win, None);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    set_window_icon(dpy, win);
    
    // --- NEW CODE: Tell X11 that we want to handle the close button (X) ourselves ---
	Atom wm_delete_window;
	// 1. Tell X11 that we want to access the "WM_DELETE_WINDOW" atom
	wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	// 2. Register this atom as a protocol on your window
	XSetWMProtocols(dpy, win, &wm_delete_window, 1);
	
       
    XMapWindow(dpy, win);
    XStoreName(dpy, win, "MIDI DB Browser");

    gc = XCreateGC(dpy, win, 0, NULL);
    
    // Load the font and retrieve its metrics (size).
    XFontStruct *font_info = XLoadQueryFont(dpy, font_name);
    if (!font_info) {
        fprintf(stderr, "Could not load the font '%s', falling back to the default.\n", font_name);
        font_info = XQueryFont(dpy, XGContextFromGC(gc));
    } else {
        XSetFont(dpy, gc, font_info->fid);
    }

    // =========================================================================
    // DYNAMIC LAYOUT CALCULATION (Based on the font)
    // =========================================================================
    font_ascent  = font_info->ascent;
    font_height  = font_info->ascent + font_info->descent;
    
    row_height    = font_height + (padding_rows * 2);
    header_height = font_height + (padding_top_bottom_bars * 2);
    footer_height = font_height + (padding_top_bottom_bars * 2);
    // =========================================================================

    check_pixmap();

    int running = 1;
    XEvent event;

    while (running) {
        XNextEvent(dpy, &event);
        
        // Allows the program to be closed using the window's "X" button with the mouse.
		if (event.type == ClientMessage) {
			if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
				running = 0;
			}
		}

        if (event.type == ConfigureNotify) {
            if (event.xconfigure.width != win_width || event.xconfigure.height != win_height) {
                win_width = event.xconfigure.width;
                win_height = event.xconfigure.height;
                check_pixmap();
                draw_ui();
            }
        }

        if (event.type == Expose) {
            if (event.xexpose.count == 0) {
                draw_ui();
            }
        }

        if (event.type == KeyPress) {
            KeySym keysym = XLookupKeysym(&event.xkey, 0);
            int len = strlen(search_query);

            if (keysym == XK_Control_L || keysym == XK_Control_R) {
                stop_midi();
                printf("Music stopped via CTRL.\n");
            }
            else if (keysym == XK_Escape) {
                running = 0;
            } 
            else if (keysym == XK_Up) {
                if (selected_index > 0) {
                    selected_index--;
                    if (selected_index < scroll_offset) {
                        scroll_offset = selected_index;
                    }
                    draw_ui();
                }
            } 
            else if (keysym == XK_Down) {
                if (selected_index < filtered_count - 1) {
                    selected_index++;
                    int mid_zone_height = win_height - header_height - footer_height;
					int max_visible_rows = mid_zone_height / row_height;
					if (selected_index >= scroll_offset + max_visible_rows) {
						scroll_offset++;
					}
					draw_ui();
				}
			} 
			else if (keysym == XK_Return) {
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
				int num = XLookupString(&event.xkey, buf, sizeof(buf), NULL, NULL);
				if (num > 0 && len + num < (int)sizeof(search_query) - 1) {
					buf[num] = '\0';
					if (buf[0] >= 32 && buf[0] <= 126) {
						strcat(search_query, buf);
						update_filter();draw_ui();
					}
				}
			}
		}
		if (event.type == ButtonPress) {
			int mouse_y = event.xbutton.y;
			if (event.xbutton.button == 1) {
				// Click within the dynamic boundaries of the central area.
				if (mouse_y >= header_height && mouse_y < (win_height - footer_height)) {
					int clicked_row = (mouse_y - header_height) / row_height;
					int target_idx = clicked_row + scroll_offset;
					if (target_idx < filtered_count) {
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
				int mid_zone_height = win_height - header_height - footer_height;
				int max_visible_rows = mid_zone_height / row_height;
				if (scroll_offset + max_visible_rows < filtered_count) {
					scroll_offset++;
					draw_ui();
				}
			}
		}
	}
	stop_midi();
	if (pixmap) XFreePixmap(dpy, pixmap);
	XFreeFontNames(XGetFontPath(dpy, &font_height)); 
	// Just for safe release if necessary.
	XFreeGC(dpy, gc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	return 0;
}
