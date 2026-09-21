#!/usr/bin/env bash
# Installs the driver on macOS (unixODBC or iODBC).
# Usage: sudo packaging/macos/install.sh [path/to/libsfodbc.dylib]
set -euo pipefail

LIB="${1:-build/libsfodbc.dylib}"
PREFIX="${PREFIX:-/usr/local}"
NAME="Eloquix Salesforce ODBC Driver"

[[ -f "$LIB" ]] || { echo "Driver not found: $LIB" >&2; exit 1; }

install -d "$PREFIX/lib"
install -m 0755 "$LIB" "$PREFIX/lib/libsfodbc.dylib"

# Bundle paths differ between Homebrew prefixes; rewrite to absolute install name.
install_name_tool -id "$PREFIX/lib/libsfodbc.dylib" "$PREFIX/lib/libsfodbc.dylib" 2>/dev/null || true

INI="/Library/ODBC/odbcinst.ini"
[[ -w /Library/ODBC ]] || INI="$HOME/Library/ODBC/odbcinst.ini"
mkdir -p "$(dirname "$INI")"
touch "$INI"

python3 - "$INI" "$NAME" "$PREFIX/lib/libsfodbc.dylib" <<'PY'
import configparser, sys
path, name, lib = sys.argv[1:4]
cfg = configparser.ConfigParser()
cfg.read(path)
if "ODBC Drivers" not in cfg:
    cfg["ODBC Drivers"] = {}
cfg["ODBC Drivers"][name] = "Installed"
cfg[name] = {"Description": "Salesforce ODBC driver (Eloquix Labs)", "Driver": lib, "Threading": "1"}
with open(path, "w") as fh:
    cfg.write(fh)
PY

echo "Installed $PREFIX/lib/libsfodbc.dylib"
echo "Registered in $INI as: $NAME"
echo
echo "For distribution outside your own machine, sign and notarise the dylib:"
echo "  codesign --timestamp --options runtime -s 'Developer ID Application: ...' $PREFIX/lib/libsfodbc.dylib"
