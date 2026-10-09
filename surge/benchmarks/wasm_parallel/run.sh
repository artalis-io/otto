#!/usr/bin/env bash
# K-fan-out portfolio + iterations->quality sweep for the "Surge as a WASM
# worker" question. Native (surge_run_native) vs WASM (surge_run.js under node),
# public Li & Lim instances only. See README.md. Build the two binaries first:
#   cc ... surge_run.c -lsurge -larbor -lshared -o surge_run_native   (with -DSG_HAS_THREADS, matching libsurge.a)
#   emcc -sNODERAWFS -sEXIT_RUNTIME surge_run.c <surge/arbor/shared srcs> -o surge_run.js
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SURGE=$(cd "$HERE/../.." && pwd)
NATIVE="$HERE/surge_run_native"
WASM="$HERE/surge_run.js"
INST="$SURGE/benchmarks/li_lim"
K=${K:-4}
CURVE_ITERS=${CURVE_ITERS:-"2500 5000 10000 20000"}
FAN_ITERS=${FAN_ITERS:-10000}
TL=${TL:-120}
INSTANCES=${INSTANCES:-"lc101 lr101 lr201"}
CSV="$HERE/results.csv"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

[ -x "$NATIVE" ] || { echo "missing $NATIVE"; exit 1; }
[ -f "$WASM" ]  || { echo "missing $WASM"; exit 1; }

echo "instance,mode,phase,K,iters,seed,veh,dist,unassigned,run_iters,elapsed_s" > "$CSV"

# solve <mode> <inst> <iters> <seed> -> "veh dist unassigned run_iters elapsed"
solve() {
  local mode=$1 inst=$2 iters=$3 seed=$4 out
  if [ "$mode" = native ]; then
    out=$("$NATIVE" "$INST/$inst.txt" --iterations "$iters" --seed "$seed" --time-limit "$TL" 2>/dev/null)
  else
    out=$(node "$WASM" "$INST/$inst.txt" --iterations "$iters" --seed "$seed" --time-limit "$TL" 2>/dev/null | tail -1)
  fi
  awk '{for(i=1;i<=NF;i++){split($i,a,"=");v[a[1]]=a[2]}
        printf "%s %s %s %s %s", v["veh"],v["dist"],v["unassigned"],v["iters"],v["elapsed"]}' <<<"$out"
}

echo "== Phase A: iterations -> quality (native single, seed 42) =="
printf "  %-8s %-7s %-4s %-11s %s\n" inst iters veh dist sec
for inst in $INSTANCES; do
  [ -f "$INST/$inst.txt" ] || { echo "  skip $inst (missing)"; continue; }
  for it in $CURVE_ITERS; do
    read -r veh dist un ri el <<<"$(solve native "$inst" "$it" 42)"
    echo "$inst,native,curve,1,$it,42,$veh,$dist,$un,$ri,$el" >> "$CSV"
    printf "  %-8s %-7s %-4s %-11s %s\n" "$inst" "$it" "$veh" "$dist" "$el"
  done
done

echo "== Phase B: K=$K fan-out best-of, native vs WASM, iters=$FAN_ITERS =="
for inst in $INSTANCES; do
  [ -f "$INST/$inst.txt" ] || continue
  nmean=0
  for mode in native wasm; do
    for s in $(seq 0 $((K-1))); do ( solve "$mode" "$inst" "$FAN_ITERS" "$s" > "$TMP/$mode.$s" ) & done
    wait
    best=""; sum=0; maxel=0
    for s in $(seq 0 $((K-1))); do
      read -r veh dist un ri el < "$TMP/$mode.$s"
      echo "$inst,$mode,fanout,$K,$FAN_ITERS,$s,$veh,$dist,$un,$ri,$el" >> "$CSV"
      sum=$(awk -v a="$sum" -v b="$el" 'BEGIN{print a+b}')
      maxel=$(awk -v a="$maxel" -v b="$el" 'BEGIN{print (b>a)?b:a}')
      best+="$veh $dist $un"$'\n'
    done
    read -r bv bd bu <<<"$(printf '%s' "$best" | sort -k3,3n -k1,1n -k2,2n | head -1)"
    mean=$(awk -v s="$sum" -v k="$K" 'BEGIN{printf "%.3f", s/k}')
    echo "$inst,$mode,best,$K,$FAN_ITERS,-,$bv,$bd,$bu,-,$maxel" >> "$CSV"
    printf "  %-8s %-6s best-of-%s: veh=%-3s dist=%-11s mean/run=%ss wall=%ss\n" "$inst" "$mode" "$K" "$bv" "$bd" "$mean" "$maxel"
    [ "$mode" = native ] && nmean=$mean
    [ "$mode" = wasm ] && awk -v n="$nmean" -v w="$mean" 'BEGIN{if(n>0)printf "  %-8s -> WASM per-run slowdown x%.2f\n","'"$inst"'",w/n}'
  done
done
echo; echo "full CSV -> $CSV"
