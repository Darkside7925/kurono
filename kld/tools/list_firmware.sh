#!/bin/bash
# list_firmware - the firmware files the iwlwifi driver can request, from the
# driver's own data, and what of that is on this host.
#
# Runs the built iwlwifi KLD container on the host with --kld-fw. That mode
# registers the family with the pci bus disabled and prints, per pci id entry,
# the cfg fields the driver carries: fw_name_pre and the ucode API range for
# the entries that name a full cfg, and every dev_info candidate of the same
# device family for the entries whose driver_data is a trans cfg (the probe
# reads the card's mac/rf/step registers before it can pick the full cfg).
#
# For each candidate the script builds the file name the driver would request
# (iwl-drv.c iwl_request_firmware: <prefix>-<api>.ucode, api from max down to
# min), checks the host's /lib/firmware for the newest one that exists and
# prints its size. Nothing is downloaded and nothing is embedded.
#
# usage: list_firmware.sh [output-file]
# env: LINUX (configured tree), KLD_BUILD (default kld/build)
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
FW_DIR=${FW_DIR:-/lib/firmware}
OUT=${1:-}
BUILD=${KLD_BUILD:-$ROOT/kld/build}
IDS=$BUILD/ids/iwlwifi.fw

if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" \
             "/mnt/windows/Users/ibrah/kw/scratch/kld/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || {
    echo "list_firmware: no configured linux-$KVER tree (set LINUX=)" >&2
    exit 1
}
command -v musl-gcc >/dev/null || { echo "list_firmware: musl-gcc not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "list_firmware: python3 not found" >&2; exit 1; }

echo "[fw] building the iwlwifi container" >&2
make -C "$ROOT/kld" driver=iwlwifi LINUX="$LINUX" -j"$(nproc)" \
    >"$BUILD/list_firmware_build.log" 2>&1 || {
    echo "list_firmware: container build failed (see $BUILD/list_firmware_build.log)" >&2
    exit 1
}

ELF=$BUILD/iwlwifi/kld-iwlwifi.elf
[ -x "$ELF" ] || { echo "list_firmware: $ELF missing" >&2; exit 1; }

mkdir -p "$BUILD/ids"
"$ELF" --kld-fw >"$IDS" 2>"$BUILD/ids/iwlwifi.fw.err" || {
    echo "list_firmware: --kld-fw failed (see $BUILD/ids/iwlwifi.fw.err)" >&2
    exit 1
}

python3 - "$IDS" "$FW_DIR" > "${OUT:-/dev/stdout}" <<'PY'
import os, re, sys
from glob import glob

ids_path, fw_dir = sys.argv[1], sys.argv[2]
fw_dir = os.path.realpath(fw_dir)

def check(prefix, lo, hi):
    """newest file the driver would request for one prefix and api range."""
    for api in range(hi, lo - 1, -1):
        if not prefix:
            return ("missing", 0, 0, api)
        if "%" in prefix:                      # dynamic hw-step pattern
            pat = prefix.replace("%", "*")
            files = sorted(glob(os.path.join(fw_dir, pat + "-%d.ucode" % api)))
            if files:
                size = sum(os.path.getsize(f) for f in files)
                return (os.path.basename(files[0]) if len(files) == 1
                        else "%s (%d hw-step files)" % (pat, len(files)),
                        size, len(files), api)
        else:
            f = os.path.join(fw_dir, "%s-%d.ucode" % (prefix, api))
            if os.path.isfile(f):
                return (os.path.basename(f), os.path.getsize(f), 1, api)
    return ("missing", 0, 0, hi)

rows = []          # (id, pattern, lo, hi, newest, bytes, files, chosen_api)
patterns = {}      # pattern key -> (prefix_for_check, lo, hi)
entries = 0

for line in open(ids_path):
    line = line.strip()
    if not line:
        continue
    parts = line.split()
    if len(parts) < 8:
        continue
    drv, v, d = parts[0], parts[1], parts[2]
    if drv != "iwlwifi":
        continue
    toks = {}
    for t in parts[7:]:
        if "=" in t:
            k, val = t.split("=", 1)
            toks[k] = val
    dev = "%s:%s" % (v, d)
    entries += 1
    if toks.get("trans") == "1":
        fam = toks.get("fam", "?")
        n = int(toks.get("cands", "0"))
        seen = set()
        for i in range(n):
            pre = toks.get("c%d.pre" % i, "-")
            mac = toks.get("c%d.mac" % i, "-")
            lo = int(toks.get("c%d.min" % i, "0"))
            hi = int(toks.get("c%d.max" % i, "0"))
            if pre != "-":
                pat = pre
                key = ("pre", pre, lo, hi)
            elif mac != "-":
                # the driver appends the mac/rf steps: iwlwifi-<mac>-<steps>-<api>
                pat = "iwlwifi-%s-%%" % mac
                key = ("mac", mac, lo, hi)
            else:
                continue
            if (pat, lo, hi) in seen:
                continue
            seen.add((pat, lo, hi))
            patterns[key] = (pat, lo, hi)
            newest, size, nf, api = check(pat, lo, hi)
            rows.append((dev, fam, pat, lo, hi, newest, size, nf, api))
    else:
        pre = toks.get("pre", "-")
        mac = toks.get("mac", "-")
        lo = int(toks.get("min", "0"))
        hi = int(toks.get("max", "0"))
        if pre != "-":
            pat = pre
            key = ("pre", pre, lo, hi)
        elif mac != "-":
            pat = "iwlwifi-%s-%%" % mac
            key = ("mac", mac, lo, hi)
        else:
            continue
        patterns[key] = (pat, lo, hi)
        newest, size, nf, api = check(pat, lo, hi)
        rows.append((dev, "-", pat, lo, hi, newest, size, nf, api))

# chosen file set: the newest existing file(s) per distinct pattern
chosen = {}
total = 0
present = missing = 0
for pat, lo, hi in patterns.values():
    newest, size, nf, api = check(pat, lo, hi)
    if size:
        # key by the actual file set (pattern + chosen api)
        k = (pat, api if "%" in pat else None)
        if k not in chosen:
            chosen[k] = size
            total += size
        present += 1
    else:
        missing += 1

print("# KLD iwlwifi firmware survey, from the driver's own pci id and cfg data")
print("# firmware dir: %s" % fw_dir)
print("# id            family  prefix/pattern                    api        newest present (bytes)")
for (dev, fam, pat, lo, hi, newest, size, nf, api) in rows:
    print("%-13s %-7s %-34s %3d..%-3d  %-42s %s" %
          (dev, fam if fam != "-" else "-",
           pat if "%" not in pat else pat.replace("%", "<hw-steps>"),
           lo, hi, newest, ("%d" % size) if size else "-"))
print()
print("# summary: %d id entries, %d distinct firmware patterns" % (entries, len(patterns)))
print("# present on this host: %d patterns, missing: %d patterns" % (present, missing))
print("# total size if every Intel card in the table were covered: %.1f MiB (%d bytes, %d files)"
      % (total / (1024.0 * 1024.0), total, sum(1 for _ in chosen)))
PY

# end (satoru)
