<img width="812" height="436" alt="midibrowser" src="https://github.com/user-attachments/assets/ca035f56-11bd-4775-8294-a9ff727da713" />
# MIDI DB Browser (midibrowser)

A minimalist, ultra-fast, and resource-efficient database browser for large collections of MIDI files (or other datasets), built using **pure C and Xlib**.

This tool is designed to handle hundreds of thousands of entries with zero lag. It loads a flat, tab-separated text database into system memory (RAM) and allows for instantaneous typing-based searching and filtering.

## 🚀 Features

- **Instantaneous Search:** Filters through over 170,000 data rows in less than 2 milliseconds as you type.
- **Double Buffering (Pixmap):** All drawing operations are performed off-screen first, ensuring a 100% flicker-free experience—even during window resizing.
- **Dynamic UI Scaling:** The layout, bars, and click zones automatically adapt to the window dimensions and the system font (padding and spacing are calculated dynamically based on font metrics).
- **Decoupled Player Control:** Forwards the selected file path to an external script (`playmidi.sh`), allowing you to easily swap playback engines (e.g., Timidity, aplaymidi) without recompiling the C binary.
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
`midi_database_kompakt.txt` is a plain text file with one record per line: a unique key (the file's MD5 hash), a **tab** character, and one or more original names separated by `; `. The names are what you search in; lines without a tab are ignored.

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

## 🎮 Shortcuts & Controls

| Key / Action | Function |
| :--- | :--- |
| **Keyboard (letters, digits, æøå ...)** | Type in the top search bar to filter the database in real-time (case-insensitive) |
| **BackSpace** | Delete characters in the search query |
| **Arrow Up / Down** | Navigate up and down the filtered list |
| **Enter** | Play the selected file (automatically terminates the previous playback) |
| **Left or Right CTRL** | Stop the currently playing MIDI immediately |
| **ESC** | Quit the program cleanly (automatically reaps background zombie processes) |
| **Left Mouse Click** | Select and play a file instantly |
| **Mouse Scroll Wheel** | Scroll rapidly up and down through the list |

## 🎨 Customization

Colors, paddings and the font list are variables at the top of `midibrowser.c`. Change them and recompile; the layout is calculated from the font's size at startup.

By default the Unicode version of the classic `fixed` font is used (`-misc-fixed-...-iso10646-1`, from the `xfonts-base` package). If it isn't installed, the plain `fixed` font is used: letters from Western European languages (Latin-1) still display correctly, and other characters are shown as `?`.

## 📝 License
Released under the [zlib license](LICENSE).
