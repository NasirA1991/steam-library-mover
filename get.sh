#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One-line installer for Steam Library Mover.
#
#   curl -fsSL https://raw.githubusercontent.com/NasirA1991/steam-library-mover/main/get.sh | bash
#
# The script is wrapped in main() so a truncated download cannot execute a
# partial installer. Every release download is checked against SHA256SUMS
# before it is extracted or run.

set -euo pipefail
umask 022

readonly REPO="NasirA1991/steam-library-mover"
readonly APP_NAME="steam-library-mover"
readonly APP_STATE_NAME="steam-library-mover"
readonly AUR_PACKAGE="steam-library-mover"
readonly APPIMAGE_ASSET="Steam_Library_Mover-x86_64.AppImage"
readonly MIN_SOURCE_VERSION="v0.2.1"
RELEASES_BASE="${GET_SH_BASE_URL:-https://github.com/${REPO}/releases}"
RELEASES_BASE="${RELEASES_BASE%/}"

METHOD="auto"
METHOD_DISPLAY=""
VERSION=""
OPT_VERSION=""
USER_MODE=0
ASSUME_YES=0
DRY_RUN=0
UNINSTALL=0
HELPER=""

FAMILY=""
OS_ID=""
OS_LIKE=""
OS_VERSION=""
TMP_DIR=""
STATE_DIR=""
STATE_FILE=""
STATE_METHOD=""
STATE_VERSION=""
STATE_PACKAGE=""
STATE_USER=""
STATE_APPIMAGE=""
STATE_WRAPPER=""
STATE_DESKTOP=""
STATE_ICON=""
INSTALLED_MANAGER=""
INSTALLED_PACKAGE=""

usage() {
    cat <<'EOF'
Usage: get.sh [OPTIONS]

Install Steam Library Mover. On Arch-based systems this uses the AUR. On other
x86_64 distributions it installs a verified AppImage under ~/.local. Pass
options through a pipe with: curl ... | bash -s -- --user

Options:
  --user             Per-user install (AppImage or source). No sudo.
  --method METHOD    auto, aur, appimage, or source (default: auto)
  --version VERSION  Pin a release, e.g. v0.2.0 (default: latest)
  --yes              Skip the confirmation prompt
  --dry-run          Print the commands without running them
  --uninstall        Undo the method recorded by a previous get.sh install
  -h, --help         Show this help

The AUR package is steam-library-mover. The prebuilt steam-library-mover-bin
package is a faster AUR alternative if you do not want to compile locally.

The AppImage method installs the GUI only. Use the AUR or --method source when
you also need the steam-library-mover-cli command.

For testing only, GET_SH_BASE_URL overrides the release download base URL
(the part before /latest/download or /download/<tag>). Do not use it for a
normal install.
EOF
}

die() {
    printf 'get.sh: %s\n' "$*" >&2
    exit 1
}

warn() {
    printf 'get.sh: warning: %s\n' "$*" >&2
}

usage_error() {
    printf 'get.sh: %s\n\n' "$*" >&2
    usage >&2
    exit 2
}

parse_args() {
    while (($#)); do
        case "$1" in
            --user)
                USER_MODE=1
                ;;
            --method)
                [[ $# -ge 2 ]] || usage_error "--method needs a value"
                METHOD="$2"
                shift
                ;;
            --method=*)
                METHOD="${1#--method=}"
                ;;
            --version)
                [[ $# -ge 2 ]] || usage_error "--version needs a value"
                OPT_VERSION="$2"
                shift
                ;;
            --version=*)
                OPT_VERSION="${1#--version=}"
                ;;
            --yes)
                ASSUME_YES=1
                ;;
            --dry-run)
                DRY_RUN=1
                ;;
            --uninstall)
                UNINSTALL=1
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            --)
                shift
                break
                ;;
            -*)
                usage_error "unknown option: $1"
                ;;
            *)
                usage_error "unexpected argument: $1"
                ;;
        esac
        shift
    done

    if (($#)); then
        usage_error "unexpected argument: $1"
    fi

    case "$METHOD" in
        auto|aur|appimage|source) ;;
        *) usage_error "unknown method: $METHOD (expected auto, aur, appimage or source)" ;;
    esac

    if [[ "$METHOD" == "aur" && "$USER_MODE" == "1" ]]; then
        usage_error "--user is not supported with --method aur; use --method appimage or source"
    fi
}

