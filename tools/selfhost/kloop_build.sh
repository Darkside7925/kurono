#!/bin/sh
# kloop's build step inside the Alpine guest, pushed in by `vm kloop` (satoru)
#   sh kloop_build.sh [clean]
# the tree lives on the guest disk (/root/kb), so the next kloop only rebuilds
# what changed. src.tar is unpacked into a staging dir and rsync -c copies only
# the files whose CONTENT differs; they get a fresh mtime, so make sees them
# (also a file reverted to an older version, which a plain tar x would hide).
# the Makefile writes no header dependency files, so a changed header,
# Makefile, linker script or asm include means a clean build. (satoru)
R=${KLOOP_ROOT:-/root}   # (a host-side test points this elsewhere) (satoru)
T=$R/kb
S=$R/kstage
mkdir -p $T
rm -rf $S $R/kchanged && mkdir -p $S || exit 90
kxfer get src.tar $R/kstage.tar >/dev/null || exit 91
tar xf $R/kstage.tar -C $S || exit 92
rm -f $R/kstage.tar
rsync -rlc --delete --exclude=/build --out-format='%n' $S/ $T/ > $R/kchanged || exit 93
rm -rf $S
N=$(grep -vc '/$' $R/kchanged)
if [ "$1" = clean ] || [ ! -f $T/build/kurono_base.elf ] ||
   grep -qE '\.(h|hpp|inc|ld|mk)$|Makefile$' $R/kchanged; then
    echo "kloop-build: clean build ($N files changed)"
    rm -rf $T/build
else
    echo "kloop-build: incremental build ($N files changed)"
fi
cd $T/src || exit 94
make -j$(nproc) KURONO_KERNEL_BASE=0x14000000 ../build/kurono_base.elf > $R/kmake.log 2>&1
r=$?
kxfer put $R/kmake.log kmake.log >/dev/null
[ $r = 0 ] || { tail -5 $R/kmake.log; exit $r; }
cd $T/build && objcopy --strip-debug kurono_base.elf kguest.elf &&
    kxfer put kguest.elf kguest.elf >/dev/null && sha256sum kurono_base.elf
