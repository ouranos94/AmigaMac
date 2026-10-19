#!/bin/sh
# Build AmigaMac.app (release) with its system template.
set -e
cd "$(dirname "$0")/.."
swift build -c release --product AmigaMac
APP=build/AmigaMac.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/System"
cp .build/release/AmigaMac "$APP/Contents/MacOS/AmigaMac"
# system template: AmigaMac's own files in System/, not what .gitignore leaves out
# (the RAM disk, Work/ and third-party software installed locally)
git -c core.quotePath=false ls-files --cached --others --exclude-standard -- System | cpio -pdm "$APP/Contents/Resources" 2>/dev/null
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>AmigaMac</string>
  <key>CFBundleDisplayName</key><string>AmigaMac</string>
  <key>CFBundleIdentifier</key><string>local.amigamac</string>
  <key>CFBundleExecutable</key><string>AmigaMac</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleDocumentTypes</key><array><dict>
    <key>CFBundleTypeName</key><string>Disquette Amiga</string>
    <key>CFBundleTypeExtensions</key><array><string>adf</string><string>ADF</string></array>
    <key>CFBundleTypeRole</key><string>Editor</string>
    <key>LSHandlerRank</key><string>Alternate</string>
  </dict></array>
</dict></plist>
PLIST
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "$APP"
