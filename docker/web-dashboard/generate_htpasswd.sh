#!/bin/bash
# Generate .htpasswd file for dashboard basic auth
#
# Usage: ./generate_htpasswd.sh [username]
#
# You will be prompted for a password.

set -e

USERNAME="${1:-admin}"
HTPASSWD_FILE="$(dirname "$0")/.htpasswd"

if ! command -v htpasswd &> /dev/null; then
    echo "htpasswd not found. Install with:"
    echo "  apt install apache2-utils  (Debian/Ubuntu)"
    echo "  apk add apache2-utils      (Alpine)"
    echo ""
    echo "Or generate manually with:"
    echo "  echo -n 'password' | openssl passwd -apr1 -stdin"
    exit 1
fi

echo "Creating dashboard login for user: $USERNAME"
htpasswd -c "$HTPASSWD_FILE" "$USERNAME"

echo ""
echo "Password file created: $HTPASSWD_FILE"
echo "This file is mounted into the nginx container."
