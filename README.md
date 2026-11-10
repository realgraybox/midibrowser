<img width="812" height="436" alt="midibrowser" src="https://github.com/user-attachments/assets/ca035f56-11bd-4775-8294-a9ff727da713" />
# MIDI DB Browser (midibrowser)

A minimalist, ultra-fast, and resource-efficient browser for large collections of MIDI files, or any other list you can put in a text file (samples, archives, documents ...), built using **pure C and Xlib**.

This tool is designed to handle hundreds of thousands of entries with zero lag. It loads a flat, tab-separated text database into system memory (RAM) and allows for instantaneous typing-based searching and filtering.

## 🚀 Features

- **Instantaneous Search:** Filters through over 170,000 data rows in less than 2 milliseconds as you type.
- **Double Buffering (Pixmap):** All drawing operations are performed off-screen first, ensuring a 100% flicker-free experience—even during window resizing.
- **Dynamic UI Scaling:** The layout, bars, and click zones automatically adapt to the window dimensions and the system font (padding and spacing are calculated dynamically based on font metrics).
- **Decoupled Player Control:** Forwards the selected entry to an external program (by default `playmidi.sh`), so you can swap playback engines (e.g., Timidity, aplaymidi) without recompiling.
- **General Purpose:** Command line options choose the file, delimiter, columns to search and show, and the program to run, so one binary can browse several collections at once (see Options below).
- **International Characters:** File names are UTF-8 (Latin-1 lines are converted when loading). You can type and search for letters such as æ, ø and å, case-insensitively (Æ finds æ). This uses only core X11 fonts, so no extra libraries are needed.
- **Keyboard & Mouse Navigation:** Full support for arrow keys, mouse wheel scrolling, and instant selection via mouse clicks.

## 🛠 Prerequisites & Installation

You need a C compiler and the X11 development library (libX11). No other libraries are used.

### 1. Install Dependencies
**Ubuntu / Debian:**
```bash
sudo apt install build-essential libx11-dev
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc make libX11-devel
```

### 2. Compilation
```bash
make                              # build ./midibrowser
make install                      # install to /usr/local/bin (PREFIX=/usr and DESTDIR= are supported)
make STATIC=1                     # static build (default on i386 toolchains, e.g. uClibc)
```
On current glibc distributions a static libX11 also needs its dependencies:
`make STATIC=1 LDLIBS="-lX11 -lxcb -lXau -lXdmcp"`.

Without make: `gcc -Os midibrowser.c -o midibrowser -lX11`

### 3. Data Layout
The program is started from a data folder containing the database file, the MIDI files and (optionally) the playback script:

```text
data/
├── midi_database_kompakt.txt
├── playmidi.sh
├── 0/0a46edd11874a30a4315c683fbdf9dea.mid
├── 1/1595531b64ef3487d0bab672c6d664d6.mid
└── ...  (one folder per first hex character, 0-9 and a-f)
```

