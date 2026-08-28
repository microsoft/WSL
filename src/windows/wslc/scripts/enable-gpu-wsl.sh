#!/bin/bash
# =============================================================================
# enable-gpu-wsl.sh — Expose an NVIDIA GPU to the WSL edge Kubernetes cluster
# -----------------------------------------------------------------------------
# Runs INSIDE the WSL Ubuntu distro that is your AKS Arc edge node. Automates
# Step 4 of docs/wsl/k8s/wsl-gpu-foundry-local.md:
#
#   1. Detect whether an NVIDIA GPU is projected into WSL (nvidia-smi -L).
#      - No GPU  -> print guidance and exit 0 (nothing to do, not an error).
#      - GPU     -> continue automatically.
#   2. Install nvidia-container-toolkit                             (4a)
#   3. Wire it into containerd as the default runtime, keep         (4b)
#      SystemdCgroup = true, restart containerd.
#   4. Deploy the NVIDIA k8s device plugin                          (4c)
#   5. Verify the node advertises nvidia.com/gpu                    (4d)
#   6. Smoke-test a GPU pod (nvidia-smi -L inside a container).
#
# Idempotent: every step checks "already done?" so re-runs resume safely.
# Safe on a LIVE cluster — it never runs `wsl --shutdown` (which would change
# WSL's eth0 IP and kill the kubeadm apiserver cert). The GPU must already be
# usable in WSL; if it isn't, enable it first per Steps 1-3 of the guide (that
# path DOES require a restart, so do it before creating the cluster).
#
# Usage:
#   ./enable-gpu-wsl.sh                 # detect + enable if GPU present
#   ./enable-gpu-wsl.sh --skip-smoke    # skip the GPU smoke-test pod
#   ./enable-gpu-wsl.sh                 # k8s (kubeadm) only
#   ./enable-gpu-wsl.sh --skip-smoke    # skip the GPU smoke-test pod
#   ./enable-gpu-wsl.sh --kubeconfig <path>   # non-default kubeadm admin.conf
#
# NOTE: k8s (kubeadm) only. k3s embeds its own containerd (different config path),
# so Step 4b does not apply; the script detects k3s and exits with guidance.
#
# This is POC-quality. NOT a supported product.
# =============================================================================
set -euo pipefail
umask 077

# -----------------------------------------------------------------------------
# Args & defaults
# -----------------------------------------------------------------------------
KUBECONFIG_PATH="${KUBECONFIG_PATH:-/etc/kubernetes/admin.conf}"
DEVICE_PLUGIN_VERSION="${DEVICE_PLUGIN_VERSION:-v0.17.1}"
SMOKE_IMAGE="${SMOKE_IMAGE:-nvcr.io/nvidia/cuda:12.6.2-base-ubuntu24.04}"
RUN_SMOKE=1

usage() { grep '^#' "$0" | sed 's/^# \{0,1\}//' | head -40; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --kubeconfig)     KUBECONFIG_PATH="${2:?}"; shift 2 ;;
    --plugin-version) DEVICE_PLUGIN_VERSION="${2:?}"; shift 2 ;;
    --smoke-image)    SMOKE_IMAGE="${2:?}"; shift 2 ;;
    --skip-smoke)     RUN_SMOKE=0; shift ;;
    -h|--help)        usage 0 ;;
    *) echo "ERROR: unknown arg '$1'" >&2; usage 1 ;;
  esac
done

log()  { echo ">>> $*"; }
warn() { echo "WARN: $*" >&2; }
die()  { echo "ERROR: $*" >&2; exit 1; }

# kubectl wrapper pinned to the edge cluster's kubeconfig (needs sudo to read it)
kc() { sudo kubectl --kubeconfig "$KUBECONFIG_PATH" "$@"; }

# -----------------------------------------------------------------------------
# Step 0 — Detect GPU in WSL
# -----------------------------------------------------------------------------
detect_gpu() {
  # No driver/tooling at all -> legitimate no-op (caller prints guidance, exits 0).
  command -v nvidia-smi >/dev/null 2>&1 || return 1
  # nvidia-smi is present: run it and keep stdout + exit status SEPARATE, so a
  # driver/NVML failure (e.g. "GPU access blocked by the OS", vGPU driver) is
  # surfaced as a hard error (return 2) instead of being silently mistaken for
  # "no GPU". Do NOT swallow the failure with `|| true`.
  local out rc
  out="$(nvidia-smi -L 2>&1)"; rc=$?
  if [[ $rc -ne 0 ]]; then
    printf '%s\n' "$out" >&2
    return 2
  fi
  [[ -n "$out" ]] || return 1        # present + healthy, but no GPU enumerated
  printf '%s\n' "$out"
  return 0
}

