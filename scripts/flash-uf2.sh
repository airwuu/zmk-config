#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s left|right|reset [path-to-artifacts]\n' "$0" >&2
    exit 2
}

target="${1:-}"
artifacts_dir="${2:-.}"

case "$target" in
    left)
        uf2="eyelash_corne_left.uf2"
        ;;
    right)
        uf2="eyelash_corne_right.uf2"
        ;;
    reset)
        uf2="eyelash_corne_settings_reset.uf2"
        ;;
    *)
        usage
        ;;
esac

source_file="${artifacts_dir%/}/$uf2"

if [[ ! -f "$source_file" ]]; then
    printf 'Could not find %s\n' "$source_file" >&2
    printf 'Pass the directory containing the downloaded/extracted UF2 files as the second argument.\n' >&2
    exit 1
fi

find_mount() {
    local base

    for base in "/run/media/${USER:-}" "/media/${USER:-}" "/Volumes"; do
        if [[ -d "$base/NICENANO" ]]; then
            printf '%s\n' "$base/NICENANO"
            return 0
        fi
    done

    return 1
}

mount_point="$(find_mount || true)"

if [[ -z "$mount_point" ]]; then
    printf 'NICENANO drive not found.\n' >&2
    printf 'Put the target half in bootloader mode, then run this command again.\n' >&2
    exit 1
fi

printf 'Copying %s to %s\n' "$source_file" "$mount_point"
cp "$source_file" "$mount_point/"
sync

printf 'Done. The NICENANO drive may disappear after a successful flash.\n'