parse_os_release() {
    [[ -r /etc/os-release ]] || return 0
    local key value
    while IFS='=' read -r key value; do
        value="${value%$'\r'}"
        case "$value" in
            \"*\") value="${value#\"}"; value="${value%\"}" ;;
            \'*\') value="${value#\'}"; value="${value%\'}" ;;
        esac
        case "$key" in
            ID) OS_ID="$value" ;;
            ID_LIKE) OS_LIKE="$value" ;;
            VERSION_ID) OS_VERSION="$value" ;;
        esac
    done < /etc/os-release

    case "$OS_ID $OS_LIKE" in
        *arch*|*manjaro*|*endeavouros*|*cachyos*) FAMILY="arch" ;;
        *debian*|*ubuntu*|*mint*) FAMILY="debian" ;;
        *fedora*|*rhel*|*centos*) FAMILY="fedora" ;;
        *suse*) FAMILY="suse" ;;
        *) FAMILY="unknown" ;;
    esac
}

is_arch() {
    [[ "$FAMILY" == "arch" ]]
}

is_x86_64() {
    [[ "$(uname -m)" == "x86_64" ]]
}

tool_install_hint() {
    case "$FAMILY" in
        arch) printf '  sudo pacman -S --needed curl tar coreutils\n' ;;
        debian) printf '  sudo apt update && sudo apt install -y curl tar coreutils\n' ;;
        fedora) printf '  sudo dnf install -y curl tar coreutils\n' ;;
        suse) printf '  sudo zypper install -y curl tar coreutils\n' ;;
        *) printf '  install curl (or wget), tar and coreutils with your package manager\n' ;;
    esac
}

ensure_tools() {
    local missing=()
    command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || missing+=("curl or wget")
    command -v tar >/dev/null 2>&1 || missing+=("tar")
    command -v sha256sum >/dev/null 2>&1 || missing+=("sha256sum")

    if ((${#missing[@]})); then
        printf 'get.sh: missing required tools: %s\n' "${missing[*]}" >&2
        printf 'Install them for your distribution:\n' >&2
        tool_install_hint >&2
        exit 1
    fi
}

require_sudo() {
    command -v sudo >/dev/null 2>&1 || die "sudo is required for this method but was not found"
}

need_root_tty() {
    if ! { true >/dev/tty; } 2>/dev/null; then
        die "no terminal is available for confirmation; re-run with --yes"
    fi
}

confirm() {
    if ((ASSUME_YES)); then
        return 0
    fi
    need_root_tty
    printf '%s [y/N] ' "$1" > /dev/tty
    local answer
    IFS= read -r answer < /dev/tty || die "could not read confirmation"
    case "$answer" in
        y|Y|yes|YES) ;;
        *) printf 'Aborted.\n'; exit 1 ;;
    esac
}

normalize_version() {
    local value="$1"
    case "$value" in
        v*) printf '%s\n' "$value" ;;
        *) printf 'v%s\n' "$value" ;;
    esac
}

version_ge() {
    local oldest="$1" candidate="$2"
    [[ "$(printf '%s\n%s\n' "$oldest" "$candidate" | sort -V | tail -n1)" == "$candidate" ]]
}

asset_url() {
    local tag="$1" asset="$2"
    if [[ -n "$tag" && "$tag" != "latest" ]]; then
        printf '%s/download/%s/%s\n' "$RELEASES_BASE" "$tag" "$asset"
    else
        printf '%s/latest/download/%s\n' "$RELEASES_BASE" "$asset"
    fi
}

