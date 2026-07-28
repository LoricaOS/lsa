#!/bin/sh
# setup.sh — provision an LSA host.
#
# Installs Firecracker and stages the LoricaOS guest (kernel + rootfs) into
# LSA_HOME (~/.lsa by default), so `lsa` can boot it. Idempotent.
#
# The guest artifacts (kernel.elf, rootfs.img) are produced by the LoricaOS
# build (the `microvm` kernel tier + a server rootfs); point at them with
# LSA_KERNEL_SRC / LSA_ROOTFS_SRC, or drop them into $LSA_HOME yourself.
set -eu

LSA_HOME="${LSA_HOME:-$HOME/.lsa}"
FC_VERSION="${FC_VERSION:-v1.16.1}"
ARCH="$(uname -m)"

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

# ── Guest kernel + rootfs ────────────────────────────────────────────────────
for pair in "kernel.elf:${LSA_KERNEL_SRC:-}" "rootfs.img:${LSA_ROOTFS_SRC:-}"; do
    dst="$LSA_HOME/${pair%%:*}"; src="${pair#*:}"
    if [ -n "$src" ]; then
        echo "[setup] staging $dst from $src"
        cp "$src" "$dst"
    elif [ ! -e "$dst" ]; then
        echo "[setup] NOTE: $dst missing — build it from LoricaOS (microvm kernel"
        echo "        tier + a server rootfs) and set LSA_KERNEL_SRC / LSA_ROOTFS_SRC,"
        echo "        or copy it into $LSA_HOME."
    fi
done

# ── KVM access ───────────────────────────────────────────────────────────────
if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    echo "[setup] WARNING: /dev/kvm not read/writable by you — add yourself to the"
    echo "        'kvm' group (usermod -aG kvm \$USER) and re-log."
fi

echo "[setup] done. Run: $(dirname "$0")/lsa"
