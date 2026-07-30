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

**Working.** Host #1 is **nebula** (a Gentoo workstation, KVM + AMD-V): `lsa`
boots LoricaOS in real Firecracker to an interactive shell — you type, and
`vigil` → `stsh` → coreutils respond over the console:

```
aegis@loricaos:/$ uname
Aegis
aegis@loricaos:/$ ls /bin
cut  expand  ls  realpath  stsh  uname  …
```

Getting there took a small round of Aegis hardening for a truly bare VMM
(Firecracker emulates no VGA, no working 8042, no PIT, and rejects unmapped
I/O): a serial-only-console VGA probe, an 8042 fast-bail, serial-RX polling
(no legacy IRQs), and a one-page virtio-blk transfer cap. All landed upstream.

## Commands

```
lsa                        open a shell in the default distribution
lsa <cmd> [args...]        run one command, stream its output, propagate exit code
lsa -d <distro> [cmd...]   target a specific distribution
lsa install [<distro>]     download + register a distribution (default: loricaos)
lsa list | -l              list installed distributions ('*' = default)
lsa set-default <distro>   make <distro> the default
lsa cp <src> <dst>         copy a file in/out (one side is <distro>:/path)
lsa unregister <distro>    remove a distribution
lsa stop                   terminate any running micro-VM
lsa version | help
```

Each invocation boots a fresh micro-VM (~0.3 s) and tears it down on exit — no
daemon, no persistent VM to manage. Distributions are `.tar.gz` bundles of
`{ kernel.elf, rootfs.img, distro.conf }`; the CI [release workflow](.github/workflows/release-image.yml)
builds and publishes the `loricaos` image that `lsa install` fetches.

### File interop

`lsa cp` moves files through the distro's ext2 rootfs image with `debugfs` (no
9p/virtiofs needed — Firecracker has neither):

```
lsa cp ./setup.sh loricaos:/etc/setup.sh     # stage a file into the distro
lsa cp loricaos:/etc/motd ./motd             # pull a file out
```

Copy-*in* and copy-*out* both work, including files the guest just created:
`lsa sh -c "echo hi > /etc/note"` then `lsa cp loricaos:/etc/note ./note`.

### Networking (rootless — no root, no sudo, no host changes)

The guest gets internet with **zero privilege**. There is no TAP on the host, no
NAT rule, no firewall change, nothing to clean up:

```
$ lsa nettest                       # connectivity self-test (by IP)
[NETTEST] connecting to 1.1.1.1:80 ... CONNECTED
HTTP/1.1 301 Moved Permanently
Server: cloudflare

$ lsa httpget example.com            # DNS + fetch by hostname
httpget: example.com -> 104.18.27.120
HTTP/1.1 200 OK
Server: cloudflare
```

Hostnames resolve too: slirp answers DNS at `10.0.2.3`, the image ships an
`/etc/resolv.conf` pointing there, and musl's `getaddrinfo()` does the rest — so
the whole userland resolves names with no extra resolver.

How: each `lsa` launch runs Firecracker inside a private **user + network
namespace**. A bridge in that namespace joins Firecracker's tap to a
[`slirp4netns`](https://github.com/rootless-containers/slirp4netns) userspace
uplink, so the guest sits directly on slirp's `10.0.2.0/24` (the classic QEMU
user-mode model — no NAT/forwarding/netfilter). When the VM exits, the namespace
and everything in it evaporate. `setup.sh` fetches the `slirp4netns` binary
alongside Firecracker; if it's absent, `lsa` still boots, just without the NIC.
Disable per-launch with `LSA_NET=0`.

Getting there needed a small Aegis change: virtio-net rode a PCI/MSI-X-only path,
so it was invisible on the microVM's virtio-**mmio** transport. It now falls
through to the LAPIC-timer RX poll (already wired for the no-IOAPIC microVM), and
takes a static address from an `ip=` kernel-cmdline token. All upstream.

## Roadmap

- [x] `lsa` — boot LoricaOS to an interactive shell
- [x] `lsa <cmd>` — one-shot command execution (clean output + exit code)
- [x] fast boot (~0.3 s kernel init)
- [x] distribution registry + CI-published `loricaos` release artifact
- [x] lifecycle: fresh VM per call, `stop`, `version`
- [x] `lsa cp` file interop (copy-in; copy-out of existing files)
- [x] rootless networking — guest internet via a user-namespace + slirp4netns, no root
- [x] guest DNS — musl `getaddrinfo` via slirp's resolver; `lsa httpget <host>` fetches by name
- [x] correct argument passing — `lsa sh -c "…"` / multi-arg commands preserve quoting
- [ ] live shared filesystem, WSL-style `/mnt` (needs a QEMU `microvm` backend — Firecracker has no 9p/virtiofs)
