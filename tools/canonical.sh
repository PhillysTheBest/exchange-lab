#!/bin/sh
# tools/canonical.sh BUILD — one line per output that must be identical on every
# platform (docs/PORTABILITY_PLAN.md, stage 0). `make canonical` runs it and
# compares with tests/canonical.txt, so the first line that differs names what
# diverged first.
#
# Engine and sessions (integer, the canonical state) come first; the run's files
# that hold derived `double` statistics (comparison.csv, evaluation.csv,
# attribution.csv, report.txt) come last and are labelled, so a difference in
# arithmetic is told apart from a difference in the market.
#
# POSIX sh and cksum only, so it runs unchanged on Linux, macOS and MSYS2.
set -e
B=${1:-build}
R=$B/release
D=$B/canonical
EXE=
[ -f "$R/sweep.exe" ] && EXE=.exe
rm -rf "$D"
mkdir -p "$D"

crc() { cksum < "$1" | cut -d' ' -f1-2; }
# Windows programs write CRLF through redirection; compare content, not line ends.
lf() { tr -d '\r' < "$1" > "$1.lf"; mv "$1.lf" "$1"; }

./$B/debug/test_determinism$EXE | tr -d '\r' | sed -n 's/^direct *hash=\([0-9a-f]*\) events=\([0-9]*\).*/engine.determinism hash=\1 events=\2/p'

./$B/debug/test_arena$EXE | tr -d '\r' > "$D/arena.txt"
sed -n 's/^  session: .* hash \([0-9a-f]*\)$/arena.session hash=\1/p' "$D/arena.txt"
sed -n 's/^  ui: .* hash \([0-9a-f]*\)$/arena.frame hash=\1/p' "$D/arena.txt"

./$R/sweep$EXE --seeds=4 --quiet | tr -d '\r' | sed -n 's/^  session hash at seed 42, per point: /sweep.seed42 /p'

./$R/suite$EXE --candidates=maker --block=smoke --out="$D/run" > /dev/null 2>&1
for s in standard wide busy; do
    ./$R/suite$EXE --suite=$s --candidates=maker --block=smoke --out="$D/fp-$s" > /dev/null 2>&1
    echo "suite.$s fingerprint=$(sed -n 's/^fingerprint=//p' "$D/fp-$s/outcome.cfg" | tr -d '\r')"
done
for f in sessions.csv participants.csv; do
    lf "$D/run/$f"
    echo "run.$f $(crc "$D/run/$f")"
done

I=./$R/ingest$EXE
for s in examples/data/*; do
    case "$s" in *.md|*orderbook*|*demo_trades*) continue ;; esac
    opts=
    case "$s" in *ITCH50) opts=symbol=DEMO ;; *quotes_and_trades.csv) opts='columns=time,price,qty,side,bid,bid_qty,ask,ask_qty' ;; esac
    rm -rf "$D/data"
    $I add "$s" dir="$D/data" $opts > /dev/null
    for t in "$D"/data/*.tape; do
        lf "$t"
        echo "tape.$(basename "$t" .tape) $(crc "$t")"
    done
done

./$R/calibrate$EXE --known=known data="$D/data" > /dev/null
lf "$D/data/known.tape"
echo "tape.known $(crc "$D/data/known.tape")"

for f in comparison.csv evaluation.csv attribution.csv report.txt; do
    lf "$D/run/$f"
    echo "derived.run.$f $(crc "$D/run/$f")"
done
