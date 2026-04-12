# rv — Reading Viewer

Vertical image stitching viewer for Manhwa/Webtoon reading. Built with C + SDL2. Designed to be driven programmatically by a parent process (like [mrm](https://github.com/Lucasldab/mrm)) via CLI args and IPC.

## Features

- **Vertical stitching** — images appended into one continuous scrollable strip
- **Smooth scrolling** — configurable speed with shift modifier for fast scroll
- **Zoom** — ctrl+scroll or +/- keys
- **IPC** — unix socket for runtime commands (append images, navigate, quit)
- **Formats** — PNG, JPG, WebP via SDL2_image

## Build

Dependencies: `sdl2`, `sdl2_image`

```sh
# Arch Linux
sudo pacman -S sdl2-compat sdl2_image

# Build
make

# Install (optional)
sudo make install
```

## Usage

```sh
# Open images directly
rv page1.png page2.png page3.png

# With options
rv --scroll-speed 100 --fast-scroll-speed 800 --fullscreen --title "Chapter 1" page*.png

# Custom socket path
rv --sock /tmp/my-viewer.sock page1.png
```

rv prints its socket path to stdout on startup for the parent process to capture.

## Keybinds

| Key | Action |
|-----|--------|
| `j` / `Down` | Scroll down |
| `k` / `Up` | Scroll up |
| `Shift` + scroll | Fast scroll |
| `Space` | Page down |
| `b` | Page up |
| `g` | Go to top |
| `End` | Go to bottom |
| `+` / `=` | Zoom in |
| `-` | Zoom out |
| `0` | Reset zoom |
| `Ctrl` + scroll | Zoom |
| `f` | Toggle fullscreen |
| `q` / `Esc` | Quit |

## IPC Protocol

rv listens on a unix socket (default: `/tmp/rv-<pid>.sock`). Send commands via `rv-msg`:

```sh
# Append image to bottom of strip
rv-msg /tmp/rv-12345.sock open /path/to/new-page.png

# Jump to image by index (0-based)
rv-msg /tmp/rv-12345.sock goto 5

# Scroll by pixels (negative = up)
rv-msg /tmp/rv-12345.sock scroll 500

# Close viewer
rv-msg /tmp/rv-12345.sock quit
```

## CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--scroll-speed <px>` | 80 | Normal scroll amount in pixels |
| `--fast-scroll-speed <px>` | 600 | Shift+scroll amount in pixels |
| `--fullscreen` | off | Start in fullscreen mode |
| `--title <str>` | "rv" | Window title |
| `--sock <path>` | `/tmp/rv-<pid>.sock` | Custom IPC socket path |

## License

MIT
