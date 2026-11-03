# Blink

Fast, native screenshot and screen recording tool for KDE Plasma, inspired by ShareX.

## Features

- **Region capture**: freezes the screens; drag an area or click a window.
- **Window** and **full screen** capture.
- **Screen recording** of a region or window to MP4, with system audio and microphone, GPU-accelerated where available.
- **Clipboard**: every capture is copied, ready to paste.
- **Library** of saved screenshots and recordings, grouped by day, month or year.
- **S3 upload** to any S3-compatible storage, with the link copied to the clipboard.
- **Custom file and upload paths** with date, time and random placeholders.

## Install

[Releases](https://github.com/almeidx/blink/releases) have packages for Arch Linux and Fedora:

```sh
sudo pacman -U blink-capture-*-x86_64.pkg.tar.zst # Arch
sudo dnf install ./blink-capture-*.x86_64.rpm     # Fedora
```

On Fedora, GPU encoding needs the
[RPM Fusion hardware codecs](https://rpmfusion.org/Howto/Multimedia#Hardware_Accelerated_Codec).

## Usage

Blink lives in the system tray. Shortcuts can be changed in System Settings > Keyboard > Shortcuts.

| Shortcut       | Action                    |
| -------------- | ------------------------- |
| `Print`        | Capture a region          |
| `Shift+Print`  | Capture the full screen   |
| `Meta+Print`   | Capture the active window |
| `Meta+Shift+R` | Start or stop recording   |

## Build from source

Requires Qt 6.10+, KDE Frameworks 6, LayerShellQt 6.6+, plasma-wayland-protocols, PipeWire 1.0+, FFmpeg 7.1+, CMake
and a C++23 compiler.

```sh
# Arch
cd packaging && makepkg -sfi

# Fedora
sudo dnf install rpm-build
sudo dnf builddep packaging/blink-capture.spec
rpmbuild -bb --build-in-place --define "_topdir $PWD/rpmbuild" packaging/blink-capture.spec
sudo dnf install ./rpmbuild/RPMS/*/blink-capture-[0-9]*.rpm
```

## License

[Apache License 2.0](LICENSE)
