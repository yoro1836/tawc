#!/usr/bin/env bash
# Keep APK contents fixed while comparing two tawcroot builds.
set -euo pipefail
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
if [[ $# != 3 ]]; then
    echo "usage: $0 DEV_APK ARM64_RUNTIME OUTPUT_APK" >&2
    exit 2
fi
APK=$(realpath "$1")
RUNTIME=$(realpath "$2")
OUT=$(realpath -m "$3")
[[ "$APK" != "$OUT" ]] || { echo "ERROR: output must differ from input" >&2; exit 2; }
SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
TOOLS="$SDK/build-tools/$(ls "$SDK/build-tools" | sort -V | tail -1)"
KEY="$HOME/.android/debug.keystore"
PACKAGE=$("$TOOLS/aapt" dump badging "$APK" | sed -n "s/^package: name='\([^']*\)'.*/\1/p")
[[ "$PACKAGE" == me.phie.tawc.dev ]] || { echo "ERROR: dev APK required" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tawc-runtime-apk.XXXXXXXX")
trap 'rm -rf "$WORK"' EXIT
python3 - "$APK" "$RUNTIME" "$WORK/unsigned.apk" <<'PY'
import sys
import zipfile
from pathlib import Path
source, runtime, output = sys.argv[1:]
target = 'lib/arm64-v8a/libtawcroot.so'
with zipfile.ZipFile(source) as src, zipfile.ZipFile(output, 'w') as dst:
    if target not in src.namelist():
        sys.exit('ERROR: arm64 tawcroot missing from APK')
    for item in src.infolist():
        if item.filename.startswith('META-INF/') and item.filename.upper().endswith(('.SF', '.RSA', '.DSA', '.EC', '.MF')):
            continue
        dst.writestr(item, Path(runtime).read_bytes() if item.filename == target else src.read(item))
PY
"$TOOLS/zipalign" -P 16 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
"$TOOLS/apksigner" sign --ks "$KEY" --ks-key-alias androiddebugkey \
    --ks-pass pass:android --key-pass pass:android --out "$OUT" "$WORK/aligned.apk"
"$TOOLS/apksigner" verify "$OUT"
original=$("$TOOLS/apksigner" verify --print-certs "$APK" | sed -n 's/.*certificate SHA-256 digest: //p' | sort -u)
signed=$("$TOOLS/apksigner" verify --print-certs "$OUT" | sed -n 's/.*certificate SHA-256 digest: //p' | sort -u)
[[ "$original" == "$signed" ]] || { echo "ERROR: signing certificate differs" >&2; exit 1; }
echo "$OUT"
