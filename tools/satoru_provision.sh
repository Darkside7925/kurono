#!/bin/bash
# Provision Satoru as a Kurono package repository over plain HTTP.
# Uploads python + basic deps.  Idempotent.
set -e

REPO=/var/www/html/kurono
sudo mkdir -p "$REPO/packages"

# Install nginx if needed
if ! command -v nginx >/dev/null 2>&1; then
  sudo apt-get update -y
  sudo apt-get install -y nginx
fi

# Listen on port 80, serve /var/www/html
sudo tee /etc/nginx/sites-available/kurono >/dev/null <<'NGINX'
server {
    listen 80 default_server;
    server_name _;
    root /var/www/html;
    autoindex on;
    location / { try_files $uri $uri/ =404; }
}
NGINX
sudo ln -sf /etc/nginx/sites-available/kurono /etc/nginx/sites-enabled/kurono
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t && sudo systemctl restart nginx

# Helper: emit one package directory with manifest.json + payload
pkg() {
    local name="$1" version="$2" desc="$3" payload_path="$4"
    local dir="$REPO/packages/$name"
    sudo mkdir -p "$dir"
    local fn="${name}-${version}.tar"
    if [ -n "$payload_path" ] && [ -e "$payload_path" ]; then
        sudo cp "$payload_path" "$dir/$fn"
    else
        # placeholder: empty tarball with just a README
        local tmp; tmp=$(mktemp -d)
        echo "Kurono package: $name $version" > "$tmp/README"
        tar -cf "/tmp/${name}.tar" -C "$tmp" README
        sudo mv "/tmp/${name}.tar" "$dir/$fn"
        rm -rf "$tmp"
    fi
    local size; size=$(stat -c '%s' "$dir/$fn")
    sudo tee "$dir/manifest.json" >/dev/null <<EOF
{
  "name": "$name",
  "version": "$version",
  "description": "$desc",
  "url": "/kurono/packages/$name/$fn",
  "size": $size
}
EOF
}

# Python (stub for now: real cpython.elf would go here once cross-built)
pkg python      "3.12.3"   "Python 3 interpreter"                ""
pkg python-pip  "24.0"     "Python package installer"            ""
pkg python-numpy "1.26.4"  "Numerical computing library"          ""
pkg python-requests "2.31.0" "HTTP library for Python"            ""
pkg python-flask "3.0.3"   "Lightweight WSGI web framework"      ""
pkg busybox     "1.36.1"   "Tiny userspace utilities"            ""
pkg vim         "9.1.0"    "Vi IMproved editor"                  ""
pkg curl        "8.5.0"    "Command-line HTTP client"            ""
pkg git         "2.43.0"   "Distributed version control"         ""
pkg gcc         "13.2.0"   "GNU C compiler"                      ""

# Build aggregate index.json
sudo tee "$REPO/index.json" >/dev/null <<'JSON'
{
  "repo": "kurono",
  "version": 1,
  "packages": [
    {"name":"python",          "version":"3.12.3", "description":"Python 3 interpreter"},
    {"name":"python-pip",      "version":"24.0",   "description":"Python package installer"},
    {"name":"python-numpy",    "version":"1.26.4", "description":"Numerical computing library"},
    {"name":"python-requests", "version":"2.31.0", "description":"HTTP library for Python"},
    {"name":"python-flask",    "version":"3.0.3",  "description":"Lightweight WSGI web framework"},
    {"name":"busybox",         "version":"1.36.1", "description":"Tiny userspace utilities"},
    {"name":"vim",             "version":"9.1.0",  "description":"Vi IMproved editor"},
    {"name":"curl",            "version":"8.5.0",  "description":"Command-line HTTP client"},
    {"name":"git",             "version":"2.43.0", "description":"Distributed version control"},
    {"name":"gcc",             "version":"13.2.0", "description":"GNU C compiler"}
  ]
}
JSON

echo
echo "=== Kurono repo ready ==="
curl -s http://127.0.0.1/kurono/index.json | head -20
echo
ls -la "$REPO/packages" | head
