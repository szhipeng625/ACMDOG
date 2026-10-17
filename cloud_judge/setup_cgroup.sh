#!/bin/bash
# ExecStartPre: set up cgroup subtree BEFORE main process starts.
# Runs as root (ExecStartPre ignores User= directive).
set -e

CG_ROOT="/sys/fs/cgroup/system.slice/cloud_judge.service"

# Wait for systemd to create the delegate cgroup directory
for i in $(seq 1 50); do
    [ -f "$CG_ROOT/cgroup.subtree_control" ] && break
    sleep 0.1
done

if [ ! -f "$CG_ROOT/cgroup.subtree_control" ]; then
    echo "[setup_cgroup] ERROR: $CG_ROOT not ready after 5s" >&2
    exit 1
fi

# Enable controllers on the subtree root (no child processes yet → will succeed)
echo "+cpu +memory +pids" > "$CG_ROOT/cgroup.subtree_control" || true

# Pre-create data_prep cgroup with low CPU weight
mkdir -p "$CG_ROOT/data_prep"
echo "50" > "$CG_ROOT/data_prep/cpu.weight" 2>/dev/null || true

# Pre-create worker cgroup (main process will be placed here by systemd or code)
mkdir -p "$CG_ROOT/worker"

echo "[setup_cgroup] done: subtree_control=$(cat "$CG_ROOT/cgroup.subtree_control"), data_prep cpu.weight=$(cat "$CG_ROOT/data_prep/cpu.weight" 2>/dev/null || echo N/A)"
