#!/bin/bash
# pack_firmware - build the KURONO firmware store for one driver family from
# the host's /lib/firmware, using the driver's OWN id/cfg table.
#
#   pack_firmware.sh <family> [--device vvvv:dddd] [--file NAME]... <outdir>
#
# with --device only the files that card can request are packed (every API
# version in the cfg's min..max that exists on the host, the pnvm files, plus
# regulatory.db). without it, every card in the family's table.
#
# The KURONO partition is FAT32 and the kernel's existing FAT32 reader has no
# long names, so files are stored under unique 8.3 names and MANIFEST.TXT maps
# the exact driver name to the store name, size and sha256:
#
#   <outdir>/firmware/<family>/MANIFEST.TXT
#   <outdir>/firmware/<family>/FWnnnnnn.BIN
#   <outdir>/firmware/REGULATO.DB        (regulatory.db)
#   <outdir>/firmware/REGULATO.P7S       (regulatory.db.p7s)
#
# Copies the result onto a FAT32 KURONO partition with mtools. Nothing is
# downloaded. Prints the total size.
#
# env: LINUX (configured tree), FW_DIR (default /lib/firmware), KLD_BUILD
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
FW_DIR=${FW_DIR:-/lib/firmware}
BUILD=${KLD_BUILD:-$ROOT/kld/build}
FAMILY=
DEVICE=
OUTDIR=
FILES=()

while [ $# -gt 0 ]; do
    case "$1" in
        --device) DEVICE=${2:-}; shift 2 ;;
        --file)   FILES+=("${2:-}"); shift 2 ;;
        --help|-h)
            sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*)
            echo "pack_firmware: unknown option $1" >&2; exit 1 ;;
        *)
            if [ -z "$FAMILY" ]; then FAMILY=$1
            elif [ -z "$OUTDIR" ]; then OUTDIR=$1
            else echo "pack_firmware: extra argument $1" >&2; exit 1; fi
            shift ;;
    esac
done

[ -n "$FAMILY" ] && [ -n "$OUTDIR" ] || {
    echo "usage: pack_firmware.sh <family> [--device vvvv:dddd] [--file NAME]... <outdir>" >&2
    exit 1
}
[ -d "$FW_DIR" ] || { echo "pack_firmware: $FW_DIR missing" >&2; exit 1; }
command -v python3 >/dev/null || { echo "pack_firmware: python3 not found" >&2; exit 1; }

