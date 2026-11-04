#!/bin/sh
# Build, sign and install one of the iOS apps on a connected iPhone.
#   tools/build_ios_app.sh mk_studio [device-udid]
# Signing uses whatever team the Apple ID in Xcode provides (a free "Personal Team" is enough);
# the certificate it issues is good for seven days.
set -e
TARGET=${1:-mk_studio}
UDID=$2
DIR=build-ios

if [ -z "$UDID" ]; then
  UDID=$(xcrun xctrace list devices 2>/dev/null | sed -n '/== Devices ==/,/^$/p' \
         | grep -oE '\(([0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}|[0-9A-Fa-f]{40})\)' | head -1 | tr -d '()')
fi
[ -n "$UDID" ] || { echo "no iPhone connected (xcrun xctrace list devices)"; exit 1; }

# The team comes from the Apple ID added in Xcode > Settings > Accounts; a free Personal Team works.
TEAM=${DEVELOPMENT_TEAM:-$(plutil -p ~/Library/Preferences/com.apple.dt.Xcode.plist 2>/dev/null \
      | sed -n 's/.*"teamID" => "\([A-Z0-9]*\)".*/\1/p' | head -1)}
[ -n "$TEAM" ] || { echo "no development team: add an Apple ID in Xcode > Settings > Accounts"; exit 1; }

cmake -S . -B "$DIR" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 >/dev/null

# a free team's certificate lasts seven days; -allowProvisioningUpdates renews it and registers
# the connected device
BUNDLE_ID=org.mediakit.$(echo "$TARGET" | tr -d '_')
xcodebuild -project "$DIR"/mediakit.xcodeproj -target "$TARGET" -configuration Release \
    -destination "id=$UDID" -allowProvisioningUpdates \
    CODE_SIGN_STYLE=Automatic DEVELOPMENT_TEAM="$TEAM" \
    PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" -quiet

APP=$DIR/Release-iphoneos/$TARGET.app
xcrun devicectl device install app --device "$UDID" "$APP"
echo "installed $APP on $UDID"
echo "launch: xcrun devicectl device process launch --device $UDID $BUNDLE_ID"
