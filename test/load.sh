#!/bin/bash
# S6 concurrency test: 1 publisher, N (default 100) concurrent players,
# plus one slow consumer (trickle reader).  Verifies:
#   - all players receive identical framing-consistent data
#   - slow consumer does not stall the publisher or the fast players
#   - memory stays flat during the run
# Usage: load.sh [N] [seconds]
set -u
BASE=${BASE:-http://127.0.0.1:8080}
N=${1:-100}
SECS=${2:-30}
OUT=/tmp/live-load
mkdir -p "$OUT"; rm -f "$OUT"/*

echo "== starting publisher (${SECS}s) and $N players"

(ffmpeg -nostats -loglevel error -re -t "$SECS" -i /Users/aliang/Movies/tcl_264.mp4 \
    -c copy -f flv "$BASE/myapp/load" > "$OUT/push.log" 2>&1 &)

# let the publisher produce the startup sequence first
sleep 2

ok=0; fail=0
for i in $(seq 1 "$N"); do
    (curl -s -m "$((SECS + 5))" "$BASE/myapp/load.flv" -o "$OUT/p$i.flv" \
        && echo ok >> "$OUT/results" || echo fail >> "$OUT/results") &
done

# one slow consumer: 16 KB/s trickle with a tiny buffer
(sleep 2; curl -s --limit-rate 16k -m "$((SECS + 5))" "$BASE/myapp/load.flv" \
    -o "$OUT/slow.flv"; echo "slow rc=$?" >> "$OUT/results") &

echo "== sampling memory (RSS) of worker"
PID=$(cat "$(dirname "$0")/../test/logs/nginx.pid" 2>/dev/null || pgrep -f "nginx: worker" | head -1)
for t in 1 2 3; do
    sleep $((SECS / 4))
    ps -o rss=,vsz= -p "$PID" 2>/dev/null | awk '{print "  sample: rss=" $1 "KB"}'
done

wait
echo "== results:"
sort "$OUT/results" | uniq -c

echo "== integrity (per-player framing check):"
good=0; bad=0
for f in "$OUT"/p*.flv; do
    if python3 - "$f" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
if len(data) < 100 or data[:3] != b"FLV":
    sys.exit(1)
pos = 13
while pos + 15 <= len(data):
    size = int.from_bytes(data[pos+1:pos+4], 'big')
    prev = int.from_bytes(data[pos+11+size:pos+15+size], 'big')
    if prev != 11 + size:
        sys.exit(1)
    pos += 15 + size
sys.exit(0)
EOF
    then good=$((good+1)); else bad=$((bad+1)); echo "  BROKEN: $f"; fi
done
echo "  players with intact framing: $good good, $bad bad"
echo "  sizes: min=$(ls -l "$OUT"/p*.flv | awk '{print $5}' | sort -n | head -1) max=$(ls -l "$OUT"/p*.flv | awk '{print $5}' | sort -n | tail -1)"
echo "== push log:"; cat "$OUT/push.log" | head -3
echo "== slow consumer size: $(ls -l "$OUT/slow.flv 2>/dev/null" | awk '{print $5}')"
