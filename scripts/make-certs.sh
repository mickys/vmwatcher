#!/bin/bash
# Internal certificates for VM Watcher.
#
#   scripts/make-certs.sh <directory> <name-or-address> [more names or addresses...]
#
# Creates a private certificate authority in <directory> the first time, then
# issues (or renews) the server certificate for the given names. Import
# <directory>/ca.crt into the browsers that should trust the viewer; keep
# ca.key private. Re-run with the same directory to renew or change names.
#
# To use a certificate from another CA instead, have that CA sign server.csr
# and write the result, followed by that CA's chain, to server.pem.
set -euo pipefail
dir=${1:?Usage: make-certs.sh <directory> <name-or-address>...}
shift
[[ $# -ge 1 ]] || { echo "Give at least one name or address for the certificate" >&2; exit 2; }
umask 077
mkdir -p "$dir"
cd "$dir"

if [[ ! -f ca.key || ! -f ca.crt ]]; then
  openssl ecparam -name prime256v1 -genkey -noout -out ca.key
  openssl req -x509 -new -key ca.key -sha256 -days 3650 -subj "/O=VM Watcher/CN=VM Watcher internal CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.crt
  echo "Created certificate authority: $PWD/ca.crt"
fi

names=()
for name in "$@"; do
  [[ $name =~ ^[A-Za-z0-9.:-]+$ ]] || { echo "Unsupported name: $name" >&2; exit 2; }
  if [[ $name =~ ^[0-9.]+$ || $name == *:* ]]; then names+=("IP:$name"); else names+=("DNS:$name"); fi
done
san=$(IFS=,; echo "${names[*]}")

openssl ecparam -name prime256v1 -genkey -noout -out server.key.new
# The request carries the names too, so another CA can sign it as it is.
openssl req -new -key server.key.new -subj "/O=VM Watcher/CN=$1" -addext "subjectAltName=$san" -out server.csr
printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=%s\n' "$san" > server.ext
# Browsers reject server certificates valid for more than 398 days.
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -sha256 -days 397 -extfile server.ext -out server.crt.new 2>/dev/null
cat server.crt.new ca.crt > server.pem.new
mv server.key.new server.key
mv server.pem.new server.pem
rm -f server.ext server.crt.new
chmod 644 ca.crt
echo "Issued $PWD/server.pem (+ server.key) for: $san"
openssl x509 -in server.pem -noout -enddate | sed 's/notAfter=/Valid until: /'
