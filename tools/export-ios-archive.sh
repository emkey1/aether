#!/bin/sh
set -eu

usage() {
    echo "usage: $0 ARCHIVE_PATH|latest EXPORT_PATH [EXPORT_OPTIONS_PLIST]" >&2
    echo "example: $0 latest /tmp/iSH-AOK-export" >&2
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

if [ "$archive_path" = latest ]; then
    archive_path=$(find "$HOME/Library/Developer/Xcode/Archives" -name 'iSH*.xcarchive' -print0 | xargs -0 ls -td 2>/dev/null | head -n 1)
    if [ -z "$archive_path" ]; then
        echo "error: no iSH .xcarchive found under ~/Library/Developer/Xcode/Archives" >&2
        exit 1
    fi
fi

if [ ! -d "$archive_path" ]; then
    echo "error: archive not found at path '$archive_path'" >&2
    echo "hint: pass an actual .xcarchive path, or use: $0 latest $export_path" >&2
    exit 1
fi

echo "Exporting archive: $archive_path"

xcodebuild \
    -exportArchive \
    -archivePath "$archive_path" \
    -exportPath "$export_path" \
    -exportOptionsPlist "$export_options"
