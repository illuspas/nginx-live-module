#!/bin/bash
# RTMP round-trip test: push over RTMP, pull over RTMP and HTTP-FLV.
#
# Usage: rtmp.sh [app] [name] [seconds]
#   rtmp.sh                  # myapp/rtmp, 20s, H264+AAC live source
#   rtmp.sh myapp cam1 30
set -u
cd "$(dirname "$0")"
APP=${1:-myapp}
NAME=${2:-rtmp}
SECS=${3:-20}

echo "== pushing $SECS s over RTMP (background)"
RTMP=1 ./push.sh "$APP" "$NAME" "$SECS" &
PUSH=$!

sleep 4

echo "== RTMP pull:"
RTMP=1 ./play.sh "$APP" "$NAME" probe || exit 1

echo "== HTTP-FLV pull of the same stream:"
./play.sh "$APP" "$NAME" probe || exit 1

echo "== RTMP dump + decode check:"
DUR=6 RTMP=1 ./play.sh "$APP" "$NAME" dump
ffmpeg -v error -i /tmp/$NAME.flv -f null - && echo "decode OK"

wait $PUSH 2>/dev/null || true
echo "== rtmp test passed"