Each MIDI file is stored as `<first character of key>/<key>.mid` (`.midi` or no extension are also accepted). This is the layout of the [Lakh MIDI Dataset](https://colinraffel.com/projects/lmd/) (`lmd_full`).

### 4. Database Format
`midi_database_kompakt.txt` is a plain text file with one record per line: a unique key (the file's MD5 hash), a **tab** character, and one or more original names separated by `; `. The names are what you search in. Empty lines are ignored.

```text
4945940cfd5f38c53f4d6c5f0beca203	beatles/Ob-La-Di_Ob-La-Da_2.mid; beatles/ob_la_di_ob_la_da.mid
a28a1a84526d81b3d7f5cd679869449b	beatles/dont_ever_change.mid
```

**From the Lakh MIDI Dataset** (`md5_to_paths.json`):
```bash
python3 -c 'import json,sys; [print(k+"\t"+"; ".join(v)) for k,v in json.load(open(sys.argv[1])).items()]' \
    md5_to_paths.json > midi_database_kompakt.txt
```

**From your own folder of MIDI files** (copies each file into the layout above; duplicates are merged into one line):
```bash
SRC=~/midi    # your MIDI collection
find "$SRC" -type f \( -iname '*.mid' -o -iname '*.midi' \) -print0 |
while IFS= read -r -d '' f; do
    h=$(md5sum "$f" | cut -c1-32)
    mkdir -p "${h:0:1}" && cp "$f" "${h:0:1}/$h.mid"
    printf '%s\t%s\n' "$h" "${f#$SRC/}"
done | awk -F'\t' '{ if ($1 in a) a[$1] = a[$1] "; " $2; else a[$1] = $2 } END { for (k in a) print k "\t" a[k] }' \
    > midi_database_kompakt.txt
```

### 5. Playback Script (`playmidi.sh`)
Create a file named `playmidi.sh` in the directory you start the program from (or anywhere in your `$PATH`), make it executable (`chmod +x playmidi.sh`), and define your preferred playback command. A `./playmidi.sh` in the current directory takes precedence over one in `$PATH`. The script runs in its own process group, so stopping playback also stops a player the script starts without `exec`. For example:

```bash
#!/bin/bash
# Example using timidity
exec timidity "$1"
```

## ⚙️ Options & Other Collections

Without options the program works as described above. Options make it a browser for other lists:

| Option | Meaning | Default |
| :--- | :--- | :--- |
| `-f FILE` | Database file, `-` for standard input | `midi_database_kompakt.txt` |
| `-d CHAR` | Column delimiter: a single character, or `tab` | tab |
| `-s COLS` | Column(s) to search, e.g. `2` or `1,3` | `2` |
| `-c COLS` | Column(s) to show, e.g. `2,1`, or a template like `'{2}  ({1})'` | `'[{1:1}/{1}] {2}'` |
| `-p TEMPLATE` | Argument for the command, e.g. `'{1}'` or `'samples/{2}.wav'` | MIDI layout `{1:1}/{1}.mid` |
| `-x COMMAND` | Program to run, may include options, e.g. `'aplay -q'` | `./playmidi.sh` or `playmidi.sh` from `$PATH` |
| `-T TITLE` | Window title | `MIDI DB Browser` |
| `-a LABEL` | Action shown in the status bar, e.g. `View` | `Play` |
| `-k` | Keep programs running: don't stop the previous one when starting a new one; Ctrl does nothing | off |

- **Templates:** `{N}` is the value of column N (columns are numbered from 1), and `{N:K}` its first K characters. Other text is copied as is. A missing column is empty.
- **One-column files** (a plain list such as the output of `find`) are shown, searched and passed to the command as they are, with no options needed.
- **The command** gets the value as its last argument, exactly as it is, even with spaces or quotes. If `-x` contains spaces or shell characters, it is run with `/bin/sh -c`, so `-x 'aplay -q'` works.
- **Several windows** can run at the same time; each one only stops its own program. Use `-T` to tell them apart.
- **Limits:** there is no CSV quoting, so choose a delimiter that never occurs in the data (tab or `|` are good choices). With OSS (`/dev/dsp`), usually only one program can play sound at a time, so a second audio window may report "device busy" unless a mixer (ALSA dmix, PulseAudio, OSS4 vmix) is used.

### Example start scripts

```sh
#!/bin/sh
# midi.sh - the MIDI collection, no options needed
cd /data/midi && exec midibrowser -T "MIDI"
```

```sh
#!/bin/sh
# wav.sh - a plain list of files:  find . -name '*.wav' | sed 's|^\./||' > wav.txt
cd /data/wav && exec midibrowser -f wav.txt -x 'aplay -q' -T "WAV samples"
```

```sh
#!/bin/sh
# kiss.sh - kiss.txt has lines like:  pinky|Pinky the cat (1995)|sets/pinky.lzh
# Search name and description, show the description first, open the file in the viewer.
cd /data/kiss && exec midibrowser -f kiss.txt -d '|' -s 1,2 -c '{2}  ({1})' -p '{3}' \
    -x fkiss -a View -k -T "KiSS sets"
```

```sh
#!/bin/sh
# src.sh - src.txt has lines like:  linux-2.6.39.tar.gz<TAB>Linux kernel 2.6.39
# Show the archive's contents in a terminal.
cd /data/src && exec midibrowser -f src.txt -s 1,2 -c 2,1 -p '{1}' \
    -x "xterm -e sh -c 'tar tvf \"\$1\" | less' sh" -a List -k -T "Sources"
```

Run `midibrowser -h` for a short summary of the options.

## 🎮 Shortcuts & Controls

| Key / Action | Function |
| :--- | :--- |
| **Keyboard (letters, digits, æøå ...)** | Type in the top search bar to filter the database in real-time (case-insensitive) |
| **BackSpace** | Delete characters in the search query |
| **Arrow Up / Down** | Navigate up and down the filtered list |
| **Enter** | Play / run the command for the selected entry (stops the previous one, unless `-k`) |
| **Left or Right CTRL** | Stop the running program immediately (not with `-k`) |
| **ESC** | Quit; stops the running program (programs started with `-k` keep running) |
| **Left Mouse Click** | Select and play / run an entry instantly |
| **Mouse Scroll Wheel** | Scroll rapidly up and down through the list |

## 🎨 Customization

Colors, paddings and the font list are variables at the top of `midibrowser.c`. Change them and recompile; the layout is calculated from the font's size at startup.

By default the Unicode version of the classic `fixed` font is used (`-misc-fixed-...-iso10646-1`, from the `xfonts-base` package). If it isn't installed, the plain `fixed` font is used: letters from Western European languages (Latin-1) still display correctly, and other characters are shown as `?`.

## 📝 License
Released under the [zlib license](LICENSE).
