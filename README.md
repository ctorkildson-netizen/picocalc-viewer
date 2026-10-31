# Picture Viewer for the PicoCalc

A picture viewer for the ClockworkPi **PicoCalc** with an RP2350 (Pico 2 W),
UF2 Loader and Blair Leduc's [PicoCalc Shell](https://github.com/BlairLeduc/picocalc-shell). It shows JPEG, PNG and BMP pictures from the SD card fitted
to the 320x320 screen, previews each one in the list, steps through a
folder, rotates and brightens pictures, and runs slide shows. Leave one
running and the PicoCalc is a picture frame.

**[VIEWER.TXT](VIEWER.TXT)** is the guide for users: installing, the keys,
the settings, what it can and can't open.

## Trying it

1. Copy `viewer.uf2` (from the latest release) into `pico2-apps` on the SD card.
2. Copy the [`samples`](samples/) folder into a folder called `/pictures`.
3. Run viewer from the shell.

## How fast

At 250 MHz, a 4000x3000 camera JPEG takes 2.5 to 3 seconds to load and fit
to the screen, a 640x480 PNG about half a second. A slide show loads the
next picture behind the one on screen, so the change happens in one go.

The speed comes from:

- decoding JPEGs at 1/2, 1/4 or 1/8 size when that still fills the screen;
- converting only the rows of a picture that land on screen;
- reading the card in 16 KB pieces;
- running at 250 MHz while the viewer is open (`speed = normal` in
  `/viewer/viewer.cfg` keeps the rated 150 MHz).

## Building

With the Pico SDK 2.x, and `PICO_SDK_PATH` pointing at it:

```
git clone https://github.com/ctorkildson-netizen/picocalc-viewer
cd picocalc-viewer
git submodule update --init --recursive
mkdir build && cd build
cmake .. && make
```

That makes `viewer.uf2`.

The source is short: [`src/main.c`](src/main.c) is the viewer itself. The
pieces other programs can use are in
[picocalc-extras](https://github.com/ctorkildson-netizen/picocalc-extras),
a submodule:

- [`picker/`](https://github.com/ctorkildson-netizen/picocalc-extras/tree/main/picker),
  the file list with its preview panel;
- [`images/`](https://github.com/ctorkildson-netizen/picocalc-extras/tree/main/images),
  the JPEG/PNG/BMP decoders, with a harness that runs them on a PC.

[`tools/make_banner.py`](tools/make_banner.py) draws the banner at the top
of the list (it needs Python, Pillow and the DejaVu fonts).

## Submodules

| Submodule | Pinned to |
|---|---|
| [picocalc-extras](https://github.com/ctorkildson-netizen/picocalc-extras) | v2.1 (the list of files, the picture decoders, and FatFs's build) |
| [picocalc-shell](https://github.com/BlairLeduc/picocalc-shell) (Blair Leduc) | v1.2 |
| [picocalc-text-starter](https://github.com/BlairLeduc/picocalc-text-starter) (Blair Leduc) | v0.17 (the screen, keyboard and SD card, with FatFs) |

Dependabot opens a pull request here when a submodule's repo moves on.

## Licence

MIT (see [LICENSE](LICENSE)). The JPEG and PNG decoders keep their own
licences, which allow this; see picocalc-extras. The sample pictures are
credited in [samples/CREDITS.TXT](samples/CREDITS.TXT).

## Status

A test build from one user and one PicoCalc. Reports and improvements are
welcome.
