#!/usr/bin/env bash
# bench/run_evidence.sh — regenerate results/<label>/: the machine's state plus
# the raw output of every benchmark run the README quotes. Nothing is filtered:
# every run's text output, CSV rows and outlier dumps are kept.
#
#   bench/run_evidence.sh windows          (Git Bash on Windows, MSYS2 UCRT64 on PATH)
#   bench/run_evidence.sh wsl2             (inside WSL / Linux; set BUILD=build-linux)
#
# Runs: thin and deep profiles x seeds 42-46 (1M recorded orders each), both
# profiles replayed 5x (seed 42), both profiles paced at 50k orders/s replayed
# 3x, then the per-operation and ring microbenchmarks. Takes ~5 minutes.
set -euo pipefail
cd "$(dirname "$0")/.."

label="${1:?usage: bench/run_evidence.sh <label>   e.g. windows | wsl2}"
build="${BUILD:-build}"
if [[ "${OS:-}" == "Windows_NT" ]]; then
    exe=".exe"
    make_cmd="${MAKE:-mingw32-make}"
else
    exe=""
    make_cmd="${MAKE:-make}"
fi
bin="$build/release"
out="results/$label"
mkdir -p "$out"
rm -f "$out"/*.csv "$out"/*.txt

"$make_cmd" BUILD="$build" "$bin/bench_matching_latency$exe" "$bin/bench_engine$exe" "$bin/bench_ring$exe" >/dev/null

# ---- machine state ----
{
    echo "captured: $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "commit:   $(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo ' (+ uncommitted changes)')"
    echo "compiler: $(g++ --version | head -1)"
    if [[ -n "$exe" ]]; then
        powershell.exe -NoProfile -Command '
            $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
            $os  = Get-CimInstance Win32_OperatingSystem
            $bat = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
            "cpu:      $($cpu.Name.Trim()), $($cpu.NumberOfCores) cores / $($cpu.NumberOfLogicalProcessors) threads"
            "os:       $($os.Caption) build $($os.BuildNumber)"
            "power:    $((powercfg /getactivescheme) -join " ")"
            if ($bat) {
                $src = if ($bat.BatteryStatus -eq 1) { "ON BATTERY" } else { "AC (battery status $($bat.BatteryStatus))" }
                "source:   $src, charge $($bat.EstimatedChargeRemaining)%"
            } else { "source:   AC (no battery)" }' | tr -d '\r'
    else
        echo "cpu:      $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //'), $(nproc) logical CPUs visible"
        echo "os:       $(uname -sr)$(grep -qi microsoft /proc/version && echo ' (WSL2 virtual machine)')"
    fi
} > "$out/machine.txt"
cat "$out/machine.txt"

run() { # name, then benchmark arguments; a FAIL result is recorded, not fatal
    local name="$1"; shift
    echo "  $name"
    "$bin/bench_matching_latency$exe" "$@" > "$out/$name.txt" || true
}

for profile in thin deep; do
    for seed in 42 43 44 45 46; do
        run "${profile}_seed${seed}" --profile="$profile" --seed="$seed" \
            --csv="$out/seeds.csv" --outliers="$out/outliers_${profile}_seed${seed}.csv"
    done
    run "${profile}_repeat5" --profile="$profile" --seed=42 --repeat=5 --csv="$out/repeat.csv"
    run "${profile}_paced50k" --profile="$profile" --seed=42 --rate=50000 --repeat=3 \
        --csv="$out/paced.csv" --outliers="$out/outliers_${profile}_paced50k.csv"
done
"$bin/bench_engine$exe" > "$out/bench_engine.txt"
"$bin/bench_ring$exe" > "$out/bench_ring.txt"

grep -h "^RESULT" "$out"/*.txt | sort | uniq -c | sort -rn | head -3 || true
echo "wrote $out/"
