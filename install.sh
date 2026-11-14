#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and install Steam Library Mover from this checkout.
# The build runs as the invoking user; sudo is used only for the install step
# when the selected prefix is not writable by that user.

set -euo pipefail

umask 022

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
readonly APP_NAME="steam-library-mover"
readonly BUILD_DIR="$SCRIPT_DIR/build"
readonly MANIFEST_REL="share/${APP_NAME}/install_manifest.txt"

MODE="system"
PREFIX_ARG=""
PREFIX=""
SKIP_DEPS=0
SKIP_TESTS=0
STAGE_DIR="${DESTDIR:-}"
MANIFEST_BACKUP=""
CMAKE_BIN=""

usage() {
    cat <<'EOF'
Usage: ./install.sh [OPTIONS]

Build Steam Library Mover from this checkout and install it.

Options:
  --user             Install to $HOME/.local (no sudo)
  --prefix PATH      Install to PATH (sudo only if PATH is not writable)
  --skip-deps        Skip the build-dependency check
  --skip-tests       Skip the test suite
  -h, --help         Show this help

Environment:
  DESTDIR            Stage files under DESTDIR instead of a live install.
                     No sudo, manifest copy, or cache refresh is performed.

The default prefix is /usr/local. The source tree is never installed; it is
built in ./build and the installed files are recorded in
PREFIX/share/steam-library-mover/install_manifest.txt for uninstall.sh.
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
        --skip-deps)
            SKIP_DEPS=1
            shift
            ;;
        --skip-tests)
            SKIP_TESTS=1
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
    die "Run as your normal user; the script will ask for sudo only for the install step."
fi

case "$MODE" in
    user)
        [[ -n "${HOME:-}" ]] || die "HOME is not set"
        PREFIX="$HOME/.local"
        ;;
    prefix)
        [[ -n "$PREFIX_ARG" ]] || die "--prefix requires a path"
        [[ "$PREFIX_ARG" == /* ]] || die "--prefix must be an absolute path"
        [[ "$PREFIX_ARG" != "/" ]] || die "refusing to install directly into /"
        PREFIX="$PREFIX_ARG"
        ;;
    system)
        PREFIX="/usr/local"
        ;;
esac

while [[ "$PREFIX" == */ && "$PREFIX" != "/" ]]; do
    PREFIX="${PREFIX%/}"
done

