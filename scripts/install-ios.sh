#!/bin/sh
# usage: install-ios.sh [install|run|game <iso or folder>|logs]
#   install: devicectl install of out/ios/Black.app     run: launch it with its console
#   game: copy the ISO (or default.xbe) into the app's Documents/Black                  logs: copy the app's log folder here
set -e
cd "$(dirname "$0")/.."
CONF=ios/signing.local.env
[ -f "$CONF" ] && . "./$CONF"
DEV=${BLACK_DEVICE:-$DEVICE}
ID=${BUNDLE_ID:?BUNDLE_ID in ios/signing.local.env}
[ -n "$DEV" ] || { echo "DEVICE in ios/signing.local.env (xcrun devicectl list devices)" >&2; exit 1; }
case "$1" in
    install) xcrun devicectl device install app --device $DEV out/ios/Black.app ;;
    run) # the app's stderr goes to os_log, not the console: read the log file with "logs"
         xcrun devicectl device process launch --device $DEV --terminate-existing \
             --environment-variables '{"BLACK_DEBUG_PRESENT":"1","BLACK_DEBUG_INPUT":"1"}' $ID ;;
    game) [ -n "$2" ] || { echo "usage: install-ios.sh game <file>" >&2; exit 1; }
          xcrun devicectl device copy to --device $DEV --domain-type appDataContainer --domain-identifier $ID \
              --source "$2" --destination "Documents/Black/$(basename "$2")" ;;
    logs) mkdir -p out/ios/device-logs
          xcrun devicectl device copy from --device $DEV --domain-type appDataContainer --domain-identifier $ID \
              --source "Library/Application Support/com.muhammad/Black/logs" --destination out/ios/device-logs ;;
    *) sed -n 2,5p "$0"; exit 1 ;;
esac
