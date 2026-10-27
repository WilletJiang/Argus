# Argus

Argus is a lightweight macOS search and launcher built with Swift, AppKit, and C. Search apps, files, and document contents with Chinese and pinyin support, then open or preview results from the keyboard. Its native Liquid Glass interface supports light and dark appearances, and queries, indexed content, and usage history stay on your Mac.

Requires macOS 26+ and Apple silicon. Download the [latest release](https://github.com/WilletJiang/Argus/releases/latest), unzip it, and open Argus.app. Press ⌘Space to search (⌥Space if the system shortcut is occupied). Build with `./scripts/build.sh`; run tests with `./scripts/test.sh` and `./scripts/test.sh --ui-only`. Released under the [MIT license](LICENSE).

Adaptive appearance follows the background behind the search bar. Enable it with **Settings → Appearance → Adaptive → Allow Background** and grant macOS screen recording access. Argus samples brightness locally once as the panel opens, without a persistent recording stream or saved screen images. Without permission, appearance follows the system. Light and Dark modes remain available.

Switching desktops or full-screen spaces dismisses the search panel. Press the shortcut again to open it on the current desktop with a fresh background sample.
