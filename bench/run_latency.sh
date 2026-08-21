#!/usr/bin/env bash
#
# run_latency.sh — drive bench_latency under core pinning + SCHED_FIFO, repeat
# it N times, and report the run-to-run spread of p50/p99/max. A single bench
# run is one sample of a noisy process; what tells you whether a machine is
# quiet enough is how little those numbers move across repeats.
#
# The matching thread pins itself to MATCH_CORE (via the bench's cpu_core arg)
# and lifts itself to SCHED_FIFO. This script additionally:
#   - confines the whole process to {MATCH_CORE, BENCH_CORE} so the pacer and
#     drainer stay off every other core (and the matching thread's self-pin to
#     MATCH_CORE still succeeds, since that core is in the mask), and
#   - runs it under SCHED_FIFO so the pacer/drainer resist preemption too.
#
# For a genuinely stable tail, boot the isolated cores out of the scheduler:
#   isolcpus=MATCH,BENCH nohz_full=MATCH,BENCH rcu_nocbs=MATCH,BENCH
# On Apple Silicon / Asahi, MATCH_CORE and BENCH_CORE MUST be P-cores (highest
# cpu_capacity) or you get bimodal P-vs-E latency. This script prints the
# per-core capacities below so you can pick them.
#
# Usage:
#   bench/run_latency.sh [reps] [samples] [rate] [match_core] [bench_core]
# Defaults: reps=10 samples=500000 rate=500000 match_core=2 bench_core=3
# The bench binary is found from $BENCH or the first build dir that has it.

set -euo pipefail

REPS=${1:-10}
SAMPLES=${2:-500000}
RATE=${3:-500000}
MATCH_CORE=${4:-2}
BENCH_CORE=${5:-3}

# ── Locate the bench binary ───────────────────────────────────────────────────
BENCH=${BENCH:-}
if [[ -z "$BENCH" ]]; then
  for cand in build/bench_latency build-release/bench_latency \
              cmake-build-release/bench_latency; do
    if [[ -x "$cand" ]]; then BENCH="$cand"; break; fi
  done
fi
if [[ -z "$BENCH" || ! -x "$BENCH" ]]; then
  echo "error: bench_latency not found. Build it, or set BENCH=/path/to/bench_latency" >&2
  exit 1
fi

# ── Environment report (so each run is self-documenting) ──────────────────────
echo "== environment =="
echo "bench binary : $BENCH"
echo "kernel       : $(uname -sr)"
if [[ -r /proc/cmdline ]]; then
  echo "cmdline      : $(cat /proc/cmdline)"
fi
gov=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
[[ -r "$gov" ]] && echo "governor     : $(cat "$gov")" || echo "governor     : (no cpufreq control)"

# Per-core capacity: on big.LITTLE / Apple Silicon the P-cores have the higher
# value. Absent on uniform x86, in which case any core is fine.
echo "cpu capacity : (higher = performance core; pick MATCH/BENCH from the top)"
have_cap=0
for c in /sys/devices/system/cpu/cpu*/cpu_capacity; do
  [[ -r "$c" ]] || continue
  have_cap=1
  cpu=$(basename "$(dirname "$c")")
  printf "  %-7s %s\n" "$cpu" "$(cat "$c")"
done
[[ $have_cap -eq 0 ]] && echo "  (cpu_capacity not exported — uniform cores, any index is fine)"
echo "using        : match_core=$MATCH_CORE bench_core=$BENCH_CORE"

# ── Privilege check ────────────────────────────────────────────────────────────
# The matching thread elevates itself to SCHED_FIFO internally (see
# ConfigureThread() in matching_engine.cpp), which needs CAP_SYS_NICE / root.
# We deliberately do NOT wrap the whole process in chrt here: pthreads default
# to PTHREAD_INHERIT_SCHED, so a process-wide chrt would promote the pacer and
# drainer threads to the same SCHED_FIFO priority as the matching thread too.
# SCHED_FIFO does not time-slice equal-priority threads, and neither the
# pacer's busy-wait nor the matching thread's poll loop ever yields or blocks
# — so if two of those threads land on the same core under the taskset mask
# below, one starves the other forever (a livelock, not a crash). Running
# only the matching thread as SCHED_FIFO — while the pacer/drainer stay
# SCHED_OTHER — keeps normal CFS preemption in play for them, which prevents
# that starvation.
if ! chrt -f 80 true 2>/dev/null; then
  echo "warning: SCHED_FIFO unavailable (need root / CAP_SYS_NICE); running SCHED_OTHER" >&2
  echo "         -> re-run with sudo for a stable tail" >&2
fi
TASKSET=(taskset -c "${MATCH_CORE},${BENCH_CORE}")

# ── Repeat the bench, scraping p50/p99/max from each run ───────────────────────
p50s=(); p99s=(); maxs=()
echo
echo "== $REPS runs: samples=$SAMPLES rate=$RATE =="
for ((i = 1; i <= REPS; i++)); do
  out=$("${TASKSET[@]}" "$BENCH" "$SAMPLES" "$RATE" "$MATCH_CORE" "$BENCH_CORE")
  p50=$(awk '/^  p50 /{print $3}' <<<"$out")
  p99=$(awk '/^  p99 /{print $3}' <<<"$out")
  mx=$(awk '/^  max /{print $3}' <<<"$out")
  p50s+=("$p50"); p99s+=("$p99"); maxs+=("$mx")
  printf "  run %2d/%d : p50=%-8s p99=%-10s max=%s\n" "$i" "$REPS" "$p50" "$p99" "$mx"
done

# ── Aggregate: median / min / max of each metric across runs ───────────────────
summ() {  # args: label, then the values
  local label=$1; shift
  printf '%s\n' "$@" | sort -n | awk -v label="$label" '
    { v[NR] = $1 }
    END {
      n = NR
      med = (n % 2) ? v[(n+1)/2] : int((v[n/2] + v[n/2+1]) / 2)
      printf "  %-6s median=%-10d min=%-10d max=%-10d spread(max/min)=%.2fx\n",
             label, med, v[1], v[n], (v[1] > 0 ? v[n]/v[1] : 0)
    }'
}

echo
echo "== stability across $REPS runs (ns) =="
summ "p50" "${p50s[@]}"
summ "p99" "${p99s[@]}"
summ "max" "${maxs[@]}"
echo
echo "A tight p99 spread (~1.x) means the machine is quiet enough to trust the"
echo "tail. A large spread means something is still preempting the hot cores."
