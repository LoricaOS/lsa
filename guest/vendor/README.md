# Vendored build dependencies

- **kconf.c** — the Kconfig engine from [LoricaOS/kconfig](https://github.com/LoricaOS/kconfig),
  vendored so `build-image.sh` (and the CI workflow) build the Aegis kernel
  without checking out that (currently private) repo. Aegis invokes `kconf` for
  `defconfig`/`syncconfig`; it's a single dependency-free C file. Re-sync from
  upstream when it changes.
