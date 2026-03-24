#!/bin/bash
# Generate self-signed TLS certificate for the web dashboard
# Run this ONCE before first deployment
#
# Usage: ./generate_tls_certs.sh
#
# For production, replace with Let's Encrypt or a proper CA cert.

set -e

CERT_DIR="$(dirname "$0")"
DAYS=365
SUBJECT="/C=LU/ST=Luxembourg/O=GHRA/CN=ghra-dashboard"

echo "Generating self-signed TLS certificate..."

openssl req -x509 -nodes -newkey rsa:2048 \
    -keyout "$CERT_DIR/server.key" \
    -out "$CERT_DIR/server.crt" \
    -days "$DAYS" \
    -subj "$SUBJECT" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:192.168.1.100"

echo "Certificate generated:"
echo "  $CERT_DIR/server.crt"
echo "  $CERT_DIR/server.key"
echo ""
echo "Valid for $DAYS days."
echo "Add the Dell OptiPlex IP to subjectAltName if different from 192.168.1.100"
