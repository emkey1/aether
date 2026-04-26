#!/bin/sh
set -eu

usage() {
    echo "usage: $0 ARCHIVE_PATH EXPORT_PATH [EXPORT_OPTIONS_PLIST]" >&2
    exit 2
}

[ "$#" -ge 2 ] || usage
[ "$#" -le 3 ] || usage

archive_path=$1
export_path=$2
export_options=${3:-AppStoreExportOptions.plist}

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case "$export_options" in
    /*) ;;
    *) export_options="$repo_root/$export_options" ;;
esac

xcodebuild \
    -exportArchive \
    -archivePath "$archive_path" \
    -exportPath "$export_path" \
    -exportOptionsPlist "$export_options"
