#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
TASK_TEST_DIR=$(mktemp -d /tmp/argus-tests.XXXXXX)
trap 'rm -rf "$TASK_TEST_DIR"' EXIT
TASK_C_SOURCES=(Sources/SearchCore/*.c)
TASK_C_FLAGS=(-O2 -Wall -Wextra -Werror)
if [[ "${1:-}" == "--ui-only" ]]; then
  for source in "${TASK_C_SOURCES[@]}"; do xcrun clang -O2 -c "$source" -o "$TASK_TEST_DIR/${source:t:r}.o"; done
  xcrun swiftc -Onone -g -swift-version 5 -import-objc-header Sources/SearchCore/SearchCore.h \
    Sources/App/Models.swift Sources/App/Preferences.swift Sources/App/HotKey.swift Sources/UI/*.swift Sources/Search/*.swift Sources/System/*.swift Tests/PanelPresentationTests.swift Tests/AppearanceTests.swift \
    "$TASK_TEST_DIR"/*.o -lsqlite3 -framework AppKit -framework Carbon -framework Contacts -framework CoreServices -framework CoreFoundation -framework PDFKit -framework Quartz -framework ServiceManagement -o "$TASK_TEST_DIR/presentation-tests"
  "$TASK_TEST_DIR/presentation-tests"
  exit 0
fi
if [[ "${1:-}" != "--index-only" ]]; then
xcrun swiftc -O -swift-version 5 Sources/App/HotKey.swift Tests/HotKeyTests.swift -framework AppKit -framework Carbon -o "$TASK_TEST_DIR/hotkey-tests"
"$TASK_TEST_DIR/hotkey-tests"
fi
if [[ "${1:-}" == "--hotkey-only" ]]; then exit 0; fi
if [[ "${1:-}" == "--sanitize" ]]; then TASK_C_FLAGS=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror); fi
if [[ "${1:-}" != "--index-only" ]]; then
xcrun clang "${TASK_C_FLAGS[@]}" -I Sources/SearchCore "${TASK_C_SOURCES[@]}" Tests/SearchCoreTests.c -lsqlite3 -framework CoreFoundation -o "$TASK_TEST_DIR/core-tests"
"$TASK_TEST_DIR/core-tests"
xcrun clang "${TASK_C_FLAGS[@]}" -I Sources/SearchCore "${TASK_C_SOURCES[@]}" Tests/SearchOptimizationTests.c -lsqlite3 -framework CoreFoundation -o "$TASK_TEST_DIR/optimization-tests"
"$TASK_TEST_DIR/optimization-tests"
xcrun swiftc -O -swift-version 5 Sources/App/Models.swift Sources/System/*.swift Sources/Search/ScanWorker.swift Sources/Search/ContentExtractor.swift Tests/SystemProviderTests.swift -framework AppKit -framework Contacts -framework CoreServices -framework PDFKit -o "$TASK_TEST_DIR/provider-tests"
"$TASK_TEST_DIR/provider-tests"
xcrun swiftc -O -swift-version 5 Sources/App/Models.swift Sources/Search/MetadataSearch.swift Tests/MetadataSearchTests.swift -framework AppKit -framework CoreServices -o "$TASK_TEST_DIR/metadata-tests"
"$TASK_TEST_DIR/metadata-tests"
xcrun swiftc -O -swift-version 5 Sources/App/Models.swift Sources/App/Preferences.swift Sources/Search/ResultRanking.swift Tests/ResultRankingTests.swift -framework AppKit -o "$TASK_TEST_DIR/ranking-tests"
"$TASK_TEST_DIR/ranking-tests"
fi
for source in "${TASK_C_SOURCES[@]}"; do xcrun clang -O2 -c "$source" -o "$TASK_TEST_DIR/${source:t:r}.o"; done
xcrun swiftc -Onone -g -swift-version 5 -import-objc-header Sources/SearchCore/SearchCore.h \
  Sources/App/Models.swift Sources/App/Preferences.swift Sources/Search/FileIndex.swift Sources/Search/IndexSearch.swift Sources/Search/FileWatcher.swift Sources/Search/ContentExtractor.swift Sources/Search/ScanWorker.swift Tests/FileIndexTests.swift \
  "$TASK_TEST_DIR"/*.o -lsqlite3 -framework AppKit -framework CoreServices -framework CoreFoundation -framework PDFKit -o "$TASK_TEST_DIR/index-tests"
"$TASK_TEST_DIR/index-tests"