FWOUT=
if [ ${#FILES[@]} -eq 0 ]; then
    # the driver's own data through the --kld-fw host run.
    if [ -z "${LINUX:-}" ]; then
        for c in "$ROOT/../third_party/linux-$KVER" \
                 "/mnt/windows/Users/ibrah/kw/scratch/kld/linux-$KVER" \
                 "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
            if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
        done
    fi
    [ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || {
        echo "pack_firmware: no configured linux-$KVER tree (set LINUX=)" >&2
        exit 1
    }
    command -v musl-gcc >/dev/null || { echo "pack_firmware: musl-gcc not found" >&2; exit 1; }
    echo "[fw] building the $FAMILY container" >&2
    make -C "$ROOT/kld" driver="$FAMILY" LINUX="$LINUX" -j"$(nproc)" \
        >"$BUILD/pack_firmware_build.log" 2>&1 || {
        echo "pack_firmware: container build failed (see $BUILD/pack_firmware_build.log)" >&2
        exit 1
    }
    ELF=$BUILD/$FAMILY/kld-$FAMILY.elf
    [ -x "$ELF" ] || { echo "pack_firmware: $ELF missing" >&2; exit 1; }
    FWOUT=$BUILD/ids/$FAMILY.fwpack
    "$ELF" --kld-fw >"$FWOUT" 2>"$BUILD/ids/$FAMILY.fwpack.err" || {
        echo "pack_firmware: --kld-fw failed" >&2; exit 1
    }
fi

python3 - "$FAMILY" "$DEVICE" "$FW_DIR" "$OUTDIR" "$FWOUT" "${FILES[@]:-}" <<'PY'
import glob, hashlib, os, re, shutil, sys

family, device, fw_dir, outdir, fwout = sys.argv[1:6]
extra = [a for a in sys.argv[6:] if a]
fw_dir = os.path.realpath(fw_dir)
base = os.path.join(outdir, "firmware")
famdir = os.path.join(base, family)
os.makedirs(famdir, exist_ok=True)
os.makedirs(base, exist_ok=True)

wanted = {}     # exact driver name -> host file path
def add(name, path):
    if name and os.path.isfile(path):
        wanted.setdefault(name, path)

# exact host names requested by the test/special cases.
for n in extra:
    add(n, os.path.join(fw_dir, n))

# the shared cfg80211 files.
reg = os.path.join(fw_dir, "regulatory.db")
add("regulatory.db", reg)
add("regulatory.db.p7s", os.path.join(fw_dir, "regulatory.db.p7s"))

def add_api(prefix, lo, hi):
    for api in range(hi, lo - 1, -1):
        add("%s-%d.ucode" % (prefix, api),
            os.path.join(fw_dir, "%s-%d.ucode" % (prefix, api)))

def add_dynamic(mac, lo, hi):
    for api in range(hi, lo - 1, -1):
        for f in glob.glob(os.path.join(fw_dir, "iwlwifi-%s-*-%d.ucode" % (mac, api))):
            add(os.path.basename(f), f)
    for f in glob.glob(os.path.join(fw_dir, "iwlwifi-%s-*.pnvm" % mac)):
        add(os.path.basename(f), f)

if fwout:
    dev = None
    if device:
        m = re.match(r'^([0-9a-fA-F]{1,4}):([0-9a-fA-F]{1,4})$', device)
        if not m:
            print("pack_firmware: bad --device (want vvvv:dddd)", file=sys.stderr)
            sys.exit(1)
        dev = (int(m.group(1), 16), int(m.group(2), 16))
    for line in open(fwout):
        parts = line.split()
        if len(parts) < 8 or parts[0] != family:
            continue
        v, d = int(parts[1], 16), int(parts[2], 16)
        if dev and (v, d) != dev:
            continue
        toks = {}
        for t in parts[7:]:
            if "=" in t:
                k, val = t.split("=", 1)
                toks[k] = val
        cands = []
        if toks.get("trans") == "1":
            n = int(toks.get("cands", "0"))
            for i in range(n):
                cands.append((toks.get("c%d.pre" % i, "-"),
                              toks.get("c%d.mac" % i, "-"),
                              int(toks.get("c%d.min" % i, "0")),
                              int(toks.get("c%d.max" % i, "0"))))
        else:
            cands.append((toks.get("pre", "-"), toks.get("mac", "-"),
                          int(toks.get("min", "0")), int(toks.get("max", "0"))))
        for pre, mac, lo, hi in cands:
            if pre != "-":
                add_api(pre, lo, hi)
                add(pre + ".pnvm", os.path.join(fw_dir, pre + ".pnvm"))
            elif mac != "-":
                add_dynamic(mac, lo, hi)

if not wanted:
    print("pack_firmware: no firmware files selected", file=sys.stderr)
    sys.exit(1)

manifest = []
total = 0
count = 0
for i, (name, path) in enumerate(sorted(wanted.items())):
    if name == "regulatory.db":
        store = os.path.join(base, "REGULATO.DB")
    elif name == "regulatory.db.p7s":
        store = os.path.join(base, "REGULATO.P7S")
    else:
        count += 1
        store83 = "FW%06d.BIN" % count
        store = os.path.join(famdir, store83)
        manifest.append((name, store83))
    shutil.copyfile(path, store)
    sz = os.path.getsize(store)
    total += sz

# the manifest lines carry the exact name, the 8.3 store name, the size and
# the sha256 of the bytes. (satoru)
man_path = os.path.join(famdir, "MANIFEST.TXT")
with open(man_path, "w") as m:
    for name, store83 in manifest:
        p = os.path.join(famdir, store83)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        m.write("%s %s %d %s\n" % (name, store83, os.path.getsize(p), h))
total += os.path.getsize(man_path)

print("pack_firmware: %s, %d files, %d bytes (%.2f MiB) in %s" %
      (family, count, total, total / (1024.0 * 1024.0), base))
PY

# end (satoru)
