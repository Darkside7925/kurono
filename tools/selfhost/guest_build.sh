#!/bin/sh
# guest_build.sh SRC_URL OUT_URL - build Kurono's kernel inside the Alpine guest. (satoru)
#
# runs in the alpine vm (from kurono: `vm run sh /root/guest_build.sh ...`,
# progress with `vm exec tail /tmp/kbuild.log`). installs the toolchain once
# (build-base + nasm), unpacks the source tarball into a tmpfs (the 3 GB
# ALPINE.IMG root is ~98% full; give the vm kurono.vmram=3008), builds
# ../build/kurono_base.elf and PUTs it back with curl.
#   SRC_URL  a tar of src/ Alpine/ kld/ tools/ logo.h pciids.h (git ls-files)
#   OUT_URL  where to PUT the elf (a directory url, the name is appended)
# busybox wget --post-file stops at the first NUL byte, so the upload uses curl. (satoru)
SRC_URL=$1; OUT_URL=$2
exec > /tmp/kbuild.log 2>&1
set -x
apk add build-base nasm curl
# the makefile builds the small user programs with musl-gcc; alpine's gcc IS a
# musl gcc, so a wrapper is enough (the specs file only has to exist) (satoru)
[ -x /usr/local/bin/musl-gcc ] || { printf '#!/bin/sh\nexec gcc "$@"\n' > /usr/local/bin/musl-gcc; chmod +x /usr/local/bin/musl-gcc; }
mkdir -p /usr/lib/x86_64-linux-musl; [ -f /usr/lib/x86_64-linux-musl/musl-gcc.specs ] || : > /usr/lib/x86_64-linux-musl/musl-gcc.specs
mkdir -p /root/k
mountpoint -q /root/k || mount -t tmpfs -o size=2000m tmpfs /root/k
cd /root/k && rm -rf ./*
wget -q -O - "$SRC_URL" | tar xf - || { echo KBUILD_FAIL fetch; exit 1; }
cd src
T0=$(date +%s)
make -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 ../build/kurono_base.elf > /tmp/kmake.log 2>&1
RC=$?
echo "make rc=$RC secs=$(( $(date +%s) - T0 ))"
[ $RC = 0 ] || { tail -20 /tmp/kmake.log; echo KBUILD_FAIL make; exit 1; }
sha256sum ../build/kurono_base.elf
[ -n "$OUT_URL" ] && curl -s -T ../build/kurono_base.elf "$OUT_URL/kurono_base.elf"
echo KBUILD_DONE
