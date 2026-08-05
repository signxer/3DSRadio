#!/bin/bash
# Download Mozilla CA certificate bundle for 3DSRadio HTTPS
# This script downloads the cacert.pem file needed by libcurl/mbedtls

set -e

CACERT_URL="https://curl.se/ca/cacert.pem"
OUTPUT_DIR="$(dirname "$0")/../romfs"
OUTPUT_FILE="${OUTPUT_DIR}/cacert.pem"

mkdir -p "$OUTPUT_DIR"

echo "Downloading CA certificate bundle from curl.se..."
if command -v curl &> /dev/null; then
    curl -sSL -o "$OUTPUT_FILE" "$CACERT_URL"
elif command -v wget &> /dev/null; then
    wget -q -O "$OUTPUT_FILE" "$CACERT_URL"
else
    echo "Error: Neither curl nor wget found. Please install one of them."
    exit 1
fi

if [ -f "$OUTPUT_FILE" ]; then
    CERT_COUNT=$(grep -c "BEGIN CERTIFICATE" "$OUTPUT_FILE" || true)
    echo "Downloaded ${CERT_COUNT} CA certificates to ${OUTPUT_FILE}"
else
    echo "Error: Failed to download CA certificates"
    exit 1
fi
