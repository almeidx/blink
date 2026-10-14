# Blink

Screenshots and screen recordings for KDE Plasma, inspired by ShareX and focused on performance.

- **Region**: freezes the screens, then drag to select an area or click a window (or the desktop) to take all of it.
- **Active window** and **full screen** (all screens).
- **Recording**: select a region or window the same way, then record it to MP4 (H.264) with system audio and the
  microphone.
- **Upload** to S3-compatible storage, with the link copied to the clipboard.

Captures come from KWin's own capture APIs.

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

### Upload paths

**Path in bucket**, in the Upload settings, decides where uploads go: placeholders in braces are filled in for each
upload, and `/` separates folders. The default, `{random:12}.{extension}`, gives keys like `k3J9xQ2aB7mP.png`;
`{random:8}/{filename}.{extension}` gives `k3J9xQ2a/Blink_2026-10-14_13-26-43.png`. The button next to the field
inserts placeholders, and the line under it shows an example link.

| Placeholder | Becomes |
| --- | --- |
| `{filename}` | The capture's file name without extension, as it is saved: `Blink_2026-10-14_13-26-43` |
| `{extension}` | `png` for screenshots, `mp4` for recordings |
| `{random:N}` | N random letters and digits |
| `{random-readable:N}` | N random letters and digits, leaving out the look-alikes `0`, `O`, `1`, `I`, `l` and `o` |
| `{random-digits:N}` | N random digits |
| `{random-hex:N}` | N random hexadecimal digits, lowercase |
| `{uuid}` | A random UUID |
| `{year}`, `{month}`, `{day}` | `2026`, `10`, `14` |
| `{hour}`, `{minute}`, `{second}`, `{millisecond}` | `13`, `26`, `43`, `042`, on a 24-hour clock |
| `{month-name}`, `{weekday}` | `October`, `Wednesday` |
| `{week}` | Week of the year, `01` to `53`: week 1 runs from 1 January to the first Sunday |
| `{unix}` | Unix time, in seconds |
| `{width}`, `{height}` | The capture's size in pixels |
| `{username}`, `{hostname}` | Your user name and the computer's name |

Dates and times are the capture's, in local time. These are ShareX's file name codes, spelled out: `%ra{8}` is
`{random:8}`, `%y` is `{year}`. Random parts come from the system's cryptographic random number generator, since links
to uploads are often public. A path without one, such as `{year}/{month}/{day}.{extension}`, repeats, and each upload
to it replaces the previous one.

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
