#!/usr/bin/env bash
# Installs the driver and registers it with unixODBC.
# Usage: sudo packaging/linux/install.sh [path/to/libsfodbc.so]
set -euo pipefail

LIB="${1:-build/libsfodbc.so}"
PREFIX="${PREFIX:-/usr/local}"
NAME="Eloquix Salesforce ODBC Driver"

[[ -f "$LIB" ]] || { echo "Driver not found: $LIB" >&2; exit 1; }
command -v odbcinst >/dev/null || { echo "unixODBC is not installed (apt-get install unixodbc)" >&2; exit 1; }

install -d "$PREFIX/lib"
install -m 0755 "$LIB" "$PREFIX/lib/libsfodbc.so"

TEMPLATE=$(mktemp)
cat > "$TEMPLATE" <<INI
[$NAME]
Description = Salesforce ODBC driver (Eloquix Labs)
Driver      = $PREFIX/lib/libsfodbc.so
Threading   = 1
FileUsage   = 0
INI

odbcinst -i -d -f "$TEMPLATE"
rm -f "$TEMPLATE"

echo "Installed $PREFIX/lib/libsfodbc.so"
echo "Registered as: $NAME"
echo
echo "Add a DSN to ~/.odbc.ini or /etc/odbc.ini, for example:"
echo "  [Salesforce]"
echo "  Driver   = $NAME"
echo "  AuthType = JWT"
echo "  ClientId = <consumer key>"
echo "  Username = integration.user@example.com"
echo "  PrivateKeyFile = /etc/eloquix/salesforce.key"