# -----------------------------------------------------------------------------
# Step 4a — nvidia-container-toolkit
# -----------------------------------------------------------------------------
install_container_toolkit() {
  if command -v nvidia-ctk >/dev/null 2>&1; then
    log "4a: nvidia-container-toolkit already installed ($(nvidia-ctk --version 2>/dev/null | head -1)) — skipping"
    return 0
  fi
  log "4a: installing nvidia-container-toolkit"
  curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
    | sudo gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
  curl -s -L https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
    | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' \
    | sudo tee /etc/apt/sources.list.d/nvidia-container-toolkit.list >/dev/null
  sudo apt-get update
  sudo apt-get install -y nvidia-container-toolkit
}

# -----------------------------------------------------------------------------
# Step 4b — wire nvidia runtime into containerd, keep SystemdCgroup = true
# -----------------------------------------------------------------------------
configure_containerd() {
  local main_cfg="/etc/containerd/config.toml"
  [[ -f "$main_cfg" ]] || die "containerd config not found at $main_cfg — is this the edge node?"

  log "4b: configuring containerd nvidia runtime (default)"
  sudo cp -n "$main_cfg" "${main_cfg}.bak" || true   # keep first backup only (idempotent)
  # nvidia-ctk writes either into config.toml or a conf.d/*.toml drop-in depending on version
  sudo nvidia-ctk runtime configure --runtime=containerd --set-as-default

  # SystemdCgroup MUST be true (kubelet uses the systemd cgroup driver). Newer
  # toolkits write a conf.d drop-in; fix BOTH the main config and any drop-in.
  local files=("$main_cfg")
  if [[ -d /etc/containerd/conf.d ]]; then
    while IFS= read -r f; do files+=("$f"); done < <(find /etc/containerd/conf.d -maxdepth 1 -name '*.toml' 2>/dev/null)
  fi
  for f in "${files[@]}"; do
    if grep -qi 'SystemdCgroup *= *false' "$f" 2>/dev/null; then
      warn "SystemdCgroup=false found in $f — setting to true"
      sudo sed -i 's/SystemdCgroup *= *false/SystemdCgroup = true/I' "$f"
    fi
  done

  # Verify the MERGED (config + drop-ins) view resolves SystemdCgroup = true
  if sudo containerd config dump 2>/dev/null | grep -i 'SystemdCgroup' | grep -qi 'false'; then
    die "containerd still resolves SystemdCgroup=false after fixup — refusing to restart (would break kubelet)"
  fi

  log "4b: restarting containerd"
  sudo systemctl restart containerd
  sleep 5
  # Node must stay Ready across the restart
  kc get nodes
}

# -----------------------------------------------------------------------------
# Step 4c — NVIDIA device plugin
# -----------------------------------------------------------------------------
deploy_device_plugin() {
  log "4c: deploying NVIDIA k8s device plugin ($DEVICE_PLUGIN_VERSION)"
  kc apply -f "https://raw.githubusercontent.com/NVIDIA/k8s-device-plugin/${DEVICE_PLUGIN_VERSION}/deployments/static/nvidia-device-plugin.yml"
}

# -----------------------------------------------------------------------------
# Step 4d — verify the node advertises GPUs
# -----------------------------------------------------------------------------
verify_gpu_advertised() {
  log "4d: waiting for the node to advertise nvidia.com/gpu (up to ~90s)"
  local node alloc
  node="$(kc get nodes -o jsonpath='{.items[0].metadata.name}')"
  for _ in $(seq 1 18); do
    alloc="$(kc get node "$node" -o jsonpath='{.status.allocatable.nvidia\.com/gpu}' 2>/dev/null || true)"
    if [[ -n "$alloc" && "$alloc" != "0" ]]; then
      log "4d: node '$node' advertises nvidia.com/gpu = $alloc"
      return 0
    fi
    sleep 5
  done
  die "node never advertised nvidia.com/gpu — check device plugin pod: kc -n kube-system get pods -l name=nvidia-device-plugin-ds"
}

