#!/bin/sh
# setup.sh — provision an LSA host: install Firecracker, then the default distro.
#
# After this, `lsa` boots you into LoricaOS. Idempotent.
set -eu

LSA_HOME="${LSA_HOME:-$HOME/.lsa}"
FC_VERSION="${FC_VERSION:-v1.16.1}"
ARCH="$(uname -m)"
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$LSA_HOME"

# ── Firecracker ──────────────────────────────────────────────────────────────
if [ ! -x "$LSA_HOME/firecracker" ]; then
    echo "[setup] installing Firecracker $FC_VERSION ($ARCH)"
    url="https://github.com/firecracker-microvm/firecracker/releases/download/${FC_VERSION}/firecracker-${FC_VERSION}-${ARCH}.tgz"
    tmp="$(mktemp -d)"
    curl -fsSL "$url" | tar -xz -C "$tmp"
    cp "$tmp/release-${FC_VERSION}-${ARCH}/firecracker-${FC_VERSION}-${ARCH}" "$LSA_HOME/firecracker"
    chmod +x "$LSA_HOME/firecracker"
    rm -rf "$tmp"
fi
"$LSA_HOME/firecracker" --version | head -1

# ── slirp4netns (rootless network uplink) ────────────────────────────────────
# Gives the guest internet with no root, no TAP on the host, no firewall rules.
# Optional: without it `lsa` still boots, just without networking.
SLIRP_VERSION="${SLIRP_VERSION:-v1.3.1}"
if [ ! -x "$LSA_HOME/slirp4netns" ] && [ "$ARCH" = x86_64 ]; then
    echo "[setup] installing slirp4netns $SLIRP_VERSION (rootless networking)"
    url="https://github.com/rootless-containers/slirp4netns/releases/download/${SLIRP_VERSION}/slirp4netns-${ARCH}"
    if curl -fsSL "$url" -o "$LSA_HOME/slirp4netns"; then
        chmod +x "$LSA_HOME/slirp4netns"
    else
        echo "[setup] WARNING: slirp4netns download failed — networking will be off"
        rm -f "$LSA_HOME/slirp4netns"
    fi
fi

# ── KVM access ───────────────────────────────────────────────────────────────
if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    echo "[setup] WARNING: /dev/kvm not read/writable by you — add yourself to the"
    echo "        'kvm' group (usermod -aG kvm \$USER) and re-log."
fi

# ── Default distribution ─────────────────────────────────────────────────────
echo "[setup] installing the default distribution (loricaos)"
"$HERE/lsa" install loricaos

echo "[setup] done. Run: $HERE/lsa"
