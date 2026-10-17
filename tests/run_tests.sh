#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
source tests/framework.sh

echo "=== ACMDOG gate run $(date +%Y-%m-%dT%H:%M:%S) ==="
echo "trigger: ${GITHUB_EVENT_NAME:-local} commit: ${GITHUB_SHA:-$(git rev-parse --short HEAD 2>/dev/null)}"

run_case "workflow_yaml_valid" "python3 .github/scripts/check_workflow.py .github/workflows/acmdog-gate.yml"
run_case "lsm_binary_present" "test -f deploy/lsm/lsm_server"
run_case "lsm_binary_elf" "file deploy/lsm/lsm_server | grep -q ELF"
run_case "lsm_binary_hash" "sha256sum -c .github/lsm-server.sha256"
run_case "lsm_config_toml" "python3 .github/scripts/check_config.py deploy/lsm/config.toml"
run_case "compose_yaml_valid" "python3 .github/scripts/check_yaml.py middleware/docker-compose.yml nginx/docker-compose.yml"
run_case "compose_middleware" "docker compose -f middleware/docker-compose.yml config -q"
run_case "compose_nginx" "docker compose -f nginx/docker-compose.yml config -q"
run_case "nginx_config_syntax" "bash .github/scripts/check_nginx.sh"
run_case "cloud_judge_build" "cmake -S cloud_judge -B build/cloud_judge -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build build/cloud_judge -j4 > /dev/null"
run_case "lsm_business_smoke" "python3 tests/test_lsm_smoke.py > /dev/null"
run_case "e2e_full_chain" "python3 tests/e2e/test_full_chain.py > /dev/null"

echo ""
echo "=== SUMMARY ==="
FAILED_LIST="${FAILED_LIST#,}"
printf "total=%-3s passed=%-3s failed=%-3s\n" "$TOTAL" "$PASSED" "$FAILED"
if [ -n "$FAILED_LIST" ]; then echo "failed: $FAILED_LIST"; exit 1; fi
echo "result: ALL PASSED"
exit 0
