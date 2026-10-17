#!/usr/bin/env bash
set -uo pipefail
REPORT_DIR="${REPORT_DIR:-tests/out}"
mkdir -p "$REPORT_DIR"
LOG="$REPORT_DIR/run.log"
: > "$LOG"
SUMMARY="$REPORT_DIR/summary.md"
TOTAL=0
: > "${REPORT_DIR:-tests/out}/summary.md"
PASSED=0
FAILED=0
FAILED_LIST=""
record() {
  local name="$1" status="$2" detail="$3" dur="$4"
  TOTAL=$((TOTAL+1))
  if [ "$status" = "PASS" ]; then
    PASSED=$((PASSED+1))
  else
    FAILED=$((FAILED+1))
    FAILED_LIST="$FAILED_LIST,$name"
  fi
  printf "%s | %-28s | %-4s | %5ss | %s\n" "$(date +%H:%M:%S)" "$name" "$status" "$dur" "$detail" | tee -a "$LOG"
}
run_case() {
  local name="$1" cmd="$2" start end dur out status detail
  start=$(date +%s)
  out=$(eval "$cmd" 2>&1)
  status=$?
  end=$(date +%s)
  dur=$((end-start))
  if [ $status -eq 0 ]; then status=PASS; else status=FAIL; fi
  detail=$(printf "%s" "$out" | tail -2 | tr "\n" " " | cut -c1-120)
  record "$name" "$status" "$detail" "$dur"
  return 0
}
