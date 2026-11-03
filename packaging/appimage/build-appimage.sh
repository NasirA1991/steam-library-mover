#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build an AppImage. Run on Ubuntu 22.04 (glibc 2.35) with a Qt 6.8+ toolchain,
# linuxdeploy and linuxdeploy-plugin-qt in PATH.
#
#   export QMAKE=/path/to/Qt/6.8.*/gcc_64/bin/qmake
#   export PATH=/path/to/Qt/6.8.*/gcc_64/bin:$PATH
#   packaging/appimage/build-appimage.sh
#
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR="$ROOT/build-appimage"
APPDIR="$ROOT/AppDir"

rm -rf "$BUILD_DIR" "$APPDIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD_DIR" --parallel
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

# The runtime mounts the AppImage with FUSE2; provide the standard fallback docs.
ln -sf usr/share/applications/steam-library-mover.desktop "$APPDIR/steam-library-mover.desktop"
ln -sf usr/share/icons/hicolor/scalable/apps/steam-library-mover.svg "$APPDIR/steam-library-mover.svg"

linuxdeploy \
    --appdir "$APPDIR" \
    --plugin qt \
    --desktop-file "$APPDIR/steam-library-mover.desktop" \
    --icon-file "$APPDIR/steam-library-mover.svg" \
    --output appimage

ls -lh "$ROOT"/Steam_Library_Mover*.AppImage
