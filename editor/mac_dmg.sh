#!/usr/bin/env bash
# mac_dmg.sh — Build PanelTalk Editor and package as a macOS DMG
# Usage:
#   ./mac_dmg.sh                         # Release build, no signing
#   ./mac_dmg.sh --sign "Developer ID"   # Code-sign with given identity
#   ./mac_dmg.sh --debug                 # Debug build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
APP_NAME="PanelTalkEditor"
APP_BUNDLE="$BUILD_DIR/${APP_NAME}.app"
DMG_NAME="${APP_NAME}.dmg"
DMG_OUT="$SCRIPT_DIR/$DMG_NAME"
BUILD_TYPE="Release"
SIGN_IDENTITY=""

# --- Parse arguments ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sign)
      SIGN_IDENTITY="${2:?--sign requires an identity argument}"
      shift 2
      ;;
    --debug)
      BUILD_TYPE="Debug"
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--sign <identity>] [--debug]"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

echo "==> Build type: $BUILD_TYPE"

# --- Verify tools ---
for tool in cmake macdeployqt hdiutil; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: '$tool' not found in PATH" >&2
    exit 1
  fi
done

# --- CMake configure (only if cache is missing) ---
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "==> Configuring CMake..."
  cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$SCRIPT_DIR"
fi

# --- Build ---
echo "==> Building editor..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j"$(sysctl -n hw.ncpu)"

EDITOR_BIN="$BUILD_DIR/editor"
if [[ ! -f "$EDITOR_BIN" ]]; then
  echo "ERROR: Build succeeded but editor binary not found at $EDITOR_BIN" >&2
  exit 1
fi

# --- Create .app bundle structure ---
echo "==> Creating .app bundle..."
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"
mkdir -p "$APP_BUNDLE/Contents/Frameworks"

# Copy binary
cp "$EDITOR_BIN" "$APP_BUNDLE/Contents/MacOS/$APP_NAME"

# Write Info.plist
cat > "$APP_BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>   <string>${APP_NAME}</string>
  <key>CFBundleIdentifier</key>  <string>com.paneltalk.editor</string>
  <key>CFBundleName</key>        <string>PanelTalk Editor</string>
  <key>CFBundleVersion</key>     <string>1.0.0</string>
  <key>CFBundleShortVersionString</key> <string>1.0</string>
  <key>CFBundlePackageType</key> <string>APPL</string>
  <key>CFBundleSignature</key>   <string>????</string>
  <key>NSHighResolutionCapable</key> <true/>
  <key>LSMinimumSystemVersion</key> <string>12.0</string>
</dict>
</plist>
PLIST

# --- macdeployqt: bundle Qt frameworks ---
echo "==> Running macdeployqt..."
macdeployqt "$APP_BUNDLE" -verbose=1

# --- Bundle non-Qt shared libs (FFmpeg, etc.) ---
echo "==> Bundling non-Qt shared libraries..."
bundle_libs() {
  local binary="$1"
  local frameworks_dir="$APP_BUNDLE/Contents/Frameworks"

  # Get all non-system dylib dependencies
  otool -L "$binary" 2>/dev/null \
    | awk '/\/(opt\/homebrew|usr\/local)\// {print $1}' \
    | while read -r lib; do
        local libname
        libname="$(basename "$lib")"
        if [[ ! -f "$frameworks_dir/$libname" ]]; then
          echo "    Bundling: $libname"
          cp "$lib" "$frameworks_dir/$libname"
          chmod 755 "$frameworks_dir/$libname"
          # Rewrite the install name to @rpath
          install_name_tool -id "@rpath/$libname" "$frameworks_dir/$libname" 2>/dev/null || true
        fi
        # Fix the reference in the binary
        install_name_tool -change "$lib" "@rpath/$libname" "$binary" 2>/dev/null || true
      done
}

bundle_libs "$APP_BUNDLE/Contents/MacOS/$APP_NAME"

# Fix rpath so the bundle finds its own Frameworks dir
install_name_tool -add_rpath "@executable_path/../Frameworks" \
  "$APP_BUNDLE/Contents/MacOS/$APP_NAME" 2>/dev/null || true

# Recursively fix any libs that were just bundled
for lib in "$APP_BUNDLE/Contents/Frameworks"/*.dylib; do
  [[ -f "$lib" ]] || continue
  bundle_libs "$lib"
done

# --- Code signing ---
# Ad-hoc signing (-s -) is always required on macOS 15+: install_name_tool
# modifications invalidate any prior signature, and the kernel kills unsigned
# or invalidly-signed code at dylib load time (SIGKILL / Code Signature Invalid).
CODESIGN_IDENTITY="${SIGN_IDENTITY:--}"
if [[ -n "$SIGN_IDENTITY" ]]; then
  echo "==> Code-signing with Developer ID: $SIGN_IDENTITY"
else
  echo "==> Applying ad-hoc signature (required for macOS 15+ local installs)"
fi

# Sign inner binaries first (frameworks, dylibs), then the app bundle last.
# Order matters: deep signing from the inside out prevents signature invalidation.
find "$APP_BUNDLE/Contents/Frameworks" \( -name "*.dylib" -o -name "*.framework" \) | sort -r | \
  while read -r item; do
    codesign --force --sign "$CODESIGN_IDENTITY" "$item" 2>/dev/null || true
  done

find "$APP_BUNDLE/Contents/PlugIns" -name "*.dylib" 2>/dev/null | \
  while read -r item; do
    codesign --force --sign "$CODESIGN_IDENTITY" "$item" 2>/dev/null || true
  done

codesign --force --sign "$CODESIGN_IDENTITY" \
  "$APP_BUNDLE/Contents/MacOS/$APP_NAME"

codesign --force --sign "$CODESIGN_IDENTITY" "$APP_BUNDLE"
echo "    Signed (identity: $CODESIGN_IDENTITY)"

# --- Create DMG ---
echo "==> Creating DMG: $DMG_OUT"
rm -f "$DMG_OUT"

# Staging folder
STAGING_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGING_DIR"' EXIT

cp -R "$APP_BUNDLE" "$STAGING_DIR/"
# Add Applications symlink for drag-to-install UX
ln -s /Applications "$STAGING_DIR/Applications"

hdiutil create \
  -volname "PanelTalk Editor" \
  -srcfolder "$STAGING_DIR" \
  -ov \
  -format UDZO \
  -fs HFS+ \
  "$DMG_OUT"

echo ""
echo "✓ Done! DMG created at: $DMG_OUT"
echo "  Size: $(du -sh "$DMG_OUT" | cut -f1)"
