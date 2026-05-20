#!/bin/bash
set -e
DOC=/var/www/html
WORK=$(mktemp -d)

sudo -n mkdir -p "$DOC" "$DOC/packages/python" "$DOC/packages/zlib" "$DOC/packages/openssl"
sudo -n mkdir -p "$DOC/kurono/packages/python" "$DOC/kurono/packages/zlib" "$DOC/kurono/packages/openssl"

build_pkg() {
  local NAME=$1
  local VERSION=$2
  local STAGE=$WORK/$NAME
  rm -rf "$STAGE"
  mkdir -p "$STAGE"
  case "$NAME" in
    python)
      mkdir -p "$STAGE/usr/bin" "$STAGE/usr/lib/python3.12" "$STAGE/usr/include/python3.12" "$STAGE/usr/share/python3"
      cat > "$STAGE/usr/bin/python3" <<'EOF'
#!kurono-builtin
# /usr/bin/python3 marker file. The Kurono shell `python3` command
# uses the built-in mini interpreter; this file just records install.
EOF
      cat > "$STAGE/usr/lib/python3.12/__init__.py" <<'EOF'
# Kurono Python 3.12 stdlib root marker.
__version__ = "3.12.0"
EOF
      cat > "$STAGE/usr/lib/python3.12/os.py" <<'EOF'
# Minimal os module shim for the Kurono mini Python interpreter.
sep = "/"
linesep = "\n"
def getcwd():
    return "/"
def listdir(path="/"):
    return []
EOF
      cat > "$STAGE/usr/lib/python3.12/sys.py" <<'EOF'
version = "3.12.0 (Kurono mini)"
platform = "kurono"
maxsize = 9223372036854775807
EOF
      cat > "$STAGE/usr/include/python3.12/Python.h" <<'EOF'
/* Minimal Python.h shim for Kurono. */
#ifndef Py_PYTHON_H
#define Py_PYTHON_H
#define PY_VERSION "3.12.0"
#endif
EOF
      cat > "$STAGE/usr/share/python3/LICENSE" <<'EOF'
PSF License Agreement v2 - Python 3.12.0 (excerpt)
EOF
      cat > "$STAGE/install.sh" <<'EOF'
#!/bin/sh
echo "Python 3.12.0 installed successfully"
EOF
      ;;
    zlib)
      mkdir -p "$STAGE/usr/lib" "$STAGE/usr/include"
      printf 'KZLIB stub library payload\n' > "$STAGE/usr/lib/libz.so"
      cat > "$STAGE/usr/include/zlib.h" <<'EOF'
/* Kurono zlib header stub */
#ifndef ZLIB_H
#define ZLIB_H
#define ZLIB_VERSION "1.3.0"
#endif
EOF
      cat > "$STAGE/install.sh" <<'EOF'
#!/bin/sh
echo "zlib 1.3.0 installed"
EOF
      ;;
    openssl)
      mkdir -p "$STAGE/usr/lib" "$STAGE/usr/include/openssl"
      printf 'KOSSL stub libssl payload\n'    > "$STAGE/usr/lib/libssl.so"
      printf 'KOSSL stub libcrypto payload\n' > "$STAGE/usr/lib/libcrypto.so"
      cat > "$STAGE/usr/include/openssl/ssl.h" <<'EOF'
#ifndef OPENSSL_SSL_H
#define OPENSSL_SSL_H
#define OPENSSL_VERSION_TEXT "OpenSSL 3.2.0 (Kurono stub)"
#endif
EOF
      cat > "$STAGE/install.sh" <<'EOF'
#!/bin/sh
echo "openssl 3.2.0 installed"
EOF
      ;;
  esac

  local OUT=$WORK/$NAME-$VERSION.kpkg
  ( cd "$STAGE" && tar --format=ustar -cf "$OUT" . )
  local SIZE=$(stat -c%s "$OUT")
  local SHA=$(sha256sum "$OUT" | awk '{print $1}')
  local HOST=kurono.satorut.com

  sudo -n cp "$OUT" "$DOC/packages/$NAME/$NAME-$VERSION.kpkg"
  sudo -n cp "$OUT" "$DOC/kurono/packages/$NAME/$NAME-$VERSION.kpkg"

  case "$NAME" in
    python)
      DEPS='["zlib-1.3.0", "openssl-3.2.0"]'
      DESC='Python 3.12 interpreter for Kurono OS'
      LICENSE='PSF-2.0'
      INSTALL='{"bin":"/usr/bin/python3","lib":"/usr/lib/python3.12/","include":"/usr/include/python3.12/","data":"/usr/share/python3/"}'
      ;;
    zlib)
      DEPS='[]'
      DESC='Compression library required by Python'
      LICENSE='Zlib'
      INSTALL='{"lib":"/usr/lib/","include":"/usr/include/"}'
      ;;
    openssl)
      DEPS='[]'
      DESC='Cryptography and SSL library required by Python'
      LICENSE='Apache-2.0'
      INSTALL='{"lib":"/usr/lib/","include":"/usr/include/openssl/"}'
      ;;
  esac

  sudo -n tee "$DOC/packages/$NAME/manifest.json" >/dev/null <<EOF
{
  "name": "$NAME",
  "version": "$VERSION",
  "description": "$DESC",
  "author": "darkside7925",
  "license": "$LICENSE",
  "size": "$SIZE",
  "sha256": "$SHA",
  "dependencies": $DEPS,
  "install": $INSTALL,
  "url": "https://$HOST/packages/$NAME/$NAME-$VERSION.kpkg"
}
EOF
}

build_pkg python  3.12.0
build_pkg zlib    1.3.0
build_pkg openssl 3.2.0

# index.json
PY_SIZE=$(stat -c%s "$DOC/packages/python/python-3.12.0.kpkg" 2>/dev/null || echo 0)
ZL_SIZE=$(stat -c%s "$DOC/packages/zlib/zlib-1.3.0.kpkg"      2>/dev/null || echo 0)
OS_SIZE=$(stat -c%s "$DOC/packages/openssl/openssl-3.2.0.kpkg" 2>/dev/null || echo 0)

sudo -n tee "$DOC/index.json" >/dev/null <<EOF
{
  "repo": "Kurono Package Repository",
  "version": "1.0",
  "updated": "$(date -u +%Y-%m-%d)",
  "packages": [
    { "name": "python",  "version": "3.12.0", "description": "Python interpreter",            "size": "$PY_SIZE" },
    { "name": "zlib",    "version": "1.3.0",  "description": "Compression library",           "size": "$ZL_SIZE" },
    { "name": "openssl", "version": "3.2.0",  "description": "Cryptography and SSL library",  "size": "$OS_SIZE" }
  ]
}
EOF

sudo -n chown -R www-data:www-data "$DOC"
sudo -n systemctl reload nginx
echo "----"
ls -l "$DOC"
ls -l "$DOC/packages"/* 2>/dev/null || true
echo "OK $WORK"