# -----------------------------------------------------------------------------
# Smoke test — a pod runs nvidia-smi -L on the GPU
# -----------------------------------------------------------------------------
smoke_test() {
  [[ "$RUN_SMOKE" == "1" ]] || { log "smoke test skipped (--skip-smoke)"; return 0; }
  log "smoke test: launching gpu-smoke pod (nvidia-smi -L inside a container)"
  kc delete pod gpu-smoke --ignore-not-found >/dev/null 2>&1 || true
  kc run gpu-smoke --rm -i --restart=Never \
    --image="$SMOKE_IMAGE" \
    --overrides="{\"spec\":{\"containers\":[{\"name\":\"cuda\",\"image\":\"$SMOKE_IMAGE\",\"command\":[\"nvidia-smi\",\"-L\"],\"resources\":{\"limits\":{\"nvidia.com/gpu\":1}}}]}}"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
  log "Detecting NVIDIA GPU in WSL..."
  local gpus rc
  gpus="$(detect_gpu)" && rc=0 || rc=$?
  if [[ "$rc" == "2" ]]; then
    die "nvidia-smi is present but FAILED (see error above) — the GPU/driver is in a bad state (e.g. a vGPU/GRID host driver, or 'GPU access blocked by the OS'). This is NOT 'no GPU'; fix the host driver / GPU-PV before retrying. See docs/wsl/KNOWN-ISSUES-GPU.md."
  fi
  if [[ "$rc" != "0" ]]; then
    cat <<'EOF'
>>> No usable NVIDIA GPU detected in this WSL distro (`nvidia-smi` absent).
>>> Nothing to enable — exiting cleanly.
>>>
>>> If this machine HAS an NVIDIA GPU, enable GPU-PV into WSL first
>>> (Steps 1-3 of docs/wsl/k8s/wsl-gpu-foundry-local.md):
>>>   - install the NVIDIA Data Center (Tesla) DCH driver on the Windows host
>>>   - `wsl --update; wsl --shutdown; wsl`  (do this BEFORE creating the cluster)
>>>   - re-run this script once `nvidia-smi -L` lists your GPU(s) in WSL.
EOF
    exit 0
  fi
  log "GPU detected:"
  echo "$gpus" | sed 's/^/    /'

  # This script's containerd wiring (Step 4b) targets the SYSTEM containerd
  # (/etc/containerd/config.toml) used by kubeadm k8s. k3s embeds its own
  # containerd with a different config path, so this flow does NOT apply to k3s.
  if [[ "$KUBECONFIG_PATH" == *"/rancher/k3s/"* || -d /etc/rancher/k3s || -f /var/lib/rancher/k3s/agent/etc/containerd/config.toml ]]; then
    die "k3s detected — this script only wires the kubeadm (k8s) system containerd. k3s embeds its own containerd; GPU enablement for k3s needs a k3s-specific config and is not supported here yet."
  fi

  # Wait (bounded, ~5 min) for BOTH the kubeconfig to exist AND the API server to
  # answer — az aksarc create is async, so neither is guaranteed immediately.
  log "Waiting for the cluster (kubeconfig + API server) to become ready (up to ~5 min)..."
  local reachable=0
  for _ in $(seq 1 30); do
    if [[ -f "$KUBECONFIG_PATH" ]] && kc get nodes >/dev/null 2>&1; then reachable=1; break; fi
    sleep 10
  done
  [[ "$reachable" == "1" ]] || die "cluster not ready via $KUBECONFIG_PATH after ~5 min (kubeconfig present? node up?) — check 'az aksarc show' / kubelet, then re-run."

  install_container_toolkit   # 4a
  configure_containerd        # 4b
  deploy_device_plugin        # 4c
  verify_gpu_advertised       # 4d
  smoke_test

  log "Done — GPU is exposed to Kubernetes. Add 'resources.limits: {nvidia.com/gpu: 1}' to any pod."
  warn "WSL isolation caveat: a pod requesting 1 GPU still SEES all physical GPUs (GPU-PV via /dev/dxg)."
  warn "On multi-GPU hosts, apply the Step 8 pipefail/SIGPIPE fix before running Foundry Local model serving."
}

main "$@"