resolve_latest_version() {
    command -v curl >/dev/null 2>&1 || die "resolving the latest release needs curl; re-run with --version vX.Y.Z"

    local effective tag body
    effective="$(curl -fsSL -o /dev/null -w '%{url_effective}' "${RELEASES_BASE}/latest" 2>/dev/null || true)"
    tag="${effective##*/}"
    if [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        printf '%s\n' "$tag"
        return 0
    fi

    body="$(curl -fsSL -H 'Accept: application/vnd.github+json' \
        -H 'User-Agent: steam-library-mover-get-sh' \
        "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null || true)"
    tag="$(printf '%s\n' "$body" | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
    if [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        printf '%s\n' "$tag"
        return 0
    fi

    die "could not determine the latest release (network error or GitHub API rate limit); re-run with --version vX.Y.Z"
}

fetch() {
    local url="$1" destination="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 2 --connect-timeout 20 -o "$destination" "$url"
    else
        wget -q -O "$destination" "$url"
    fi
}

download_verified() {
    local asset="$1" destination="$2"
    local sums="$TMP_DIR/SHA256SUMS"
    local expected actual

    mkdir -p "$TMP_DIR"
    fetch "$(asset_url "$VERSION" "$asset")" "$destination" \
        || die "download failed: $asset"
    fetch "$(asset_url "$VERSION" "SHA256SUMS")" "$sums" \
        || die "download failed: SHA256SUMS"

    expected="$(awk -v name="$asset" '$2 == name || $2 == "*" name {print $1; exit}' "$sums")"
    [[ -n "$expected" ]] || die "SHA256SUMS does not contain $asset"

    actual="$(sha256sum "$destination" | awk '{print $1}')"
    if [[ "$actual" != "$expected" ]]; then
        die "checksum mismatch for $asset (expected $expected, got $actual)"
    fi
}

write_state() {
    local method="$1"
    shift
    mkdir -p "$STATE_DIR"
    {
        printf 'method=%s\n' "$method"
        local line
        for line in "$@"; do
            printf '%s\n' "$line"
        done
    } > "$STATE_FILE"
    chmod 600 "$STATE_FILE"
}

read_state() {
    STATE_METHOD=""
    STATE_VERSION=""
    STATE_PACKAGE=""
    STATE_USER=""
    STATE_APPIMAGE=""
    STATE_WRAPPER=""
    STATE_DESKTOP=""
    STATE_ICON=""
    [[ -f "$STATE_FILE" ]] || return 1

    local key value
    while IFS='=' read -r key value; do
        value="${value%$'\r'}"
        case "$key" in
            method) STATE_METHOD="$value" ;;
            version) STATE_VERSION="$value" ;;
            package) STATE_PACKAGE="$value" ;;
            user) STATE_USER="$value" ;;
            appimage) STATE_APPIMAGE="$value" ;;
            wrapper) STATE_WRAPPER="$value" ;;
            desktop) STATE_DESKTOP="$value" ;;
            icon) STATE_ICON="$value" ;;
        esac
    done < "$STATE_FILE"
    [[ -n "$STATE_METHOD" ]]
}

find_installed_package() {
    local main_bin
    main_bin="$(command -v "$APP_NAME" 2>/dev/null || true)"
    [[ -n "$main_bin" ]] || return 1

    if command -v pacman >/dev/null 2>&1 && pacman -Qqo "$main_bin" >/dev/null 2>&1; then
        INSTALLED_MANAGER="pacman"
        INSTALLED_PACKAGE="$(pacman -Qqo "$main_bin" | head -n1)"
        return 0
    fi
    if command -v dpkg >/dev/null 2>&1 && dpkg -S "$main_bin" >/dev/null 2>&1; then
        INSTALLED_MANAGER="dpkg"
        INSTALLED_PACKAGE="$(dpkg -S "$main_bin" 2>/dev/null | sed 's/: .*//' | head -n1)"
        return 0
    fi
    if command -v rpm >/dev/null 2>&1 && rpm -qf "$main_bin" >/dev/null 2>&1; then
        INSTALLED_MANAGER="rpm"
        INSTALLED_PACKAGE="$(rpm -qf "$main_bin" 2>/dev/null | head -n1)"
        return 0
    fi
    return 1
}

find_helper() {
    local name
    for name in paru yay; do
        if command -v "$name" >/dev/null 2>&1; then
            printf '%s\n' "$name"
            return 0
        fi
    done
    return 1
}

