# vendored bearssl

Prebuilt static BearSSL for the musl-static userspace TLS clients
(`src/userprogs/dsai.c`). Vendored because `/tmp` gets wiped and because
**Debian's packaged `libbearssl.a` cannot be used here** - it is built against
glibc with `_FORTIFY_SOURCE`, so it references `__memset_chk` / `__memcpy_chk`
which musl does not provide, and the static link fails.

Rebuild with:

    apt-get source bearssl
    cd bearssl-*/ && make CC=musl-gcc -j4
    cp build/libbearssl.a src/third_party/bearssl/
    cp inc/*.h            src/third_party/bearssl/inc/

Version: 0.6+dfsg.1 (Debian source), built with musl-gcc.
