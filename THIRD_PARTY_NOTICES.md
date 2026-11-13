# Third-party notices

Kurono's own code is licensed under the GPL v2 (see `LICENSE`). The repository also ships some
third-party code, programs and data. Each one keeps its own license, listed below, and the full
license texts are in `licenses/`.

The bundled programs are separate executables. Kurono loads and runs them as their own processes,
so they are aggregated with Kurono, not linked into it.

If a source link below goes stale or doesn't match the binary in this repository, open an issue
and I'll provide the exact source for that component.

## Linked into the kernel

| Component | Where | License | Source |
| --- | --- | --- | --- |
| BearSSL | `src/third_party/bearssl/` | MIT, `licenses/MIT-BearSSL.txt` | https://bearssl.org/ |
| stb_image, stb_truetype | `src/third_party/stb_*.h` | public domain or MIT, at the end of each file | https://github.com/nothings/stb |

## Bundled programs and libraries

| Component | Where | License | Source |
| --- | --- | --- | --- |
| GNU Bash 5.2.37 (static, musl) | `src/userprogs/bash.elf` | GPL v3, `licenses/GPL-3.0.txt` | https://ftp.gnu.org/gnu/bash/bash-5.2.tar.gz plus https://ftp.gnu.org/gnu/bash/bash-5.2-patches/ (patches 001 to 037) |
| Git 2.47.3 (static, musl) | `src/userprogs/git.elf` | GPL v2, `LICENSE` | https://mirrors.edge.kernel.org/pub/software/scm/git/git-2.47.3.tar.xz |
| FFmpeg 5.1 (libavcodec 59.37.100), LGPL build | `src/userprogs/ffmpeg.elf` | LGPL v2.1 or later, `licenses/LGPL-2.1.txt` | https://ffmpeg.org/releases/ (the 5.1 release series) |
| musl libc 1.2.6 | `src/userprogs/musl_libc.so` | MIT, `licenses/MIT-musl.txt` | https://musl.libc.org/ |
| gdk-pixbuf 2.42.12 | `src/assets/gdk-pixbuf/libgdk_pixbuf-2.0.so.0` | LGPL v2.1 or later, `licenses/LGPL-2.1.txt` | https://download.gnome.org/sources/gdk-pixbuf/2.42/gdk-pixbuf-2.42.12.tar.xz |
| SQLite (`lib/libsqlite3.so.0`) | `src/assets_nsslibs.tar` | public domain | https://sqlite.org/ |
| Alpine Linux guest: Linux 6.18.20-0-virt and 6.18.60-0-virt kernels, initramfs | `Alpine/` | Linux: GPL v2 (`LICENSE`). Initramfs packages: their own licenses (mostly GPL v2 and MIT) | Linux: https://kernel.org/. Alpine packages: https://gitlab.alpinelinux.org/alpine/aports |

## Data and fonts

| Component | Where | License | Source |
| --- | --- | --- | --- |
| ICU data 78 | `src/assets/icu/icudt78l.dat` | Unicode License v3, `licenses/Unicode-ICU.txt` | https://github.com/unicode-org/icu |
| Mozilla CA root store (Debian ca-certificates 20250419) | `src/system/claude_cabundle.inc` | MPL 2.0, `licenses/MPL-2.0.txt` | https://salsa.debian.org/debian/ca-certificates |
| xkeyboard-config | `src/assets_xkb.tar`, `src/assets/keymaps/keymaps.bin` (built from it) | MIT/X11 style, `licenses/MIT-xkeyboard-config.txt` | https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config |
| Inter | `src/assets/fonts/Inter-*.otf` | SIL Open Font License 1.1, `licenses/OFL-1.1-Inter.txt` | https://github.com/rsms/inter |
| DejaVu Sans Mono | `src/assets/fonts/DejaVuSansMono.ttf` | DejaVu/Bitstream Vera license, `src/assets/fonts/LICENSE-DejaVu.txt` | https://dejavu-fonts.github.io/ |
| FFmpeg presets | `tools/ffmpeg-8.1-essentials_build/` | FFmpeg's license, `tools/ffmpeg-8.1-essentials_build/LICENSE` | https://www.gyan.dev/ffmpeg/builds/ |

## Not third-party

The icon set (`src/assets/icons/`), the logo and the two wallpapers ("Mist" and "Peaks",
rendered by `tools/wallpapers/gen.py`) are original and part of Kurono under the GPL v2.

The Linux drivers that KLD runs are not stored in this repository. `kld/tools/fetch_linux.sh`
downloads the Linux 6.12.99 source (GPL v2) from kernel.org, and the drivers are built from that.
