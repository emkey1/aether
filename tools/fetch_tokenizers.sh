#!/usr/bin/env bash
# Put the tokenizers the guide token gate (tools/check_guide_tokens.py) measures
# with into a local cache, sha256-checked. They are third-party files, so they
# are fetched, never vendored into this repo.
#
#   tools/fetch_tokenizers.sh                     fetch both (needs network)
#   tools/fetch_tokenizers.sh --o200k-from FILE   copy a local o200k_base.tiktoken
#                                                 (or a tiktoken cache entry) instead
#   tools/fetch_tokenizers.sh --qwen-from FILE    copy a local Qwen3.5 tokenizer.json
#                                                 (e.g. an LM Studio download) instead
#   tools/fetch_tokenizers.sh --verify            check what is cached; fetch nothing
#
# Cache: $TOKENIZER_DIR, default ~/.cache/aether-tokenizers
#   tiktoken/<sha1 of the o200k URL>   tiktoken's own cache naming; check_guide_tokens.py
#                                      points TIKTOKEN_CACHE_DIR here when it is unset
#   qwen3.5/tokenizer.json
#
# The Qwen3.5 file has no default URL: set QWEN_TOKENIZER_URL to a tokenizer.json
# whose sha256 is QWEN_SHA256 below. The pinned file is the one the 2026-10-06
# review measured (medium 15,913); it came from a local Qwen3.6-27B derivative
# (model_type qwen3_5), whose NOTICE names Qwen/Qwen3.6-27B on Hugging Face as
# the upstream. Confirm that upstream's hash before pointing CI at it; a file
# that does not match the pin is rejected, never cached.
set -euo pipefail

O200K_URL="https://openaipublic.blob.core.windows.net/encodings/o200k_base.tiktoken"
O200K_SHA256="446a9538cb6c348e3516120d7c08b09f57c36495e2acfffe59a5bf8b0cfb1a2d"
O200K_CACHE_NAME="fb374d419588a4632f3f557e76b4b70aebbca790"   # sha1 of O200K_URL
QWEN_SHA256="87a7830d63fcf43bf241c3c5242e96e62dd3fdc29224ca26fed8ea333db72de4"
QWEN_URL="${QWEN_TOKENIZER_URL:-}"

DEST="${TOKENIZER_DIR:-$HOME/.cache/aether-tokenizers}"
O200K_DEST="$DEST/tiktoken/$O200K_CACHE_NAME"
QWEN_DEST="$DEST/qwen3.5/tokenizer.json"

o200k_from=""
qwen_from=""
verify_only=0
while [ $# -gt 0 ]; do
    case "$1" in
        --o200k-from) o200k_from="$2"; shift 2 ;;
        --qwen-from) qwen_from="$2"; shift 2 ;;
        --verify) verify_only=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

# install NAME PIN DEST [LOCAL_SOURCE] [URL]
install_one() {
    local name="$1" pin="$2" dest="$3" src="$4" url="$5"
    if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$pin" ]; then
        echo "ok      $name  $dest"
        return 0
    fi
    if [ "$verify_only" = 1 ]; then
        if [ -f "$dest" ]; then
            echo "BAD     $name  $dest does not match its pinned sha256" >&2
        else
            echo "missing $name  $dest" >&2
        fi
        return 1
    fi
    mkdir -p "$(dirname "$dest")"
    local tmp="$dest.tmp.$$"
    if [ -n "$src" ]; then
        cp "$src" "$tmp"
    elif [ -n "$url" ]; then
        curl -fsSL --retry 3 -o "$tmp" "$url"
    else
        echo "skip    $name  no source: pass --qwen-from FILE or set QWEN_TOKENIZER_URL" >&2
        return 1
    fi
    local got
    got="$(sha256_of "$tmp")"
    if [ "$got" != "$pin" ]; then
        rm -f "$tmp"
        echo "BAD     $name  sha256 $got, expected $pin; nothing cached" >&2
        return 1
    fi
    mv "$tmp" "$dest"
    echo "cached  $name  $dest"
}

status=0
install_one o200k_base "$O200K_SHA256" "$O200K_DEST" "$o200k_from" "$O200K_URL" || status=1
install_one qwen3.5 "$QWEN_SHA256" "$QWEN_DEST" "$qwen_from" "$QWEN_URL" || status=1
exit "$status"
