#!/usr/bin/env bash
# bench/run_arena_evidence.sh <label> — regenerates results/arena-<label>/.
#
# Every arena number quoted in README.md and docs/ARENA.md comes out of this
# script, the same arrangement run_evidence.sh gives the engine's benchmarks: the
# tables are regenerated, never edited. If a number in the documentation is not
# in one of these files, it should not be in the documentation.
#
# What it runs, and why each one is here:
#   headline     the curve. The maker's reaction delay over three decades.
#   null         the same sweep with the only informed participant switched off.
#                With no information in the session, speed must be worth nothing.
#                This is the control, and it runs before the result is believed.
#   no-rival     the same sweep with the competing maker removed, which is the
#                degenerate market the design notes warn about. Kept as evidence
#                that the warning is real rather than a story.
#   edge/spread/ three robustness sweeps. A shape that only exists at one set of
#   arrival      parameters is not a finding, so the other parameters get swept
#                too and the files are published whatever they say.
#
# Determinism is checked rather than claimed: --replay-check runs every session
# twice and compares session hashes, and the script fails if any pair differs.
#
# Nothing here measures the machine. Reaction delays are simulated inputs, so
# unlike the engine's benchmarks these results do not depend on the hardware,
# the power plan or the load — the same seed gives the same answer anywhere.
set -euo pipefail

label="${1:-local}"
out="results/arena-${label}"
sweep="build/release/sweep"
[[ -x "${sweep}.exe" ]] && sweep="${sweep}.exe"

if [[ ! -x "$sweep" ]]; then
    echo "build the sweep runner first:  make arena" >&2
    exit 1
fi

mkdir -p "$out"
echo "writing $out"

# What produced these numbers. No machine state: there is none to record.
{
    echo "arena evidence"
    echo "label:    $label"
    echo "date:     $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "commit:   $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "dirty:    $(git status --porcelain 2>/dev/null | wc -l) files"
    echo "compiler: $(${CXX:-g++} --version | head -1)"
    echo
    echo "Every number in this directory is simulated. Reaction delays are inputs"
    echo "chosen by the experimenter, not measurements. The engine's measured"
    echo "latency is in results/windows/ and results/wsl2/ and is unrelated."
} > "$out/about.txt"

SEEDS="${SEEDS:-24}"
REQUESTS="${REQUESTS:-200000}"
DECADES="1000,10000,100000,1000000"

run() {
    local name="$1"
    shift
    echo "  $name"
    "$sweep" --label="$name" --seeds="$SEEDS" --requests="$REQUESTS" \
        --csv="$out/${name}_sessions.csv" --summary-csv="$out/${name}.csv" \
        "$@" > "$out/${name}.txt" 2>&1 || {
        echo "FAILED: $name (see $out/${name}.txt)" >&2
        return 1
    }
}

# The result, and the control that has to come out flat for the result to mean
# anything. Both with the replay check on.
run headline --param=maker.reaction_ns --values="$DECADES" --replay-check
run null     --param=maker.reaction_ns --values="$DECADES" --replay-check --null

# The market the design notes call degenerate, published so the claim is checkable.
run no-rival --param=maker.reaction_ns --values="$DECADES" --set=rival.enabled=0

# Robustness: does the shape survive changing everything else?
run edge     --param=informed.edge_ticks    --values=0,1,2,4
run spread   --param=maker.half_spread_ticks --values=1,2,4,8
run arrival  --param=noise.timer_ns          --values=2500,5000,10000,40000
run learning --param=maker.impact_centiticks --values=15,30,60,120

# A frame of the desktop from a fixed seed and a fixed instant, so the picture in
# the documentation is reproducible rather than a screenshot somebody took once.
arena="build/release/arena"
[[ -x "${arena}.exe" ]] && arena="${arena}.exe"
if [[ -x "$arena" ]]; then
    echo "  still frame"
    "$arena" --still=4000000 --seed=42 > "$out/still.ansi" 2>&1 || true
    sed -e 's/\x1b\[[0-9;]*m//g' -e 's/\x1b\[H//' "$out/still.ansi" > "$out/still.txt" || true
fi

echo
echo "wrote:"
ls -1 "$out"
echo
echo "summary tables are the *.csv files; *.txt holds what was printed."