has_base_devel() {
    command -v git >/dev/null 2>&1 \
        && command -v makepkg >/dev/null 2>&1 \
        && pacman -Qg base-devel >/dev/null 2>&1
}

manual_aur_steps() {
    printf 'Install it from the AUR manually:\n'
    printf '  git clone https://aur.archlinux.org/%s.git\n' "$AUR_PACKAGE"
    printf '  cd %s\n' "$AUR_PACKAGE"
    printf '  makepkg -si\n\n'
    printf 'Or use the prebuilt AUR package steam-library-mover-bin instead.\n'
    printf 'Then re-run get.sh --uninstall if you need to remove it.\n'
}

choose_method() {
    if [[ "$METHOD" == "auto" && "$USER_MODE" == "1" ]]; then
        METHOD="appimage"
        return 0
    fi

    if [[ "$METHOD" == "aur" ]]; then
        is_arch || die "the AUR method requires an Arch-based distribution"
        if [[ "$USER_MODE" == "1" ]]; then
            die "--user is not supported with --method aur; use --method appimage or source"
        fi
        HELPER="$(find_helper || true)"
        if [[ -n "$HELPER" ]]; then
            METHOD="aur-helper"
        elif has_base_devel; then
            METHOD="aur-makepkg"
        else
            METHOD="aur-manual"
        fi
        return 0
    fi

    if [[ "$METHOD" != "auto" ]]; then
        return 0
    fi

    if is_arch; then
        HELPER="$(find_helper || true)"
        if [[ -n "$HELPER" ]]; then
            METHOD="aur-helper"
        elif has_base_devel; then
            METHOD="aur-makepkg"
        else
            METHOD="aur-manual"
        fi
    else
        METHOD="appimage"
    fi

    if [[ "$METHOD" == "aur-helper" || "$METHOD" == "aur-makepkg" ]] && [[ "$USER_MODE" == "1" ]]; then
        die "--user is not supported with the AUR method; use --method appimage or source"
    fi
}

handle_installed_package() {
    find_installed_package || return 0

    case "$INSTALLED_MANAGER" in
        pacman)
            if [[ "$METHOD" == "auto" || "$METHOD" == "aur" ]]; then
                if [[ "$USER_MODE" == "1" ]]; then
                    printf 'Steam Library Mover is already installed by pacman (%s).\n' "$INSTALLED_PACKAGE"
                    printf 'Remove it or upgrade it with pacman/pacman-based tools; --user will not install a second copy.\n'
                    exit 0
                fi
                HELPER="$(find_helper || true)"
                if [[ -n "$HELPER" ]]; then
                    METHOD="aur-helper"
                    printf 'Steam Library Mover is already installed by pacman (%s).\n' "$INSTALLED_PACKAGE"
                    printf 'It will be upgraded through %s.\n' "$HELPER"
                    return 0
                fi
                printf 'Steam Library Mover is already installed by pacman (%s).\n' "$INSTALLED_PACKAGE"
                printf 'Upgrade it with:\n  sudo pacman -Syu %s\n' "$AUR_PACKAGE"
                exit 0
            fi
            ;;
        dpkg)
            printf 'Steam Library Mover is already installed by dpkg (%s).\n' "$INSTALLED_PACKAGE"
            printf 'Upgrade it with your package manager instead of installing a second copy.\n'
            exit 0
            ;;
        rpm)
            printf 'Steam Library Mover is already installed by rpm (%s).\n' "$INSTALLED_PACKAGE"
            printf 'Upgrade it with your package manager instead of installing a second copy.\n'
            exit 0
            ;;
    esac

    die "Steam Library Mover is already installed by $INSTALLED_MANAGER ($INSTALLED_PACKAGE); remove it before using get.sh"
}

fuse2_available() {
    command -v fusermount >/dev/null 2>&1 || return 1
    if command -v ldconfig >/dev/null 2>&1; then
        ldconfig -p 2>/dev/null | grep -q 'libfuse\.so\.2' && return 0
        return 1
    fi
    local dir
    for dir in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /lib /lib64; do
        [[ -e "$dir/libfuse.so.2" ]] && return 0
    done
    return 1
}

