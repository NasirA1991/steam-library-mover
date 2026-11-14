#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Remove a Steam Library Mover install recorded by install.sh.
# Only files named in the install manifest are removed.

set -euo pipefail

readonly APP_NAME="steam-library-mover"
readonly MANIFEST_REL="share/${APP_NAME}/install_manifest.txt"

MODE="system"
PREFIX_ARG=""
PREFIX=""

usage() {
    cat <<'EOF'
Usage: ./uninstall.sh [OPTIONS]

Remove a Steam Library Mover installation recorded by install.sh.
Only files listed in PREFIX/share/steam-library-mover/install_manifest.txt
are removed; user settings, journals and caches are left untouched.

Options:
  --user             Remove the $HOME/.local installation (no sudo)
  --prefix PATH      Remove the installation under PATH
  -h, --help         Show this help

The default prefix is /usr/local. Run install.sh with the same option used
when the application was installed.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

while (($# > 0)); do
    case "$1" in
        --user)
            MODE="user"
            shift
            ;;
        --prefix)
            (($# >= 2)) || die "--prefix requires a path"
            MODE="prefix"
            PREFIX_ARG="$2"
            shift 2
            ;;
        --prefix=*)
            MODE="prefix"
            PREFIX_ARG="${1#--prefix=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            die "unknown option: $1 (use --help)"
            ;;
    esac
done

(($# == 0)) || die "unexpected argument: $1"

if ((EUID == 0)); then
    die "Run as your normal user; the script will ask for sudo only when it needs to remove system files."
fi

case "$MODE" in
    user)
        [[ -n "${HOME:-}" ]] || die "HOME is not set"
        PREFIX="$HOME/.local"
        ;;
    prefix)
        [[ -n "$PREFIX_ARG" ]] || die "--prefix requires a path"
        [[ "$PREFIX_ARG" == /* ]] || die "--prefix must be an absolute path"
        [[ "$PREFIX_ARG" != "/" ]] || die "refusing to use / as an install prefix"
        PREFIX="$PREFIX_ARG"
        ;;
    system)
        PREFIX="/usr/local"
        ;;
esac

while [[ "$PREFIX" == */ && "$PREFIX" != "/" ]]; do
    PREFIX="${PREFIX%/}"
done

prefix_is_writable() {
    local dir="$PREFIX"
    local parent

    while [[ ! -e "$dir" ]]; do
        parent="$(dirname -- "$dir")"
        [[ "$parent" == "$dir" ]] && break
        dir="$parent"
    done
    [[ -w "$dir" ]]
}

NEED_SUDO=0
if ! prefix_is_writable; then
    NEED_SUDO=1
    command -v sudo >/dev/null 2>&1 || die "sudo is required to remove system files under $PREFIX"
fi

run_privileged() {
    if ((NEED_SUDO == 1)); then
        sudo "$@"
    else
        "$@"
    fi
}

MANIFEST="$PREFIX/$MANIFEST_REL"
if [[ ! -f "$MANIFEST" ]]; then
    cat >&2 <<EOF
error: install manifest not found: $MANIFEST

This uninstaller only removes installations recorded by install.sh.
For a package-manager install, use the package manager:
  Arch-based: sudo pacman -Rns steam-library-mover
  AppImage:   delete the AppImage file
For a manual CMake install, inspect the install log or build directory and
remove the files you installed by hand. This script will not guess paths.
EOF
    exit 1
fi

removed=0
failures=0

while IFS= read -r path || [[ -n "$path" ]]; do
    [[ -n "$path" ]] || continue

    case "$path" in
        *"/../"*|*"/./"*|*"/..")
            warn "skipping unsafe manifest entry: $path"
            failures=1
            continue
            ;;
    esac

    case "$path" in
        "$PREFIX"/*) ;;
        *)
            warn "skipping path outside $PREFIX: $path"
            failures=1
            continue
            ;;
    esac

    # A missing file is already uninstalled; do not treat it as an error.
    if [[ ! -e "$path" && ! -L "$path" ]]; then
        continue
    fi

    if [[ -d "$path" && ! -L "$path" ]]; then
        warn "skipping directory listed in manifest: $path"
        failures=1
        continue
    fi

    if run_privileged rm -f -- "$path"; then
        removed=$((removed + 1))
    else
        failures=1
    fi
done < "$MANIFEST"

if ((failures != 0)); then
    cat >&2 <<EOF
Some files could not be removed or an unsafe manifest entry was skipped.
The install manifest was kept at: $MANIFEST
Fix the problem and run this script again.
EOF
    exit 1
fi

if ! run_privileged rm -f -- "$MANIFEST"; then
    printf 'error: could not remove %s\n' "$MANIFEST" >&2
    exit 1
fi

# Remove only empty directories inside the two app-specific trees, deepest
# first. find does not follow symlinks here. A repeated pass is needed because
# a parent is only observed as empty after its children have been removed.
remove_empty_app_tree() {
    local dir="$1"
    local removed child
    while [[ -d "$dir" ]]; do
        removed=0
        while IFS= read -r -d '' child; do
            if run_privileged rmdir -- "$child" 2>/dev/null; then
                removed=1
            fi
        done < <(find "$dir" -depth -type d -empty -print0 2>/dev/null)
        ((removed == 1)) || break
    done
}

remove_empty_app_tree "$PREFIX/share/$APP_NAME"
remove_empty_app_tree "$PREFIX/share/doc/$APP_NAME"

refresh_caches() {
    case "$PREFIX" in
        /usr|/usr/local|"$HOME"/.local) ;;
        *) return 0 ;;
    esac

    if command -v update-desktop-database >/dev/null 2>&1; then
        run_privileged update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
    fi
    if [[ -f "$PREFIX/share/icons/hicolor/icon-theme.cache" ]] && \
            command -v gtk-update-icon-cache >/dev/null 2>&1; then
        run_privileged gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
    fi
    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        kbuildsycoca6 >/dev/null 2>&1 || true
    fi
}

refresh_caches

CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"

printf '\nRemoved %d installed file(s) from %s.\n' "$removed" "$PREFIX"
printf 'The install manifest was removed.\n'
cat <<EOF

Application data was not removed. To delete it manually:
  Settings:  $CONFIG_HOME/SteamLibraryMover/steam-library-mover.conf
  Journals, logs and backups:
             $STATE_HOME/steam-library-mover
  Scan cache: $CACHE_HOME/SteamLibraryMover/steam-library-mover
  Artwork:   $CACHE_HOME/steam-library-mover/artwork

These defaults can be overridden by XDG_* variables, SLM_STATE_DIR,
SLM_CACHE_DIR and SLM_ARTWORK_DIR. Steam game libraries, saves and Proton
prefixes are never touched by this script.
EOF
