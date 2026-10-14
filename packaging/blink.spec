Name:           blink
# Version, Release and the changelog are updated by .github/scripts/bump-version.sh when a release is prepared.
Version:        0.1.1~alpha.1
Release:        1%{?dist}
Summary:        Fast screenshots and screen recordings for KDE Plasma
License:        Apache-2.0
URL:            https://github.com/almeidx/blink
Source:         %{url}/archive/v%{version_no_tilde}/%{name}-%{version_no_tilde}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6WaylandClient)
BuildRequires:  cmake(KF6GlobalAccel)
BuildRequires:  cmake(KF6GuiAddons)
BuildRequires:  cmake(LayerShellQt)
BuildRequires:  cmake(PlasmaWaylandProtocols)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(libavcodec)
BuildRequires:  pkgconfig(libavformat)
BuildRequires:  pkgconfig(libavfilter)
BuildRequires:  pkgconfig(libavutil)
BuildRequires:  pkgconfig(libswresample)
BuildRequires:  pkgconfig(libswscale)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  desktop-file-utils
# Owns the hicolor directories the icons install into.
Requires:       hicolor-icon-theme

%description
Blink takes screenshots of a region, a window or the whole screen and records
the screen with system audio and the microphone, inspired by ShareX. Captures
go straight to the clipboard and can be uploaded to S3-compatible storage. It
lives in the system tray and uses KWin's own capture APIs.

%prep
%autosetup -n %{name}-%{version_no_tilde}

%build
%cmake
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop

%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}
%dir %{_libexecdir}/%{name}
%{_libexecdir}/%{name}/%{name}-recorder
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%{_datadir}/icons/hicolor/*/apps/%{name}.svg

%changelog
* Wed Oct 14 2026 almeidx - 0.1.1~alpha.1-1
- Update to 0.1.1-alpha.1

* Tue Oct 13 2026 almeidx - 0.1.0-1
- Initial package
