#!/bin/bash
# the drm interface wire format lives in src/kernel/kld_dri_proto.h and is copied to
# kld/lx_emul/kimpl/lx_dri.h; both must stay identical from the first #ifndef on. (satoru)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
a=$(sed -n '/^#ifndef KLD_LX_DRI_H/,$p' "$ROOT/src/kernel/kld_dri_proto.h")
b=$(sed -n '/^#ifndef KLD_LX_DRI_H/,$p' "$ROOT/kld/lx_emul/kimpl/lx_dri.h")
[ "$a" = "$b" ] && echo "check_dri_proto: identical" || { echo "check_dri_proto: DIFFERENT"; exit 1; }
