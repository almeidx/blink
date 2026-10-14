# Blink

Screenshots and screen recordings for KDE Plasma, inspired by ShareX and focused on performance. Blink lives in the
system tray: press a key, and the capture is on the clipboard.

- **Region**: freezes the screens, then drag to select an area or click a window (or the desktop) to take all of it.
  A magnifier shows the pixels around the pointer; selections are exact to the device pixel, also with fractional
  scaling.
- **Active window** and **full screen** (all screens).
- **Recording**: select a region or window the same way, then record it to MP4 (H.264) with system audio and the
  microphone, until you stop it from the tray or with the shortcut again. Encoding uses NVENC on NVIDIA, VA-API on
  AMD and Intel, and falls back to software.
- **Upload** to S3-compatible storage (AWS S3, Cloudflare R2, MinIO, Garage, ...), with the link copied to the
  clipboard.

Captures come from KWin's own capture APIs: KWin hands over a 4K screen in ~15 ms, and PNG encoding runs on all
cores, ~15 ms for a 4K screenshot instead of ~200 ms with Qt's encoder.

## Install

[Releases](https://github.com/almeidx/blink/releases) have x86_64 packages for Arch Linux and Fedora:

```sh
sudo pacman -U blink-*-x86_64.pkg.tar.zst # Arch
sudo dnf install ./blink-*.x86_64.rpm     # Fedora
```

### From source: Arch

```sh
cd packaging && makepkg -sif
```

To rebuild after pulling changes, add `-f`: the version only changes on release, and makepkg refuses to overwrite the
package it built before.

### From source: Fedora

```sh
sudo dnf install rpm-build
sudo dnf builddep packaging/blink.spec
rpmbuild -bb --build-in-place --define "_topdir $PWD/rpmbuild" packaging/blink.spec
sudo dnf install ./rpmbuild/RPMS/*/blink-[0-9]*.rpm
```

If already installed previously, first delete `rpmbuild` and use `dnf reinstall` instead.

Fedora's own FFmpeg and Mesa leave out most H.264 encoders. NVENC works with the NVIDIA driver from RPM Fusion; on
AMD and Intel, install `mesa-va-drivers-freeworld` from RPM Fusion for VA-API, or keep Cisco's OpenH264 repository
enabled for the software encoder.

## Usage

Start Blink from the application menu; it starts with the session from then on (see Settings). The first start takes
these shortcuts from Spectacle, which owns them by default:

| Shortcut       | Action                        |
| -------------- | ----------------------------- |
| `Print`        | Capture a region              |
| `Shift+Print`  | Capture the full screen       |
| `Meta+Print`   | Capture the active window     |
| `Meta+Shift+R` | Start or stop recording       |

Change them in System Settings > Keyboard > Shortcuts > Blink. Left-click the tray icon to capture a region, or to
stop a recording; its menu has everything else, including the settings.

During region selection: drag to select, click to take the window or desktop under the pointer, `Enter` to take the
highlighted one, `Esc` or right click to cancel (right click while dragging starts over).

The same actions are available from the command line, which also reaches an instance that is already running:
`blink --region`, `--window`, `--fullscreen`, `--record` and `--settings`.

Recordings are saved to `~/Videos/Blink`, and copied to the clipboard as a file. Screenshots are only saved, to
`~/Pictures/Blink`, if enabled in the settings.

Settings, including the S3 secret key, are kept in `~/.config/blink/blink.conf`, readable only by you.

## Build

Requires Qt 6.8+, KDE Frameworks 6 (KGlobalAccel, KGuiAddons), LayerShellQt, plasma-wayland-protocols, PipeWire,
FFmpeg 7.1+, zlib, CMake, Ninja and a C++23 compiler.

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

KWin only lets an executable take screenshots and record the screen when a desktop file names it, by absolute path,
in `Exec`. To run a build tree, install a desktop file that points at it:

```sh
desktop=~/.local/share/applications/blink-dev.desktop
rm -f "$desktop" # KWin rereads desktop files that are added or removed, not ones edited in place
sed "s|@CMAKE_INSTALL_FULL_BINDIR@|$PWD/build|" data/blink.desktop.in > "$desktop"
./build/blink
```

Quit an installed Blink first: a second instance hands its command line to the running one and exits.

`QT_LOGGING_RULES="blink.debug=true"` logs the timings of each capture.

### Development

Code is formatted with clang-format and checked with clang-tidy.

```sh
clang-format --dry-run --Werror src/*.cpp src/*.h recorder/*.cpp recorder/*.h
cmake --build build
run-clang-tidy -p build -quiet '/(src|recorder)/[^/]*$'
```

`blink-recorder`, which the tray app starts for each recording, can be driven by hand: see `src/recorderprotocol.h`.

## License

[Apache License 2.0](LICENSE)