fuse_package_hint() {
    case "$FAMILY" in
        debian) printf 'libfuse2 (or libfuse2t64)' ;;
        arch) printf 'fuse2' ;;
        fedora) printf 'fuse-libs' ;;
        suse) printf 'libfuse2' ;;
        *) printf 'your distribution FUSE 2 package' ;;
    esac
}

print_plan() {
    printf 'Detected: %s %s, %s.\n' "${OS_ID:-unknown}" "${OS_VERSION:-}" "$(uname -m)"
    printf 'Method: %s\n' "$METHOD_DISPLAY"
    printf 'Will run:\n'

    case "$METHOD" in
        aur-helper)
            printf '  %s -S %s\n' "$HELPER" "$AUR_PACKAGE"
            ;;
        aur-makepkg)
            printf '  git clone https://aur.archlinux.org/%s.git %s/aur\n' "$AUR_PACKAGE" "$TMP_DIR"
            printf '  (cd %s/aur && makepkg -si)\n' "$TMP_DIR"
            ;;
        aur-manual)
            printf '  print manual AUR instructions\n'
            ;;
        appimage)
            printf '  mkdir -p %s/.local/lib/steam-library-mover %s/.local/bin %s/.local/share/applications\n' "$HOME" "$HOME" "$HOME"
            printf '  curl -fsSL -o %s/Steam_Library_Mover.AppImage %s\n' "$TMP_DIR" "$(asset_url "$VERSION" "$APPIMAGE_ASSET")"
            printf '  curl -fsSL -o %s/SHA256SUMS %s\n' "$TMP_DIR" "$(asset_url "$VERSION" "SHA256SUMS")"
            printf '  sha256sum --check --ignore-missing SHA256SUMS\n'
            printf '  install -m 0755 %s/Steam_Library_Mover.AppImage %s/.local/lib/steam-library-mover/Steam_Library_Mover.AppImage\n' "$TMP_DIR" "$HOME"
            printf '  (cd %s && %s/.local/lib/steam-library-mover/Steam_Library_Mover.AppImage --appimage-extract)\n' "$TMP_DIR" "$HOME"
            printf '  install -m 0644 <extracted desktop file> %s/.local/share/applications/steam-library-mover.desktop\n' "$HOME"
            printf '  install -m 0644 <extracted icon> %s/.local/share/icons/hicolor/<size>/apps/steam-library-mover.<ext>\n' "$HOME"
            printf "  sed -i 's|^Exec=.*|Exec=%s/.local/bin/steam-library-mover|' %s/.local/share/applications/steam-library-mover.desktop\n" "$HOME" "$HOME"
            printf '  create %s/.local/bin/steam-library-mover wrapper (execs the AppImage)\n' "$HOME"
            printf '  chmod 0755 %s/.local/bin/steam-library-mover\n' "$HOME"
            printf '  write %s\n' "$STATE_FILE"
            ;;
        source)
            printf '  curl -fsSL -o %s/steam-library-mover-%s.tar.gz %s\n' "$TMP_DIR" "${VERSION#v}" "$(asset_url "$VERSION" "steam-library-mover-${VERSION#v}.tar.gz")"
            printf '  curl -fsSL -o %s/SHA256SUMS %s\n' "$TMP_DIR" "$(asset_url "$VERSION" "SHA256SUMS")"
            printf '  sha256sum --check --ignore-missing SHA256SUMS\n'
            printf '  tar -xzf steam-library-mover-%s.tar.gz\n' "${VERSION#v}"
            printf '  %s\n' "$([ "$USER_MODE" -eq 1 ] && printf './install.sh --user' || printf './install.sh')"
            printf '  write %s\n' "$STATE_FILE"
            ;;
    esac
}

