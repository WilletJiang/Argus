#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
TASK_BUILD_DIR=$(mktemp -d /tmp/argus-build.XXXXXX)
trap 'rm -rf "$TASK_BUILD_DIR"' EXIT
TASK_SDK=$(xcrun --show-sdk-path)
mkdir -p dist/Argus.app/Contents/MacOS dist/Argus.app/Contents/Resources
for source in Sources/SearchCore/*.c; do
  xcrun clang -O3 -DNDEBUG -Wall -Wextra -isysroot "$TASK_SDK" -mmacosx-version-min=26.0 -c "$source" -o "$TASK_BUILD_DIR/${source:t:r}.o"
done
TASK_SWIFT_SOURCES=(Sources/App/*.swift Sources/UI/*.swift Sources/Search/*.swift Sources/System/*.swift)
TASK_SWIFT_FLAGS=()
if [[ "${1:-}" == "--benchmark" ]]; then
  TASK_SWIFT_SOURCES+=(Tests/AppPerformance.swift)
  TASK_SWIFT_FLAGS+=(-DARGUS_BENCHMARK)
fi
xcrun swiftc -O -whole-module-optimization -swift-version 5 -sdk "$TASK_SDK" -target arm64-apple-macos26.0 \
  "${TASK_SWIFT_FLAGS[@]}" -import-objc-header Sources/SearchCore/SearchCore.h "${TASK_SWIFT_SOURCES[@]}" \
  "$TASK_BUILD_DIR"/*.o -lsqlite3 -framework AppKit -framework Carbon -framework Contacts \
  -framework CoreServices -framework CoreFoundation -framework PDFKit -framework Quartz -framework ServiceManagement \
  -o "$TASK_BUILD_DIR/Argus"
cp "$TASK_BUILD_DIR/Argus" dist/Argus.app/Contents/MacOS/Argus
cp Resources/Info.plist dist/Argus.app/Contents/Info.plist
cp LICENSE dist/Argus.app/Contents/Resources/LICENSE
if [[ -f Resources/AppIcon.icns ]]; then cp Resources/AppIcon.icns dist/Argus.app/Contents/Resources/AppIcon.icns; fi
codesign --force --sign - --identifier dev.argus.search dist/Argus.app
du -sh dist/Argus.app
