#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Destructive, root-only NTFS integration test. Creates two loopback NTFS
# images and a disposable Steam root. Never points at a real Steam library.
#
# Usage: sudo tests/manual/ntfs-loop.sh [path-to-steam-library-mover-cli]
#
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "Run as root (loop mounts are required)." >&2
    exit 1
fi
CLI=${1:-"$(cd "$(dirname "$0")/../.." && pwd)/build/steam-library-mover-cli"}
[[ -x "$CLI" ]] || { echo "CLI not found/executable: $CLI" >&2; exit 1; }
command -v mount.ntfs-3g >/dev/null || command -v ntfs-3g >/dev/null || {
    echo "ntfs-3g is required." >&2; exit 1;
}

ROOT=$(mktemp -d /tmp/slm-ntfs-test.XXXXXX)
IMAGE_A="$ROOT/a.img"
IMAGE_B="$ROOT/b.img"
MNT_A="$ROOT/mnt-a"
MNT_B="$ROOT/mnt-b"
STEAM="$ROOT/steam"
LOOP_A=""
LOOP_B=""
cleanup() {
    set +e
    [[ -n "$LOOP_A" ]] && umount "$MNT_A" 2>/dev/null
    [[ -n "$LOOP_B" ]] && umount "$MNT_B" 2>/dev/null
    [[ -n "$LOOP_A" ]] && losetup -d "$LOOP_A" 2>/dev/null
    [[ -n "$LOOP_B" ]] && losetup -d "$LOOP_B" 2>/dev/null
    rm -rf "$ROOT"
}
trap cleanup EXIT

mkdir -p "$MNT_A" "$MNT_B" "$STEAM/steamapps/compatdata/999999/pfx" \
         "$STEAM/steamapps/common" "$MNT_A/steamapps/common/Test Game" \
         "$MNT_A/steamapps/shadercache/999999" "$MNT_B/steamapps"
truncate -s 2G "$IMAGE_A" "$IMAGE_B"
mkfs.ntfs -F -q "$IMAGE_A"
mkfs.ntfs -F -q "$IMAGE_B"
LOOP_A=$(losetup -f --show "$IMAGE_A")
LOOP_B=$(losetup -f --show "$IMAGE_B")
mount -t ntfs-3g -o uid=0,gid=0,umask=022 "$LOOP_A" "$MNT_A"
mount -t ntfs-3g -o uid=0,gid=0,umask=022 "$LOOP_B" "$MNT_B"

ln -s "$STEAM/steamapps/compatdata" "$MNT_A/steamapps/compatdata"
ln -s "$STEAM/steamapps/compatdata" "$MNT_B/steamapps/compatdata"

printf 'game payload\n' > "$MNT_A/steamapps/common/Test Game/game.dat"
printf 'prefix data\n' > "$STEAM/steamapps/compatdata/999999/pfx/system.reg"
printf 'shader\n' > "$MNT_A/steamapps/shadercache/999999/cache.bin"
cat > "$MNT_A/steamapps/appmanifest_999999.acf" <<'EOF'
"AppState"
{
    "appid" "999999"
    "name" "Test Game"
    "StateFlags" "4"
    "installdir" "Test Game"
    "SizeOnDisk" "13"
}
EOF
cat > "$STEAM/steamapps/libraryfolders.vdf" <<EOF
"libraryfolders"
{
    "0"
    {
        "path" "$STEAM"
        "label" ""
        "apps" { "999999" "13" }
    }
    "1"
    {
        "path" "$MNT_A"
        "label" "loop A"
        "apps" { }
    }
    "2"
    {
        "path" "$MNT_B"
        "label" "loop B"
        "apps" { }
    }
}
EOF

export SLM_STATE_DIR="$ROOT/state"
"$CLI" list --steam-root "$STEAM"
"$CLI" plan --steam-root "$STEAM" --appid 999999 --to "$MNT_B" --allow-ntfs
"$CLI" move --steam-root "$STEAM" --appid 999999 --to "$MNT_B" --allow-ntfs --yes

test -f "$MNT_B/steamapps/appmanifest_999999.acf"
test -f "$MNT_B/steamapps/common/Test Game/game.dat"
test ! -e "$MNT_A/steamapps/common/Test Game"
test ! -e "$MNT_A/steamapps/appmanifest_999999.acf"
test -f "$STEAM/steamapps/compatdata/999999/pfx/system.reg"
echo "NTFS loop move test passed."