if [[ -n "$STAGE_DIR" ]]; then
    [[ "$STAGE_DIR" == /* ]] || die "DESTDIR must be an absolute path"
fi

# A prefix is writable when its nearest existing parent is writable.
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
if [[ -z "$STAGE_DIR" ]] && ! prefix_is_writable; then
    NEED_SUDO=1
    command -v sudo >/dev/null 2>&1 || die "sudo is required to install into $PREFIX"
fi

run_privileged() {
    if ((NEED_SUDO == 1)); then
        sudo "$@"
    else
        "$@"
    fi
}

find_compiler() {
    local candidate
    if [[ -n "${CXX:-}" ]]; then
        command -v "$CXX" >/dev/null 2>&1 || return 1
        printf '%s\n' "$CXX"
        return 0
    fi
    for candidate in c++ g++ clang++; do
        if command -v "$candidate" >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

read_os_release() {
    OS_ID=""
    OS_LIKE=""
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        set +u
        # shellcheck source=/dev/null
        . /etc/os-release
        set -u
        OS_ID="${ID:-}"
        OS_LIKE="${ID_LIKE:-}"
    fi
}

distro_family() {
    case " ${OS_ID} ${OS_LIKE} " in
        *arch*|*cachyos*|*endeavouros*|*manjaro*) printf 'arch\n' ;;
        *debian*|*ubuntu*|*linuxmint*|*pop*) printf 'debian\n' ;;
        *fedora*) printf 'fedora\n' ;;
        *suse*) printf 'suse\n' ;;
        *) printf 'unknown\n' ;;
    esac
}

print_dependency_help() {
    local family
    read_os_release
    family="$(distro_family)"

    cat >&2 <<EOF

Missing build dependencies for Steam Library Mover.
Detected distribution: ${OS_ID:-unknown} ${OS_LIKE:+(ID_LIKE: $OS_LIKE)}

Install the required tools and Qt 6 development modules with one of:
EOF

    case "$family" in
        arch)
            cat >&2 <<'EOF'
  Arch / CachyOS / EndeavourOS:
    sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-svg
EOF
            ;;
        debian)
            cat >&2 <<'EOF'
  Debian / Ubuntu:
    sudo apt update && sudo apt install -y build-essential cmake ninja-build qt6-base-dev qt6-svg-dev libgl1-mesa-dev
EOF
            ;;
        fedora)
            cat >&2 <<'EOF'
  Fedora:
    sudo dnf install -y gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel mesa-libGL-devel
EOF
            ;;
        suse)
            cat >&2 <<'EOF'
  openSUSE:
    sudo zypper install -y gcc-c++ cmake ninja qt6-base-devel qt6-svg-devel Mesa-libGL-devel
EOF
            ;;
        *)
            cat >&2 <<'EOF'
  Arch / CachyOS / EndeavourOS:
    sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-svg

  Debian / Ubuntu:
    sudo apt update && sudo apt install -y build-essential cmake ninja-build qt6-base-dev qt6-svg-dev libgl1-mesa-dev

  Fedora:
    sudo dnf install -y gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel mesa-libGL-devel

  openSUSE:
    sudo zypper install -y gcc-c++ cmake ninja qt6-base-devel qt6-svg-devel Mesa-libGL-devel
EOF
            ;;
    esac

    cat >&2 <<'EOF'

The Qt 6 modules required by this project are Core, Gui, Widgets, Svg,
Network, DBus and Test, as declared by find_package() in CMakeLists.txt.
EOF
}

check_core_tools() {
    CMAKE_BIN="$(command -v cmake || true)"
    if [[ -z "$CMAKE_BIN" ]]; then
        print_dependency_help
        die "cmake is not installed"
    fi

    if ! COMPILER="$(find_compiler)"; then
        print_dependency_help
        die "no C++ compiler found (set CXX or install g++/clang++)"
    fi
    if command -v ninja >/dev/null 2>&1; then
        GENERATOR="Ninja"
    else
        command -v make >/dev/null 2>&1 || {
            print_dependency_help
            die "neither ninja nor make is installed"
        }
        GENERATOR="Unix Makefiles"
    fi
}

check_qt_dependencies() {
    local probe_dir
    probe_dir="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf -- '$probe_dir'" EXIT

    cat > "$probe_dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(SlmInstallProbe LANGUAGES CXX)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Svg Network DBus Test)
EOF

    if ! "$CMAKE_BIN" -S "$probe_dir" -B "$probe_dir/build" \
            -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release \
            >"$probe_dir/probe.log" 2>&1; then
        cat "$probe_dir/probe.log" >&2
        print_dependency_help
        die "Qt 6 development modules are missing or too old"
    fi

    rm -rf -- "$probe_dir"
    trap - EXIT
}

warn_if_aur_package_installed() {
    command -v pacman >/dev/null 2>&1 || return 0
    local pkg
    for pkg in steam-library-mover steam-library-mover-bin; do
        if pacman -Qq "$pkg" >/dev/null 2>&1; then
            warn "$pkg is installed by pacman; an install in $PREFIX can shadow it in PATH."
            warn "Remove the package first if you want to avoid two copies of $APP_NAME."
        fi
    done
}

warn_if_user_bin_missing() {
    [[ "$MODE" == "user" ]] || return 0
    [[ "$PREFIX" == "$HOME/.local" ]] || return 0
    case ":$PATH:" in
        *":$HOME/.local/bin:"*) ;;
        *)
            warn "$HOME/.local/bin is not on PATH."
            warn "Add: export PATH=\"\$HOME/.local/bin:\$PATH\""
            ;;
    esac
}

refresh_caches() {
    [[ -z "$STAGE_DIR" ]] || return 0

    # Desktop caches only make sense for prefixes that participate in the
    # standard XDG data paths. A temporary/custom prefix is not in
    # XDG_DATA_DIRS, and writing caches there would leave unmanaged files
    # behind after uninstall.
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

prepare_manifest_backup() {
    local destination="$PREFIX/$MANIFEST_REL"

    if [[ -f "$destination" ]]; then
        MANIFEST_BACKUP="$(mktemp)"
        if ! run_privileged cat -- "$destination" > "$MANIFEST_BACKUP"; then
            rm -f -- "$MANIFEST_BACKUP"
            MANIFEST_BACKUP=""
            die "could not read the existing install manifest: $destination"
        fi
    fi
}

write_install_manifest() {
    local manifest="$BUILD_DIR/install_manifest.txt"
    local destination="$PREFIX/$MANIFEST_REL"
    local old_sorted new_sorted merged old_only

    [[ -f "$manifest" ]] || die "CMake did not create $manifest"
    run_privileged mkdir -p -- "$PREFIX/share/$APP_NAME"

    old_sorted="$(mktemp)"
    new_sorted="$(mktemp)"
    merged="$(mktemp)"
    if [[ -n "$MANIFEST_BACKUP" && -f "$MANIFEST_BACKUP" ]]; then
        awk 'NF' "$MANIFEST_BACKUP" | LC_ALL=C sort -u > "$old_sorted"
        awk 'NF' "$manifest" | LC_ALL=C sort -u > "$new_sorted"
        LC_ALL=C sort -u "$old_sorted" "$new_sorted" > "$merged"
        old_only="$(comm -23 "$old_sorted" "$new_sorted" | wc -l)"
        old_only="${old_only//[[:space:]]/}"
    else
        awk 'NF' "$manifest" | LC_ALL=C sort -u > "$merged"
        old_only=0
    fi

    run_privileged install -m 0644 -- "$merged" "$destination"
    rm -f -- "$old_sorted" "$new_sorted" "$merged" "$MANIFEST_BACKUP"
    MANIFEST_BACKUP=""
    printf '  manifest: %s (%s previous-only entr%s carried over)\n' \
        "$destination" "$old_only" "$([[ "$old_only" == 1 ]] && printf y || printf ies)"
}

if ((SKIP_DEPS == 0)); then
    check_core_tools
    check_qt_dependencies
else
    CMAKE_BIN="$(command -v cmake || true)"
    [[ -n "$CMAKE_BIN" ]] || die "cmake is not installed"
    if command -v ninja >/dev/null 2>&1; then
        GENERATOR="Ninja"
    else
        GENERATOR="Unix Makefiles"
    fi
    COMPILER="${CXX:-c++}"
fi

warn_if_aur_package_installed
warn_if_user_bin_missing

printf 'Building Steam Library Mover in %s\n' "$BUILD_DIR"
printf '  compiler: %s\n' "$COMPILER"
printf '  generator: %s\n' "$GENERATOR"

"$CMAKE_BIN" -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS="OWNER_READ;OWNER_WRITE;OWNER_EXECUTE;GROUP_READ;GROUP_EXECUTE;WORLD_READ;WORLD_EXECUTE"
"$CMAKE_BIN" --build "$BUILD_DIR" --parallel

if ((SKIP_TESTS == 0)) && [[ -f "$SCRIPT_DIR/tests/CMakeLists.txt" ]]; then
    printf 'Running the test suite\n'
    ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 60
fi

if [[ -n "$STAGE_DIR" ]]; then
    # A manifest from an earlier install can list a different prefix; never
    # copy it into a new staging tree.
    rm -f -- "$BUILD_DIR/install_manifest.txt"
    printf 'Staging into DESTDIR=%s\n' "$STAGE_DIR"
    DESTDIR="$STAGE_DIR" "$CMAKE_BIN" --install "$BUILD_DIR"
    printf '\nStaged Steam Library Mover under %s with prefix %s.\n' \
        "$STAGE_DIR" "$PREFIX"
    printf 'No manifest or cache refresh was added to the staging tree.\n'
    exit 0
fi

prepare_manifest_backup

# A previous sudo or DESTDIR install may have left a root-owned or stale
# manifest here. The build dir is ours, so we can always unlink it.
rm -f -- "$BUILD_DIR/install_manifest.txt"

printf 'Installing into %s\n' "$PREFIX"
if ((NEED_SUDO == 1)); then
    sudo "$CMAKE_BIN" --install "$BUILD_DIR"
    sudo chown "$(id -u):$(id -g)" -- "$BUILD_DIR/install_manifest.txt"
else
    "$CMAKE_BIN" --install "$BUILD_DIR"
fi

write_install_manifest
refresh_caches

UNINSTALL_ARGS=()
case "$MODE" in
    user) UNINSTALL_ARGS=(--user) ;;
    prefix) UNINSTALL_ARGS=(--prefix "$PREFIX") ;;
    system) ;;
esac

printf '\nInstalled Steam Library Mover.\n'
printf '  GUI:      %s/bin/%s\n' "$PREFIX" "$APP_NAME"
printf '  CLI:      %s/bin/%s-cli\n' "$PREFIX" "$APP_NAME"
printf '  Manifest: %s\n' "$PREFIX/$MANIFEST_REL"
printf '  Uninstall: %q' "$SCRIPT_DIR/uninstall.sh"
if ((${#UNINSTALL_ARGS[@]} > 0)); then
    printf ' %q' "${UNINSTALL_ARGS[@]}"
fi
printf '\n\nApplication data is separate; uninstall.sh prints its locations.\n'
