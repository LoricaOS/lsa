# Guest — the LoricaOS image LSA boots

LSA needs two artifacts, produced by the LoricaOS build, staged into `$LSA_HOME`:

- **`kernel.elf`** — the Aegis kernel, `microvm` tier. Build in the [Aegis](https://github.com/LoricaOS/Aegis) tree:
  ```sh
  make microvm_defconfig && make      # -> build/aegis.elf
  ```
  It's PVH-bootable (the ELF note) and speaks virtio-mmio, so Firecracker boots
  it directly. CONFIG_PCI and CONFIG_ACPI are off; the serial console works by
  polling (Firecracker routes no legacy IRQs).

- **`rootfs.img`** — an ext2 image with the LoricaOS userland: `/bin/vigil`
  (init), a shell, coreutils, and a console service that starts a shell. Build
  it with the **baseline** musl toolchain (`tools/build-musl.sh` pins
  `-march=x86-64`) — a native-CPU build bakes in AVX the microVM CPU can't run.

`host/setup.sh` stages both (see `LSA_KERNEL_SRC` / `LSA_ROOTFS_SRC`).
