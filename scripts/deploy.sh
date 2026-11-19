#!/bin/bash
# Build and install VM Watcher on a host over SSH.
#
#   scripts/deploy.sh central ssh-host bind-address [port] [install-root]
#   scripts/deploy.sh agent    ssh-host   bind-address  [port] [install-root]
#
# The core is compiled on the target with its own g++ (no other build
# dependencies), so the binary always matches that machine's C library.
#
# The central service uses HTTPS and a dedicated certificate authority
# kept in <install-root>/tls (created on the first install).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

role=${1:?Usage: scripts/deploy.sh central|agent [ssh-host] [bind-address] [port] [install-root]}
case $role in
  central) host=${2:?central needs an ssh host}; bind=${3:?central needs a bind address}; port=${4:-6443}; root=${5:-/opt/vmwatcher}; unit=vmwatcher ;;
  agent)   host=${2:?agent needs an ssh host}; bind=${3:?agent needs the address the central service will reach it on}
           port=${4:-6089}; root=${5:-/opt/vmwatcher}; unit=vmwatcher-agent ;;
  *) echo "Unknown role: $role" >&2; exit 2 ;;
esac
for value in "$host" "$bind" "$port" "$root"; do
  [[ $value =~ ^[A-Za-z0-9_./:@-]+$ ]] || { echo "Unsupported characters in: $value" >&2; exit 2; }
done

echo "== Testing locally"
make -C core -j"$(nproc)" test
release=$(date -u +%Y%m%dT%H%M%S)
mkdir -p .work
if [[ $role == central ]]; then
  (cd web && npm ci && npm test && npm run build)
  # The web bundle is unpacked as <release>/web.
  tar -czf .work/release.tar.gz core/Makefile core/src core/tests scripts/make-certs.sh --transform 's,^web/dist,web,' web/dist
else
  tar -czf .work/release.tar.gz core/Makefile core/src core/tests
fi

echo "== Building on $host in $root/releases/$release"
ssh "$host" "mkdir -p '$root/releases/$release'"
scp -q .work/release.tar.gz "$host:$root/releases/$release/release.tar.gz"
ssh "$host" "set -e; cd '$root/releases/$release'; tar -xzf release.tar.gz; rm release.tar.gz
  make -C core -j\$(nproc) CXX=g++ >/dev/null; make -C core CXX=g++ test | tail -1; core/build/vmwatcher version"

allowed_names=""
if [[ $role == central ]]; then
  echo "== Certificates"
  ssh "$host" "set -e; if [ -f '$root/tls/server.pem' ]; then echo 'Keeping the existing certificate in $root/tls'
    else bash '$root/releases/$release/scripts/make-certs.sh' '$root/tls' '$bind' \"\$(hostname -s)\" \"\$(hostname -f)\"; fi"
  sans=$(ssh "$host" "openssl x509 -in '$root/tls/server.pem' -noout -ext subjectAltName")
  allowed_names=$(sed '1d' <<< "$sans" | tr ',' '\n' | sed -nE 's/^[[:space:]]*DNS:([a-zA-Z0-9.-]+)[[:space:]]*$/\1/p' | paste -sd, -)

fi

echo "== Installing $unit.service"
sed -e "s|@ALLOWED_NAMES@|$allowed_names|g" -e "s|@ROOT@|$root|g" -e "s|@BIND@|$bind|g" -e "s|@PORT@|$port|g" "deploy/$unit.service" > ".work/$unit.service"
scp -q ".work/$unit.service" "$host:/etc/systemd/system/$unit.service"
ssh "$host" "set -e; mkdir -p '$root/data' '$root/agent-data'; ln -sfn 'releases/$release' '$root/current'; systemctl daemon-reload
  systemctl enable '$unit' >/dev/null 2>&1; systemctl restart '$unit'; sleep 1; systemctl is-active '$unit'"

if [[ $role == central ]]; then
  echo "Viewer: https://$bind:$port  (the first visit sets the shared password)"
  echo "Trust it in a browser by importing the CA certificate: $host:$root/tls/ca.crt"
else
  echo "Agent token (paste it into Settings > Hosts > Add host, address http://$bind:$port):"
  ssh "$host" "'$root/current/core/build/vmwatcher' agent-token --data '$root/agent-data'"
fi
