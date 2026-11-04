#!/usr/bin/env bash
# Install the standalone (no KernelSU) SUSFS supercall bridge into a kernel
# tree that already has the regular SUSFS patch applied.
set -euo pipefail

KERNEL_ROOT="${1:-}"
if [ -z "$KERNEL_ROOT" ] || [ ! -d "$KERNEL_ROOT/common/fs" ]; then
  echo "usage: $0 <kernel-root>" >&2
  exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
FS_DIR="$KERNEL_ROOT/common/fs"
KCONFIG="$FS_DIR/Kconfig"
MAKEFILE="$FS_DIR/Makefile"

if [ ! -f "$FS_DIR/susfs.c" ]; then
  echo "SUSFS patch is not applied (missing $FS_DIR/susfs.c)" >&2
  exit 1
fi

cp "$SRC_DIR/susfs_standalone.c" "$FS_DIR/susfs_standalone.c"

MARKER="# SUSFS standalone bridge (no KernelSU)"
if grep -qF "$MARKER" "$KCONFIG"; then
  echo "SUSFS standalone Kconfig already present"
else
  {
    echo ""
    echo "$MARKER"
    cat "$SRC_DIR/Kconfig"
  } >> "$KCONFIG"
  echo "Added SUSFS standalone Kconfig"
fi

if ! grep -q "susfs_standalone.o" "$MAKEFILE"; then
  cat "$SRC_DIR/Makefile.fragment" >> "$MAKEFILE"
  echo "Added susfs_standalone.o to fs/Makefile"
fi

# The SUSFS patch declares KernelSU preinit hooks in fs/exec.c; they are not
# needed (and not implemented) without KernelSU.
if ! grep -q "KSU_NO_PREINIT" "$MAKEFILE"; then
  echo "CFLAGS_exec.o += -DKSU_NO_PREINIT" >> "$MAKEFILE"
  echo "Enabled KSU_NO_PREINIT for fs/exec.o"
fi

echo "SUSFS standalone bridge installed"
