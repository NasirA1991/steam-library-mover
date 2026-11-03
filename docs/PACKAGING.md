# Packaging and distribution

Repository: <https://github.com/NasirA1991/steam-library-mover>

Release artifacts are built by GitHub Actions when a `v*` tag is pushed. The workflow is
[`.github/workflows/release.yml`](../.github/workflows/release.yml).

## Versioning

- `0.x` releases are public alpha snapshots.
- `1.0.0` requires the real-machine acceptance checks in [TESTING.md](TESTING.md).
- Tag releases as `vX.Y.Z`.
- Stable AUR package: tagged release. AUR `-git` package: development branch.

## Release assets

- `Steam_Library_Mover-x86_64.AppImage` — portable, built on Ubuntu 22.04 with Qt 6.8.
- `steam-library-mover-*.pkg.tar.zst` — Arch package built in an Arch container.
- `steam-library-mover-*.tar.gz` — source archive generated from the tag.
- `SHA256SUMS` — checksums for the release files.

The release is marked as a GitHub prerelease while the project remains `0.x`.

## AppImage

The build script is [`packaging/appimage/build-appimage.sh`](../packaging/appimage/build-appimage.sh).
It expects Qt 6.8 development files, `linuxdeploy`, `linuxdeploy-plugin-qt` and `appimagetool` in
`PATH`. CI installs them and runs an AppImage extraction smoke test.

For manual builds:

```bash
export QMAKE=/path/to/Qt/6.8.*/gcc_64/bin/qmake
export PATH=/path/to/Qt/6.8.*/gcc_64/bin:$PATH
export APPIMAGE_EXTRACT_AND_RUN=1
packaging/appimage/build-appimage.sh
```

Build against Qt 6.8 (not a distro Qt with unbundlable KDE image plugins). Test the result on a
clean KDE system and a non-KDE distribution. FUSE2 is required unless using
`APPIMAGE_EXTRACT_AND_RUN=1`.

## AUR

Recipes:

- [`packaging/aur/PKGBUILD`](../packaging/aur/PKGBUILD) — tagged source package.
- [`packaging/aur/PKGBUILD-git`](../packaging/aur/PKGBUILD-git) — VCS package.

Maintainer: **Nasir A <evilleader91@gmail.com>**.

The release workflow runs `updpkgsums` and builds the package in `archlinux:latest`. The repository
template keeps `sha256sums=('SKIP')` so local iteration and CI can regenerate the checksum from the
tagged source. Before uploading the stable recipe to AUR, run:

```bash
cd packaging/aur
updpkgsums
makepkg --nodeps --nocheck
namcap *.pkg.tar.zst
```

Publishing to AUR is a separate operation and requires an AUR account/SSH key; GitHub Actions only
builds and validates the package.

## Flatpak (gated, not shipped)

The Flatpak manifest remains in the repository for future work, but it is manual-only and is not part of a release. A
confined app cannot reliably inspect host Steam/Proton/Wine processes until `flatpak-spawn --host`
detection and shutdown are validated in a real sandbox. Until then, AppImage and Arch/AUR are the
supported release artifacts.

If that gate is passed:

```bash
flatpak-builder --user --install --force-clean build-flatpak \
  packaging/flatpak/io.github.nasira1991.SteamLibraryMover.yml
flatpak run io.github.nasira1991.SteamLibraryMover
```

Flathub submission also needs a tagged source (not `type: dir`), screenshots in the metainfo and
`flatpak-builder-lint`.

## Continuous integration

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs:

- GCC Debug build + full test suite + metadata validation on Ubuntu 24.04.
- Clang Release build + tests.
- Arch Linux Release build (tests run in the GCC/Clang jobs).

## Release checklist

- [ ] Version bumped in `CMakeLists.txt`, metainfo release entry and release notes.
- [ ] `ctest` green locally and in CI.
- [ ] Manual smoke test completed on a writable Steam library.
- [ ] AppImage extracted and launched.
- [ ] Arch package installed and launched.
- [ ] AUR checksums regenerated for the stable recipe.
- [ ] Flatpak gate explicitly recorded as not shipped.
- [ ] `SHA256SUMS` and release notes attached.
- [ ] No personal paths, tokens, build trees or screenshots containing private data are committed.
