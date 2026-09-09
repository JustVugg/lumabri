#!/bin/sh
# This file also ships as Lumabri.command in the native macOS candidate.
# Keep the terminal attached: approval and chat are interactive.
bundle_dir=$(CDPATH= cd -P "$(dirname "$0")" && pwd) || exit 1
exec "$bundle_dir/bin/lumabri" "$@"
