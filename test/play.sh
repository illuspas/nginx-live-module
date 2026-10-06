#!/bin/bash
# Play / inspect a live stream.
#
# Usage: play.sh [app] [name] [mode]
#   play.sh                     # probe myapp/test
#   play.sh myapp test ffplay   # open a player window
#   play.sh myapp test dump     # save 15s to /tmp/<name>.flv and probe it
#   play.sh myapp test tags     # dump the first tags of the startup sequence
#   RTMP=1 play.sh ...          # use RTMP (port 1935; tags mode is HTTP-only)
set -u
BASE=${BASE:-http://127.0.0.1:8080}
RTMP_BASE=${RTMP_BASE:-rtmp://127.0.0.1:1935}
APP=${1:-myapp}
NAME=${2:-test}
MODE=${3:-probe}

URL="$BASE/$APP/$NAME.flv"

if [ "${RTMP:-0}" != "0" ] && [ "$MODE" != "tags" ]; then
    URL="$RTMP_BASE/$APP/$NAME"
fi

case "$MODE" in
probe)
    ffprobe -v error -show_entries \
        stream=codec_name,codec_type,width,height,sample_rate \
        -of default=noprint_wrappers=1 "$URL"
    ;;
ffplay)
    exec ffplay -loglevel warning "$URL"
    ;;
dump)
    DUR=${DUR:-15}
    OUT=/tmp/$NAME.flv
    if [ "${RTMP:-0}" != "0" ]; then
        ffmpeg -nostats -loglevel error -y -t "$DUR" -i "$URL" -c copy -f flv "$OUT"
    else
        curl -s -m "$DUR" "$URL" -o "$OUT"
    fi
    echo "saved $(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT") bytes to $OUT"
    ffprobe -v error -show_entries stream=codec_name,width,height \
        -of default=noprint_wrappers=1 "$OUT"
    ;;
tags)
    curl -s -m 3 "$URL" -o /tmp/$NAME-tags.flv || true
    python3 - /tmp/$NAME-tags.flv <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
if data[:3] != b"FLV":
    sys.exit("not an FLV response")
pos, n = 13, 0
while pos + 15 <= len(data) and n < 12:
    t = data[pos]
    size = int.from_bytes(data[pos+1:pos+4], 'big')
    ts = int.from_bytes(data[pos+4:pos+7], 'big') | (data[pos+7] << 24)
    body = data[pos+11:pos+11+size]
    kind = ""
    if body:
        b0 = body[0]
        if t == 9 and b0 & 0x80:
            kind = f" enhanced pkt={b0 & 0xf} frame={(b0 >> 4) & 7} fourcc={body[1:5]}"
        elif t == 8 and (b0 >> 4) == 9:
            kind = f" enhanced pkt={b0 & 0xf} fourcc={body[1:5]}"
        elif t == 9:
            kind = f" legacy codec={b0 & 0xf} frame={(b0 >> 4) & 7}"
    print(f"tag#{n} type={t} size={size} ts={ts}{kind}")
    pos += 15 + size
    n += 1
EOF
    ;;
*)
    echo "unknown mode: $MODE (probe|ffplay|dump|tags)" >&2
    exit 2
    ;;
esac
