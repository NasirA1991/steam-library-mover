# Installation

This page covers supported installation methods and building from source. For a quick install, see
the [README installation section](../README.md#installation).

## Supported methods

| Method | Best for | Notes |
| --- | --- | --- |
| AUR package | Arch Linux, CachyOS, EndeavourOS | Recommended on Arch-based distributions; builds from the tagged source and also installs the CLI |
| AUR prebuilt package | Arch Linux, CachyOS, EndeavourOS | Installs the GitHub Actions-built Arch package; conflicts with the source package |
| AppImage | Other distributions | Portable, x86_64, no system Qt installation required |
| Source build | Development or unsupported distributions | Requires Qt 6.4+ and a C++20 compiler |
| Flatpak | Not currently offered | Experimental CI artifact only; see [PACKAGING.md](PACKAGING.md#flatpak-experimental-actions-artifact) |

Steam Library Mover is a Linux application. Windows and macOS are not supported.

## Requirements

### Runtime

- Linux x86_64 with Steam installed.
- A destination Steam library that already exists and is registered in Steam.
- Write access to the source and destination libraries, and enough free space for the copy.
- Steam and any Proton/Wine games closed before a move.

The application does not create a Steam library, add a drive entry, or install a game. Create and
register the destination library in Steam first.

### Build from source

- CMake 3.21 or newer.
- Ninja (recommended; Makefiles also work).
- A C++20 compiler (GCC or Clang).
- Qt 6.4 or newer with the Core, Gui, Widgets, Svg, Network, DBus and Test modules.
- Git if you are cloning the repository.

The CI jobs use the packages below. Other distributions need equivalent Qt 6 development packages.

**Arch Linux:**

```bash
sudo pacman -S --needed base-devel cmake ninja git qt6-base qt6-svg
```

**Debian/Ubuntu:**

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git \
  qt6-base-dev qt6-svg-dev libgl1-mesa-dev
```

CI additionally installs `clang`, `desktop-file-utils` and `appstream` for the Clang and metadata
validation jobs.

## AUR

Install with any AUR helper:

```bash
yay -S steam-library-mover
# or: paru -S steam-library-mover
# or: pikaur -S steam-library-mover
```

The package installs the desktop application, `steam-library-mover-cli`, the desktop entry, icon,
AppStream metadata and man page.

If you prefer not to compile during installation, use the prebuilt package:

```bash
yay -S steam-library-mover-bin
# or: paru -S steam-library-mover-bin
```

It installs the same payload from the GitHub release archive built by GitHub Actions. It
`provides`/`conflicts` with `steam-library-mover`; remove one before installing the other.

Uninstall with:

```bash
sudo pacman -Rns steam-library-mover
# or: sudo pacman -Rns steam-library-mover-bin
```

## AppImage

```bash
curl -LO https://github.com/NasirA1991/steam-library-mover/releases/latest/download/Steam_Library_Mover-x86_64.AppImage
chmod +x Steam_Library_Mover-x86_64.AppImage
./Steam_Library_Mover-x86_64.AppImage
```

The AppImage is self-contained and does not install files system-wide. It normally needs FUSE 2.
If FUSE is unavailable, run it in extraction mode:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./Steam_Library_Mover-x86_64.AppImage
```

Release checksums and build-provenance verification are described in
[PACKAGING.md](PACKAGING.md#supply-chain). Remove the AppImage file to uninstall it.

## Build from source

```bash
git clone https://github.com/NasirA1991/steam-library-mover.git
cd steam-library-mover
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 60
sudo cmake --install build
```

The default install prefix is `/usr/local`, so the binaries are installed as
`/usr/local/bin/steam-library-mover` and `/usr/local/bin/steam-library-mover-cli`. Use
`-DCMAKE_INSTALL_PREFIX=/usr` when building a distribution package instead.

Do not run the test suite as root. Some tests skip when they detect root, and a move is refused
when the application runs as root.

### Uninstall a source install

CMake does not provide an uninstall target. For the default prefix, remove the files installed by
the command above:

```text
/usr/local/bin/steam-library-mover
/usr/local/bin/steam-library-mover-cli
/usr/local/share/applications/steam-library-mover.desktop
/usr/local/share/icons/hicolor/scalable/apps/steam-library-mover.svg
/usr/local/share/metainfo/io.github.nasira1991.SteamLibraryMover.metainfo.xml
/usr/local/share/man/man1/steam-library-mover.1
/usr/local/share/doc/steam-library-mover/
```

Review the output of `sudo cmake --install build --verbose` before deleting files if you changed the
install prefix or used a package manager.

## Steam installation

| Steam type | Support |
| --- | --- |
| Native Steam | Supported |
| Flatpak Steam | Supported when Steam Library Mover runs natively (AppImage, AUR or source build) |
| Snap Steam | Detected but moves are not supported |
| Custom Steam root | Selectable in **Settings → Steam root override** |

Steam Library Mover reads the selected Steam installation and its `libraryfolders.vdf`. It does not
read Steam credentials or cloud-save data.

## Next steps

- [Move a game](../README.md#how-to-move-a-game)
- [Command-line reference](CLI.md)
- [Safety model](SAFETY.md)
- [NTFS libraries](NTFS.md)
