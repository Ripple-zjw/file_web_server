#!/bin/bash
# Generate self-signed certificate for development/testing
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
CERT="$DIR/cert.pem"
KEY="$DIR/key.pem"

openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
    -keyout "$KEY" \
    -out "$CERT" \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

chmod 600 "$KEY"
echo "Generated: $CERT"
echo "Generated: $KEY"
