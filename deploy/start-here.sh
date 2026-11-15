#!/bin/sh
# Double-click start. Ships as start-here.sh on Linux and START-HERE.command
# on macOS. It creates this computer's household if there is none, starts
# sharing and the local web page, and opens the chat page in the default
# browser. The page is served on 127.0.0.1 only.
#   --tui         open the terminal workspace instead of the browser
#   --no-browser  print the private sign-in link instead of opening it
# Everything keeps running after this window closes. To stop it, run the
# other launcher in this folder with: service stop
bundle_dir=$(CDPATH= cd -P "$(dirname "$0")" && pwd) || exit 1
"$bundle_dir/bin/lumabri" start "$@"
status=$?
# A window opened by a double click closes with this script. Keep a failure
# on screen until it has been read.
if [ "$status" -ne 0 ] && [ "$#" -eq 0 ] && [ -t 0 ] && [ -t 1 ]; then
    printf '\nLumabri did not start completely. Press Enter to close this window.\n'
    read -r _
fi
exit "$status"
