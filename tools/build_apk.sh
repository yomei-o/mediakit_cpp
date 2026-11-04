#!/bin/sh
# Wrap one of the shared libraries built for Android into an installable APK (NativeActivity, no Java).
#   tools/build_apk.sh build-android/libmk_studio.so [out.apk]
# Needs ANDROID_HOME (build-tools and a platform android.jar) and a debug keystore.
set -e
SO=$1
[ -n "$SO" ] || { echo "usage: $0 <path/to/libNAME.so> [out.apk]"; exit 1; }
NAME=$(basename "$SO" .so); NAME=${NAME#lib}
OUT=${2:-$(dirname "$SO")/$NAME.apk}
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
# apksigner and aapt2 need a JDK; Android Studio ships one when there is none on the PATH
if [ -z "$JAVA_HOME" ] && ! java -version >/dev/null 2>&1; then
  for d in "/Applications/Android Studio.app/Contents/jbr/Contents/Home" "$SDK/jbr"; do
    [ -x "$d/bin/java" ] && JAVA_HOME=$d && export JAVA_HOME && PATH="$d/bin:$PATH" && export PATH && break
  done
fi
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
JAR=$(ls -d "$SDK"/platforms/*/android.jar | sort -V | tail -1)
ABI=${ABI:-arm64-v8a}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

sed "s/MK_NAME/$NAME/g" "$HERE/AndroidManifest.xml" > "$WORK/AndroidManifest.xml"
mkdir -p "$WORK/lib/$ABI"
cp "$SO" "$WORK/lib/$ABI/lib$NAME.so"

"$BT/aapt2" link -o "$WORK/base.apk" -I "$JAR" --manifest "$WORK/AndroidManifest.xml" \
    --min-sdk-version 26 --target-sdk-version 33
(cd "$WORK" && "$BT/aapt" add -0 '' base.apk "lib/$ABI/lib$NAME.so" >/dev/null)
"$BT/zipalign" -f -p 4 "$WORK/base.apk" "$WORK/aligned.apk"
KS=$HOME/.android/debug.keystore
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
    --out "$OUT" "$WORK/aligned.apk"
echo "$OUT"
