#!/bin/sh
# Builds a universal (Apple silicon + Intel) installer package:
#   dist/truSDX-Bridge-<version>.pkg
# installing truSDX.driver into /Library/Audio/Plug-Ins/HAL and
# TruSDXBridge.app into /Applications.
#
# Usage: scripts/package.sh [version]   (default: from the latest git tag, e.g. v0.3.0)
# Unsigned for now; set CODESIGN_ID / an installer identity here once there's a
# Developer ID, then notarize the result.
set -eu

cd "$(dirname "$0")/.."
ROOT=$(pwd)
VERSION=${1:-$(git describe --tags --abbrev=0 2>/dev/null || echo 0.0.0)}
VERSION=${VERSION#v}
BUILD="$ROOT/build-release"
STAGE="$BUILD/pkgroot"
DIST="$ROOT/dist"
IDENTIFIER=com.github.trusdx-mac-audio-bridge
PKG="$DIST/truSDX-Bridge-$VERSION.pkg"

echo "==> Building $VERSION (arm64 + x86_64)"
cmake -S . -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DTRUSDX_VERSION="$VERSION"
cmake --build "$BUILD" -j
ctest --test-dir "$BUILD" --output-on-failure

for bin in "$BUILD/truSDX.driver/Contents/MacOS/truSDX" \
           "$BUILD/TruSDXBridge.app/Contents/MacOS/TruSDXBridge"; do
    lipo "$bin" -verify_arch arm64 x86_64
done

echo "==> Staging payload"
rm -rf "$STAGE"
mkdir -p "$STAGE/Library/Audio/Plug-Ins/HAL" "$STAGE/Applications" "$DIST"
ditto "$BUILD/truSDX.driver" "$STAGE/Library/Audio/Plug-Ins/HAL/truSDX.driver"
ditto "$BUILD/TruSDXBridge.app" "$STAGE/Applications/TruSDXBridge.app"

# Without this, Installer "relocates" the app onto any other copy with the same
# bundle ID it finds (e.g. a development build in ~/Applications).
pkgbuild --analyze --root "$STAGE" "$BUILD/components.plist"
plutil -replace 0.BundleIsRelocatable -bool NO "$BUILD/components.plist"
plutil -replace 1.BundleIsRelocatable -bool NO "$BUILD/components.plist" 2>/dev/null || true

echo "==> Building package"
pkgbuild \
    --root "$STAGE" \
    --component-plist "$BUILD/components.plist" \
    --scripts "$ROOT/packaging/scripts" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --install-location / \
    "$BUILD/component.pkg"

productbuild --synthesize --package "$BUILD/component.pkg" "$BUILD/distribution.xml"
# Title in the installer window; install for the whole machine; allow both
# architectures natively (no Rosetta prompt).
python3 - "$BUILD/distribution.xml" "$VERSION" <<'PY'
import re, sys
path, version = sys.argv[1], sys.argv[2]
xml = open(path).read()
xml = re.sub(r'\s*<options\b[^>]*/>', '', xml)
extra = (f'\n    <title>truSDX Bridge {version}</title>'
         '\n    <options customize="never" require-scripts="true" hostArchitectures="arm64,x86_64"/>'
         '\n    <domains enable_localSystem="true"/>')
xml = re.sub(r'(<installer-gui-script\b[^>]*>)', lambda m: m.group(1) + extra, xml, count=1)
open(path, 'w').write(xml)
PY
productbuild --distribution "$BUILD/distribution.xml" --package-path "$BUILD" "$PKG"

echo "==> $PKG"
shasum -a 256 "$PKG"
