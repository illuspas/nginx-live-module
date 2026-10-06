#!/bin/bash
# Enhanced-RTMP codec matrix test using precompiled samples
# (~/Movies/tcl_*.mp4, or pass a directory via MOVIES=...).
#
# Pushes each sample through the server and verifies the pulled
# stream: codec passthrough via ffprobe + tag framing integrity +
# enhanced fourcc in the startup sequence.
#
# Usage: matrix.sh [seconds]
set -u
BASE=${BASE:-http://127.0.0.1:8080}
MOVIES=${MOVIES:-/Users/aliang/Movies}
DUR=${1:-20}
OUT=${OUT:-/tmp/live-matrix}
mkdir -p "$OUT"

walk() {
python3 - "$1" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
if data[:3] != b"FLV":
    sys.exit("not FLV")
pos, n = 13, 0
while pos + 15 <= len(data) and n < 8:
    t = data[pos]
    size = int.from_bytes(data[pos+1:pos+4], 'big')
    body = data[pos+11:pos+11+size]
    prev = int.from_bytes(data[pos+11+size:pos+15+size], 'big')
    if prev != 11 + size:
        sys.exit(f"PreviousTagSize broken at tag {n}")
    if body:
        b0 = body[0]
        if t == 9 and b0 & 0x80:
            print(f"   tag#{n} ENHANCED pkt={b0 & 0xf} frame={(b0 >> 4) & 7} fourcc={body[1:5].decode()}")
        elif t == 8 and (b0 >> 4) == 9:
            print(f"   tag#{n} AUDIO-EX pkt={b0 & 0xf} fourcc={body[1:5].decode()}")
    pos += 15 + size
    n += 1
print(f"   framing: OK ({n} tags checked)")
EOF
}

run_one() {
    local name=$1 src=$2
    echo "=== $name : $(basename "$src")"
    (ffmpeg -nostats -loglevel error -re -t "$DUR" -i "$src" \
        -c copy -f flv "$BASE/myapp/$name" > "$OUT/$name.push.log" 2>&1 &)
    sleep 4
    curl -s -m 12 "$BASE/myapp/$name.flv" -o "$OUT/$name.pull.flv"
    echo "   pulled: $(stat -f%z "$OUT/$name.pull.flv" 2>/dev/null || stat -c%s "$OUT/$name.pull.flv") bytes"
    echo "   codecs: $(ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$OUT/$name.pull.flv" 2>/dev/null | tr '\n' ' ')"
    walk "$OUT/$name.pull.flv" || echo "   FAILED: $walk_rc"
    head -3 "$OUT/$name.push.log" | sed 's/^/   PUSH: /'
}

run_one h264 "$MOVIES/tcl_264.mp4"
run_one h265 "$MOVIES/tcl_265.mp4"
run_one av1  "$MOVIES/tcl_av1.mp4"
run_one vp9  "$MOVIES/tcl_vp9.mp4"
run_one opus "$MOVIES/tcl_opus.mp4"
