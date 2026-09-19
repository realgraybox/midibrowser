# MIDI DB Browser (midibrowser)

A minimalist, ultra-fast, and resource-efficient database browser for large collections of MIDI files (or other datasets), built using **pure C and Xlib**.

This tool is designed to handle hundreds of thousands of entries with zero lag. It loads a flat, tab-separated text database into system memory (RAM) and allows for instantaneous typing-based searching and filtering.

## 🚀 Features

- **Instantaneous Search:** Filters through over 170,000 data rows in less than 2 milliseconds as you type.
- **Double Buffering (Pixmap):** All drawing operations are performed off-screen first, ensuring a 100% flicker-free experience—even during window resizing.
- **Dynamic UI Scaling:** The layout, bars, and click zones automatically adapt to the window dimensions and the system font (padding and spacing are calculated dynamically based on font metrics).
- **Decoupled Player Control:** Forwards the selected file path to an external script (`playmidi.sh`), allowing you to easily swap playback engines (e.g., Timidity, aplaymidi) without recompiling the C binary.
- **Keyboard & Mouse Navigation:** Full support for arrow keys, mouse wheel scrolling, and instant selection via mouse clicks.

## 🛠 Prerequisites & Installation

You need the X11 development libraries installed on your Linux system to compile the program.

### 1. Install Dependencies
**Ubuntu / Debian:**
```bash
sudo apt install libx11-dev
```

**Fedora / RHEL:**
```bash
sudo apt install libX11-devel
```

### 2. Database Format
The program expects a tab-separated text file named `midi_database_kompakt.txt` in the same directory. The format must consist of a unique key (such as an MD5 hash), followed by a tab character (`\t`), and one or more paths separated by semicolons:

```text
4945940cfd5f38c53f4d6c5f0beca203    beatles/Ob-La-Di_Ob-La-Da_2.mid; beatles/ob_la_di_ob_la_da.mid
a28a1a84526d81b3d7f5cd679869449b    beatles/dont_ever_change.mid
```

### 3. Playback Script (`playmidi.sh`)
Create a file named `playmidi.sh` in the same directory, make it executable (`chmod +x playmidi.sh`), and define your preferred playback command. For example:

```bash
#!/bin/bash
# Example using timidity
exec timidity "$1"
```

### 4. Compilation
Compile the program by linking the X11 library:

```bash
gcc midibrowser.c -o midibrowser -lX11
```

## 🎮 Shortcuts & Controls

| Key / Action | Function |
| :--- | :--- |
| **Keyboard (Alphanumeric)** | Type in the top search bar to filter the database in real-time |
| **BackSpace** | Delete characters in the search query |
| **Arrow Up / Down** | Navigate up and down the filtered list |
| **Enter** | Play the selected file (automatically terminates the previous playback) |
| **Left or Right CTRL** | Stop the currently playing MIDI immediately |
| **ESC** | Quit the program cleanly (automatically reaps background zombie processes) |
| **Left Mouse Click** | Select and play a file instantly |
| **Mouse Scroll Wheel** | Scroll rapidly up and down through the list |

## 🎨 Customization

Colors and structural padding can easily be customized by tweaking the variables at the beginning of the `main()` function in `midibrowser.c`. The layout automatically recalculates the geometry on the next compilation.

## 📝 License
This project is open-source. Feel free to use, modify, and distribute the code.
