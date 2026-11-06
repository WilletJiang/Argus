#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
# Build only the extractable project; no parent paths, Swift or AppKit.
mkdir "$work/source"
(cd "$root" && tar --exclude='./build*' --exclude='./.git' -cf - .) | (cd "$work/source" && tar xf -)
cmake -S "$work/source" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$work/install"
cmake --build "$work/build" --parallel 4
ctest --test-dir "$work/build" --output-on-failure
cmake --install "$work/build"
mkdir "$work/client"
cp "$work/source/examples/embed.c" "$work/client/main.c"
cat > "$work/client/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(external_client C)
find_package(ah CONFIG REQUIRED)
add_executable(client main.c)
target_link_libraries(client PRIVATE ah::ah)
CMAKE
cmake -S "$work/client" -B "$work/client-build" -DCMAKE_PREFIX_PATH="$work/install"
cmake --build "$work/client-build"
"$work/client-build/client"
