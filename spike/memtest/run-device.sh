#!/bin/sh
# usage: run-device.sh <variant>  (after build-ios.sh <variant> --sign): install, launch, copy the report back
set -e
cd "$(dirname "$0")"
V=${1:?variant}
. ../../ios/signing.local.env
DEV=$DEVICE
MEMTEST=$BUNDLE_ID.memtest
mkdir -p results
xcrun devicectl device install app --device $DEV build/$V/Memtest.app
xcrun devicectl device process launch --device $DEV --terminate-existing $MEMTEST || true
sleep 4
xcrun devicectl device copy from --device $DEV --domain-type appDataContainer --domain-identifier $MEMTEST \
    --source Documents/memtest-$V.txt --destination results/memtest-$V.txt && cat results/memtest-$V.txt \
    || echo "copy failed: read it on screen or in Files > Memtest"
