# xbmfont (bundled)

Embedded bitmap fonts for Xlib, from <https://github.com/realgraybox/xbmfont>
(commit 7e9b277): `xbmfont.h` and the fonts midibrowser uses (6x13, and 10x20
for large screens unless built with `SMALL_FONTS=1`), unchanged. midibrowser
draws all its text with these, so it looks the same on every X server.

The library is zlib licensed (see `LICENSE`). The fonts are the X11 misc-fixed
fonts, which are public domain; see the xbmfont repository's `fonts/README.md`
for their sources.
