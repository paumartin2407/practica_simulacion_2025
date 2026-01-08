#!/usr/bin/env bash
set -euo pipefail

# Runs practica over multiple lambda values and all dispatcher/queue policy combinations
# Output: results/results.csv with columns:
# lambda,dispatch,queue,mean_system_time,mean_queue_size,mean_in_system,tasks

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BIN="./practica"
PLATFORM="platform-cluster.xml"

# Ensure binary exists; build if missing
if [[ ! -x "$BIN" ]]; then
  echo "[info] Building project..."
  make -j
fi

mkdir -p results
OUT_FILE="results/results.csv"

echo "lambda,dispatch,queue,mean_system_time,mean_queue_size,mean_in_system,tasks" > "$OUT_FILE"

lambdas=(0.2 0.5 0.7 0.9)
dispatchers=(
  random
  sqf
  rr
  two-random-choices
  two-rr-random-choices
)
queues=(
  fcfs
  sjf
  ljf
)

run_one() {
  local lam="$1"; shift
  local disp="$1"; shift
  local queue="$1"; shift

  echo "[run] lambda=$lam dispatch=$disp queue=$queue" >&2
  # Execute and capture output
  local output
  if ! output=$($BIN "$PLATFORM" "$lam" "$disp" "$queue"); then
    echo "[error] run failed: lambda=$lam dispatch=$disp queue=$queue" >&2
    return 1
  fi
  # Take the last line, which contains the numeric results
  local values
  values=$(printf '%s\n' "$output" | tail -n 1)

  # Parse numbers: tiempoMedioServicio, TamañoMediocola, TareasMediasEnElSistema, tareas
  # shellcheck disable=SC2206
  local fields=( $values )
  if [[ ${#fields[@]} -lt 4 ]]; then
    echo "[warn] unexpected output format for lambda=$lam dispatch=$disp queue=$queue: '$values'" >&2
    return 1
  fi
  local mean_system_time="${fields[0]}"
  local mean_queue_size="${fields[1]}"
  local mean_in_system="${fields[2]}"
  local tasks="${fields[3]}"

  echo "$lam,$disp,$queue,$mean_system_time,$mean_queue_size,$mean_in_system,$tasks" >> "$OUT_FILE"
}

for lam in "${lambdas[@]}"; do
  for disp in "${dispatchers[@]}"; do
    for queue in "${queues[@]}"; do
      run_one "$lam" "$disp" "$queue"
    done
  done
  echo "[done] lambda=$lam" >&2
done

echo "[ok] Results saved to $OUT_FILE" >&2