install_appimage() {
    local lib_dir="$HOME/.local/lib/$APP_NAME"
    local bin_dir="$HOME/.local/bin"
    local image="$lib_dir/Steam_Library_Mover.AppImage"
    local wrapper="$bin_dir/$APP_NAME"
    local desktop="$HOME/.local/share/applications/$APP_NAME.desktop"
    local appimage_tmp="$TMP_DIR/Steam_Library_Mover.AppImage"
    local extract_dir="$TMP_DIR/squashfs-root"
    local desktop_src icon_src icon_rel icon_dest

    mkdir -p "$TMP_DIR"
    download_verified "$APPIMAGE_ASSET" "$appimage_tmp"

    mkdir -p "$lib_dir" "$bin_dir" "$(dirname "$desktop")"
    install -m 0755 "$appimage_tmp" "$image"

    (cd "$TMP_DIR" && "$image" --appimage-extract >/dev/null)

    desktop_src="$(find "$extract_dir" -type f -name "$APP_NAME.desktop" -print -quit)"
    icon_src="$(find "$extract_dir" -type f -path "*/hicolor/*/apps/$APP_NAME.*" -print -quit)"
    [[ -n "$desktop_src" ]] || die "the AppImage does not contain $APP_NAME.desktop"
    [[ -n "$icon_src" ]] || die "the AppImage does not contain an application icon"

    icon_rel="${icon_src#*/hicolor/}"
    icon_dest="$HOME/.local/share/icons/hicolor/$icon_rel"
    mkdir -p "$(dirname "$icon_dest")"
    install -m 0644 "$desktop_src" "$desktop"
    install -m 0644 "$icon_src" "$icon_dest"
    sed -i "s|^Exec=.*|Exec=$wrapper|" "$desktop"

    {
        printf '#!/usr/bin/env bash\n'
        printf 'set -euo pipefail\n'
        if ! fuse2_available; then
            printf 'export APPIMAGE_EXTRACT_AND_RUN=1\n'
        fi
        printf 'exec %q "$@"\n' "$image"
    } > "$wrapper"
    chmod 0755 "$wrapper"

    if ! fuse2_available; then
        warn "FUSE 2 was not found; the wrapper will use APPIMAGE_EXTRACT_AND_RUN=1."
        warn "Install $(fuse_package_hint) if you want normal AppImage mounting."
    fi

    case ":$PATH:" in
        *":$bin_dir:"*) ;;
        *) warn "$bin_dir is not on PATH; add it to run $APP_NAME directly." ;;
    esac

    if [[ -x "$extract_dir/usr/bin/$APP_NAME-cli" ]]; then
        printf 'This AppImage exposes %s-cli inside the image, but its AppRun does not\n' "$APP_NAME"
        printf 'provide a CLI wrapper. Use --method aur or --method source for the CLI.\n'
    fi

    write_state "appimage" \
        "version=$VERSION" \
        "appimage=$image" \
        "wrapper=$wrapper" \
        "desktop=$desktop" \
        "icon=$icon_dest"

    printf 'Installed %s %s for %s in %s\n' "$APP_NAME" "$VERSION" "$(id -un)" "$lib_dir"
    printf 'Run: %s\n' "$wrapper"
}

install_aur() {
    require_sudo

    if [[ "$METHOD" == "aur-helper" ]]; then
        "$HELPER" -S "$AUR_PACKAGE"
    else
        mkdir -p "$TMP_DIR"
        git clone "https://aur.archlinux.org/$AUR_PACKAGE.git" "$TMP_DIR/aur"
        (cd "$TMP_DIR/aur" && makepkg -si)
    fi

    local package_version
    package_version="$(pacman -Q "$AUR_PACKAGE" 2>/dev/null | awk '{print $2}' || true)"
    [[ -n "$package_version" ]] || package_version="unknown"
    write_state "aur" \
        "version=v$package_version" \
        "package=$AUR_PACKAGE" \
        "user=0"

    printf 'Installed %s %s through the AUR.\n' "$AUR_PACKAGE" "$package_version"
}

download_source() {
    local asset="steam-library-mover-${VERSION#v}.tar.gz"
    local archive="$TMP_DIR/$asset"
    download_verified "$asset" "$archive"
    tar -xzf "$archive" -C "$TMP_DIR"
    SOURCE_DIR="$(find "$TMP_DIR" -maxdepth 1 -mindepth 1 -type d -name "steam-library-mover-*" -print -quit)"
    [[ -n "$SOURCE_DIR" ]] || die "could not find the extracted source tree"
}

