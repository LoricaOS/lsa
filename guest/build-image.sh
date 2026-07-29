#!/bin/bash
# build-image.sh — produce the "loricaos" LSA distribution artifact.
#
# Output: loricaos-x86_64.tar.gz = { kernel.elf, rootfs.img, distro.conf }, the
# bundle `lsa install loricaos` downloads and boots.
#
# Builds from sibling checkouts (override with env):
#   AEGIS   the Aegis kernel tree   (LoricaOS/Aegis)
#   LORICA  the LoricaOS tree        (LoricaOS/LoricaOS)  — vigil, stsh
#   CORE    the coreutils tree       (LoricaOS/coreutils) — the mux binary
#
# Requires: the kernel cross-toolchain, the LoricaOS baseline musl-gcc
# (tools/build-musl.sh — pins -march=x86-64), mke2fs + debugfs.
set -euo pipefail
export PATH="$PATH:/sbin:/usr/sbin"    # mke2fs/debugfs live here on most distros
HERE="$(cd "$(dirname "$0")" && pwd)"

# Kernel toolchain: bare cross by default; a CI runner overrides to its stock
# gnu toolchain (the freestanding kernel builds fine with either).
KCC="${KCC:-x86_64-elf-gcc}"; KLD="${KLD:-x86_64-elf-ld}"
KOBJCOPY="${KOBJCOPY:-x86_64-elf-objcopy}"; KNM="${KNM:-x86_64-elf-nm}"

AEGIS="${AEGIS:-$HOME/Developer/LoricaOS/aegis}"
LORICA="${LORICA:-$HOME/Developer/LoricaOS/loricaos}"
CORE="${CORE:-$HOME/Developer/coreutils}"
OUT="${OUT:-$PWD/loricaos-x86_64.tar.gz}"
VERSION="${VERSION:-$(cat "$AEGIS/VERSION" 2>/dev/null || echo 0.0.0)}"
MG="$LORICA/build/musl-dynamic/usr/bin/musl-gcc"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/stage"; mkdir -p "$STAGE"

# aegis drives its config with `kconf` (a separate repo, LoricaOS/kconfig). Build
# the vendored copy so the build is self-contained (no private-repo checkout).
KCONF="${KCONF:-$WORK/kconf}"
[ -x "$KCONF" ] || cc -O2 -o "$WORK/kconf" "$HERE/vendor/kconf.c"

echo "[img] kernel: aegis microvm tier"
# Pass the toolchain to BOTH invocations — the Makefile resolves the gcc include
# path at parse time, so even `microvm_defconfig` fails if CC points at an
# absent cross-gcc.
KT="CC=$KCC LD=$KLD OBJCOPY=$KOBJCOPY NM=$KNM KCONF=$KCONF"
( cd "$AEGIS" && make $KT microvm_defconfig >/dev/null && make -j"$(nproc)" $KT >/dev/null )
cp "$AEGIS/build/aegis.elf" "$STAGE/kernel.elf"

echo "[img] userland: vigil + stsh + coreutils (baseline musl)"
[ -x "$MG" ] || { echo "[img] baseline musl-gcc missing — run $LORICA/tools/build-musl.sh"; exit 1; }
"$MG" -static -O2 -o "$WORK/vigil" "$LORICA/user/bin/vigil/main.c"
"$MG" -static -O2 -I"$LORICA/user/bin/stsh" -o "$WORK/stsh" "$LORICA/user/bin/stsh"/*.c
( cd "$CORE" && make clean >/dev/null 2>&1 || true; make mux MUSL_CC="$MG" \
    MUX_CFLAGS="-static -O2 -s -Wl,--build-id=none" >/dev/null )
cp "$CORE/mux/cu" "$WORK/cu"
APPLETS="$(cat "$CORE/mux/applets")"

# lsa-exit: reboot(cmd=1) -> Firecracker exits. Lets `lsa <cmd>` return.
cat > "$WORK/lsa-exit.c" <<'C'
#include <unistd.h>
int main(void){ syscall(169, 1, 0, 0); return 0; }   /* SYS_reboot, kbd reset */
C
"$MG" -static -O2 -o "$WORK/lsa-exit" "$WORK/lsa-exit.c"

# lsa-entry: the console service. Run a one-off command (lsa <cmd>, via the
# lsa.exec= cmdline token) then exit; otherwise open an interactive shell.
cat > "$WORK/lsa-entry" <<'SH'
#!/bin/sh
enc=$(sed -n 's/.*lsa\.exec=\([^ ]*\).*/\1/p' /proc/cmdline)
if [ -n "$enc" ]; then
    printf '%s' "$enc" | /bin/base64 -d > /run/lsa.cmd
    /bin/sh /run/lsa.cmd; rc=$?
    /bin/sync
    /bin/lsa-exit    # reboot -> Firecracker exits; if denied, fall through
    exit $rc
fi
exec /bin/stsh
SH

echo "[img] rootfs.img (ext2)"
IMG="$WORK/rootfs.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mke2fs -t ext2 -F -b 4096 -L loricaos "$IMG" >/dev/null 2>&1
printf '/bin/lsa-entry' > "$WORK/svc-run"
printf 'respawn'        > "$WORK/svc-policy"
printf 'Welcome to LoricaOS %s (LSA)\n' "$VERSION" > "$WORK/motd"
{
  echo "mkdir /bin"
  for f in vigil stsh cu lsa-exit; do echo "write $WORK/$f /bin/$f"; echo "set_inode_field /bin/$f mode 0100755"; done
  echo "write $WORK/lsa-entry /bin/lsa-entry"; echo "set_inode_field /bin/lsa-entry mode 0100755"
  for a in $APPLETS "["; do echo "ln /bin/cu /bin/$a"; done
  echo "mkdir /etc"; echo "write $WORK/motd /etc/motd"
  echo "mkdir /etc/vigil"; echo "mkdir /etc/vigil/services"; echo "mkdir /etc/vigil/services/console"
  echo "write $WORK/svc-run /etc/vigil/services/console/run"
  echo "write $WORK/svc-policy /etc/vigil/services/console/policy"
  echo "mkdir /tmp"; echo "mkdir /run"; echo "mkdir /proc"
} > "$WORK/dbg"
debugfs -w -f "$WORK/dbg" "$IMG" >/dev/null 2>&1
cp "$IMG" "$STAGE/rootfs.img"

cat > "$STAGE/distro.conf" <<CONF
name=loricaos
version=$VERSION
arch=x86_64
mem_mib=512
vcpus=1
boot_args=boot=text
CONF

echo "[img] packing $OUT"
tar -C "$STAGE" -czf "$OUT" kernel.elf rootfs.img distro.conf
echo "[img] done: $OUT ($(wc -c < "$OUT") bytes, loricaos $VERSION, $(echo "$APPLETS" | wc -w) applets)"
