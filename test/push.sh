#!/bin/bash
# Push a stream to the server (HTTP-FLV or RTMP publish).
#
# Usage: push.sh [app] [name] [seconds] [ffmpeg options...]
#   push.sh                          # myapp/test, 30s, H264+AAC live source
#   push.sh myapp cam1 60            # 60 seconds
#   push.sh myapp cam1 0 -i in.mp4 -c copy   # remux a file (0 = no limit)
#   RTMP=1 push.sh ...               # publish over RTMP (port 1935)
set -u
BASE=${BASE:-http://127.0.0.1:8080}
RTMP_BASE=${RTMP_BASE:-rtmp://127.0.0.1:1935}
APP=${1:-myapp}
NAME=${2:-test}
SECS=${3:-30}
shift 3 2>/dev/null || shift $#   # may be fewer args

if [ "${RTMP:-0}" != "0" ]; then
    BASE=$RTMP_BASE
fi

# RTMP URLs take the stream name without the .flv suffix; both are
# accepted by the HTTP side, so keep one form.

SRC=( -re -f lavfi -i testsrc2=size=640x360:rate=25
      -f lavfi -i sine=frequency=440:sample_rate=44100 )
ENC=( -c:v libx264 -preset veryfast -tune zerolatency -g 50 -c:a aac )

if [ $# -gt 0 ]; then
    # custom ffmpeg arguments replace the default source+codec
    exec ffmpeg -nostats -loglevel warning "$@" \
        ${SECS:+} ${SECS:+} -f flv "$BASE/$APP/$NAME"
fi

exec ffmpeg -nostats -loglevel warning \
    "${SRC[@]}" "${ENC[@]}" \
    ${SECS:+"-t" "$SECS"} \
    -f flv "$BASE/$APP/$NAME"