install_source() {
    if ! version_ge "$MIN_SOURCE_VERSION" "$VERSION"; then
        die "Release $VERSION is older than the installer requires; use --method aur or wait for the next release."
    fi

    download_source
    [[ -f "$SOURCE_DIR/install.sh" ]] \
        || die "Release $VERSION does not contain install.sh; use --method aur or wait for the next release."

    if ((USER_MODE)); then
        (cd "$SOURCE_DIR" && bash ./install.sh --user)
        write_state "source" "version=$VERSION" "user=1"
    else
        require_sudo
        (cd "$SOURCE_DIR" && bash ./install.sh)
        write_state "source" "version=$VERSION" "user=0"
    fi

    printf 'Installed %s %s from the verified source archive.\n' "$APP_NAME" "$VERSION"
}

remove_state_path() {
    local path="$1" kind="$2"
    [[ -n "$path" ]] || return 0
    case "$path" in
        "$HOME"/.local/*) ;;
        *) die "refusing to remove unexpected $kind path from state: $path" ;;
    esac
    if [[ -e "$path" ]]; then
        rm -f -- "$path"
        printf 'Removed %s\n' "$path"
    fi
}

uninstall_appimage() {
    remove_state_path "$STATE_APPIMAGE" "AppImage"
    remove_state_path "$STATE_WRAPPER" "wrapper"
    remove_state_path "$STATE_DESKTOP" "desktop entry"
    remove_state_path "$STATE_ICON" "icon"

    local lib_dir="$HOME/.local/lib/$APP_NAME"
    if [[ -d "$lib_dir" ]]; then
        rmdir --ignore-fail-on-non-empty -- "$lib_dir" || true
    fi

    rm -f -- "$STATE_FILE"
    rmdir --ignore-fail-on-non-empty -- "$STATE_DIR" 2>/dev/null || true
    printf 'Removed the get.sh AppImage installation. Your game data and settings were not touched.\n'
}

uninstall_aur() {
    require_sudo
    local package="${STATE_PACKAGE:-$AUR_PACKAGE}"
    if pacman -Qq "$package" >/dev/null 2>&1; then
        sudo pacman -Rns "$package"
    else
        printf '%s is not installed by pacman; nothing to remove.\n' "$package"
    fi
    rm -f -- "$STATE_FILE"
    rmdir --ignore-fail-on-non-empty -- "$STATE_DIR" 2>/dev/null || true
}

uninstall_source() {
    VERSION="$(normalize_version "$STATE_VERSION")"
    if ! version_ge "$MIN_SOURCE_VERSION" "$VERSION"; then
        die "the recorded source release $VERSION is too old to contain uninstall.sh; remove it manually"
    fi
    download_source
    [[ -f "$SOURCE_DIR/uninstall.sh" ]] \
        || die "the recorded source release does not contain uninstall.sh; remove it manually"
    if [[ "$STATE_USER" == "1" ]]; then
        (cd "$SOURCE_DIR" && bash ./uninstall.sh --user)
    else
        require_sudo
        (cd "$SOURCE_DIR" && bash ./uninstall.sh)
    fi
    rm -f -- "$STATE_FILE"
    rmdir --ignore-fail-on-non-empty -- "$STATE_DIR" 2>/dev/null || true
}

print_no_state_uninstall() {
    printf 'No get.sh state file found at %s.\n' "$STATE_FILE"
    printf 'Uninstall by the method you used:\n\n'
    printf 'AUR:\n  sudo pacman -Rns %s\n\n' "$AUR_PACKAGE"
    printf 'AppImage:\n'
    printf '  rm -f ~/.local/bin/%s\n' "$APP_NAME"
    printf '  rm -f ~/.local/share/applications/%s.desktop\n' "$APP_NAME"
    printf '  rm -f ~/.local/lib/%s/Steam_Library_Mover.AppImage\n\n' "$APP_NAME"
    printf 'Source install:\n  run ./uninstall.sh in the source tree you built\n'
}

uninstall_all() {
    if ! read_state; then
        print_no_state_uninstall
        return 0
    fi

    printf 'Detected: %s %s, %s.\n' "${OS_ID:-unknown}" "${OS_VERSION:-}" "$(uname -m)"
    printf 'Method: %s\n' "$STATE_METHOD"
    printf 'Will remove the installation recorded in %s.\n' "$STATE_FILE"
    if ((DRY_RUN)); then
        case "$STATE_METHOD" in
            appimage) printf '  rm -f %s %s %s %s\n' "$STATE_APPIMAGE" "$STATE_WRAPPER" "$STATE_DESKTOP" "$STATE_ICON" ;;
            aur) printf '  sudo pacman -Rns %s\n' "${STATE_PACKAGE:-$AUR_PACKAGE}" ;;
            source) printf '  download and verify %s, then run ./uninstall.sh\n' "$STATE_VERSION" ;;
        esac
        printf 'Dry run: no commands executed.\n'
        return 0
    fi

    confirm "Remove the get.sh installation?"
    case "$STATE_METHOD" in
        appimage) uninstall_appimage ;;
        aur) uninstall_aur ;;
        source) uninstall_source ;;
        *) die "unknown recorded method: $STATE_METHOD" ;;
    esac
}

start_tmp() {
    TMP_DIR="$(mktemp -d)"
    cleanup() {
        [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]] && rm -rf -- "$TMP_DIR"
    }
    trap cleanup EXIT INT TERM
}

main() {
    parse_args "$@"

    if ((EUID == 0)); then
        die "Run as your normal user; get.sh uses sudo only for the steps that need it."
    fi

    parse_os_release
    STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/$APP_STATE_NAME"
    STATE_FILE="$STATE_DIR/get-sh.state"

    if ((UNINSTALL)); then
        ensure_tools
        if ((DRY_RUN == 0)); then
            start_tmp
        fi
        uninstall_all
        return 0
    fi

    is_x86_64 || die "get.sh supports x86_64 only; use the source install instructions from the README on this architecture."

    if [[ -f "$STATE_FILE" ]]; then
        read_state || true
        printf 'get.sh already recorded an installation at %s (method: %s).\n' "$STATE_FILE" "${STATE_METHOD:-unknown}"
        printf 'Run get.sh --uninstall first if you want to change methods or versions.\n'
        return 0
    fi

    if [[ -n "$OPT_VERSION" ]]; then
        VERSION="$(normalize_version "$OPT_VERSION")"
        [[ "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "invalid version: $OPT_VERSION"
    fi

    ensure_tools
    handle_installed_package || true
    choose_method

    if [[ "$METHOD" == "aur-manual" ]]; then
        manual_aur_steps
        return 0
    fi

    if [[ "$DRY_RUN" == "0" ]]; then
        start_tmp
    else
        TMP_DIR="${TMPDIR:-/tmp}/get-sh.XXXXXX"
    fi

    if [[ "$METHOD" != "aur-helper" && "$METHOD" != "aur-makepkg" ]]; then
        if [[ -z "$VERSION" ]]; then
            if ((DRY_RUN)); then
                VERSION="latest"
            else
                VERSION="$(resolve_latest_version)"
            fi
        fi
    fi

    if [[ "$METHOD" == "source" && "$VERSION" != "latest" ]] && ! version_ge "$MIN_SOURCE_VERSION" "$VERSION"; then
        die "Release $VERSION is older than the installer requires; use --method aur or wait for the next release."
    fi

    if [[ "$METHOD" == "aur-helper" ]]; then
        METHOD_DISPLAY="aur-helper ($HELPER)"
    else
        METHOD_DISPLAY="$METHOD"
    fi

    print_plan

    if ((DRY_RUN)); then
        printf 'Dry run: no commands executed.\n'
        return 0
    fi

    confirm "Continue with this installation?"
    case "$METHOD" in
        appimage) install_appimage ;;
        aur-helper|aur-makepkg) install_aur ;;
        source) install_source ;;
        *) die "unsupported method: $METHOD" ;;
    esac
}

main "$@"
