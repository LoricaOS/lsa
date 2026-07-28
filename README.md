# LSA — Linux Subsystem for Aegis

Run **[LoricaOS](https://github.com/LoricaOS/LoricaOS)** — the capability-based OS
built on the from-scratch [Aegis](https://github.com/LoricaOS/Aegis) kernel — as a
lightweight subsystem *on* a Linux host. The WSL model, inverted: where WSL2 runs
a Linux kernel in a fast micro-VM on Windows, **LSA runs the Aegis kernel in a
micro-VM on Linux**, and drops you into a LoricaOS shell.

```
$ lsa
   _______ _______  ______ _____ _______
   |_____| |______ |  ____   |   |______
   |     | |______ |_____| __|__ ______|
   Aegis "Ambient Argus"
aegis@loricaos:/$ uname
Aegis
```

## Why a micro-VM, not a compat layer

LoricaOS' whole point is the kernel: a capability-checked syscall boundary with
**no ambient authority**. Its userland binaries are ordinary static musl ELFs and
would *run* on a Linux kernel directly — but they'd run under Linux's security
model, not Aegis'. So LSA boots the **real Aegis kernel** (where the capability
model is actually enforced) in a KVM micro-VM, exactly the way it runs on bare
metal or in the cloud. You get the genuine article, isolated, in milliseconds.

This is a direct payoff of Aegis' micro-VM support: **PVH direct boot** (no
bootloader), **virtio-mmio** transport, and a build with no PCI and no ACPI —
i.e. the `microvm` tier. LSA is that tier, made a first-class thing you launch
from your Linux prompt.

## Backend

**[Firecracker](https://firecracker-microvm.github.io/)** — the same minimal KVM
VMM that runs serverless workloads: a single static binary, ~a few MB of overhead,
sub-second boots. Aegis speaks Firecracker's boot (PVH) and device (virtio-mmio)
protocols natively. (QEMU `-machine microvm` works too and is the fallback where
KVM/Firecracker isn't available.)

## Layout

```
host/     the `lsa` launcher + host-side setup that runs on the Linux host
guest/    how the LoricaOS guest (kernel + rootfs) is built/fetched
config/   Firecracker machine templates
```

## Status

Early. Host #1 is **nebula** (a Gentoo workstation, KVM + AMD-V). See `host/`.

## Roadmap

- [ ] `lsa` — boot LoricaOS to an interactive shell (v1)
- [ ] `lsa run <cmd>` — one-shot command execution
- [ ] shared filesystem with the host (virtio-9p, WSL-style `/mnt`)
- [ ] fast, snappy boot (fix micro-VM timer calibration; Firecracker has no PIT)
- [ ] networking passthrough
- [ ] `lsa` lifecycle: persistent VM, `stop`/`status`
