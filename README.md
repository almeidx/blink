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
sudo pacman -U blink-capture-*-x86_64.pkg.tar.zst # Arch
sudo dnf install ./blink-capture-*.x86_64.rpm     # Fedora
```

The package was called `blink` up to 0.1.1-alpha.1. Installing `blink-capture` replaces it.

For GPU encoding on Fedora, install the
[hardware codecs from RPM Fusion](https://rpmfusion.org/Howto/Multimedia#Hardware_Accelerated_Codec).

### From source: Arch

```sh
cd packaging && makepkg -sfi
```

### From source: Fedora

```sh
sudo dnf install rpm-build
sudo dnf builddep packaging/blink-capture.spec
rpmbuild -bb --build-in-place --define "_topdir $PWD/rpmbuild" packaging/blink-capture.spec
sudo dnf install ./rpmbuild/RPMS/*/blink-capture-[0-9]*.rpm
```

If already installed previously, first delete `rpmbuild` and use `dnf reinstall` instead.

## Usage

Start Blink from the application menu. It stays in the system tray and starts with your session.

| Shortcut       | Action                    |
| -------------- | ------------------------- |
| `Print`        | Capture a region          |
| `Shift+Print`  | Capture the full screen   |
| `Meta+Print`   | Capture the active window |
| `Meta+Shift+R` | Start or stop recording   |

Blink takes these shortcuts from Spectacle on its first start. Change them in System Settings > Keyboard > Shortcuts.

The command line has the same actions: `blink --region`, `--fullscreen`, `--window`, `--record`, `--library` and
`--settings`.

Captures are copied to the clipboard and saved to `~/Pictures/Blink` and `~/Videos/Blink`, in a folder for each month.
Captures of a window are named after its app, like `Firefox_2026-10-14_13-26-43.png`.

**Library**, in the tray menu, shows the saved screenshots and recordings. Open, copy, drag or move them to the trash
from there, or upload them when S3 storage is set up.

Settings, including the S3 secret key, are kept in `~/.config/blink/blink.conf`, readable only by you.

### Folders

**Subfolder**, in the Screenshots and Recording settings, sets the folder saved captures go in. The default
`{year}-{month}` gives `2026-10`, `{year}/{month}` gives `2026/10`, and empty saves straight into the main folder. It
takes the date, time and computer placeholders below.

### Upload paths

**Path in bucket**, in the Upload settings, sets where uploads go. Placeholders are filled in for each upload:
`{random:8}/{filename}.{extension}` gives `k3J9xQ2a/Blink_2026-10-14_13-26-43.png`.

| Placeholder | Becomes |
| --- | --- |
| `{filename}` | The capture's file name: `Blink_2026-10-14_13-26-43` |
| `{extension}` | `png` or `mp4` |
| `{random:N}` | N random letters and digits |
| `{random-readable:N}` | N random letters and digits, without look-alikes such as `0` and `O` |
| `{random-digits:N}` | N random digits |
| `{random-hex:N}` | N random hexadecimal digits |
| `{uuid}` | A random UUID |
| `{year}`, `{month}`, `{day}` | `2026`, `10`, `14` |
| `{hour}`, `{minute}`, `{second}`, `{millisecond}` | `13`, `26`, `43`, `042` |
| `{month-name}`, `{weekday}` | `October`, `Wednesday` |
| `{week}` | Week of the year |
| `{unix}` | Unix time, in seconds |
| `{width}`, `{height}` | Size in pixels |
| `{username}`, `{hostname}` | User name and computer name |

Without a random part, a path can repeat, and the new upload replaces the old one.

## Build

Requires Qt 6.8+, KDE Frameworks 6 (KGlobalAccel, KGuiAddons), LayerShellQt, plasma-wayland-protocols, PipeWire,
FFmpeg 7.1+, zlib, CMake, Ninja and a C++23 compiler.

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

KWin only lets Blink capture the screen when a desktop file points at it. To run the build, quit any installed Blink
and add one:

```sh
sed "s|@CMAKE_INSTALL_FULL_BINDIR@|$PWD/build|" data/blink.desktop.in > ~/.local/share/applications/blink-dev.desktop
./build/blink
```

### Development

Code is formatted with clang-format and checked with clang-tidy.

```sh
clang-format --dry-run --Werror src/*.cpp src/*.h recorder/*.cpp recorder/*.h
cmake --build build
run-clang-tidy -p build -quiet '/(src|recorder)/[^/]*$'
```

## License

[Apache License 2.0](LICENSE)
