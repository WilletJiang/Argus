import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var index: FileIndex!
    private var providers: SystemProviders!
    private var coordinator: SearchCoordinator!
    private var panel: PanelController!
    private var preferences: PreferencesController!
    private let hotkey = HotKey()
    private var item: NSStatusItem!
    private var searchMenuItem: NSMenuItem!
#if ARGUS_BENCHMARK
    private var benchmark: AppPerformance?
#endif

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        Preferences.shared.appearanceMode.apply()
        searchMenuItem = AppMenu.install(target: self, search: #selector(toggleSearch), settings: #selector(showPreferences), quit: #selector(quit))
        index = FileIndex()
        providers = SystemProviders()
        coordinator = SearchCoordinator(index: index, providers: providers)
        panel = PanelController(coordinator: coordinator)
        preferences = PreferencesController(index: index, providers: providers, hotkey: hotkey)
        panel.onSettings = { [weak self] in self?.panel.hide(); self?.preferences.show() }
        index.onStatus = { [weak self] text, _ in
            self?.panel.updateStatus(text)
            self?.preferences.updateStatus()
        }
        hotkey.onPress = { [weak self] in self?.panel.toggle() }
        panel.isToggleShortcut = { [weak self] event in self?.hotkey.matches(event) == true }
        hotkey.register(choice: Preferences.shared.hotkey)
        preferences.onHotkeyChanged = { [weak self] in
            self?.updateHotkey()
        }
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = NSImage(systemSymbolName: "square.stack.3d.up", accessibilityDescription: "Argus")
        let menu = NSMenu()
        menu.addItem(withTitle: "Search…", action: #selector(showSearch), keyEquivalent: "").target = self
        menu.addItem(withTitle: "Settings…", action: #selector(showPreferences), keyEquivalent: ",").target = self
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit Argus", action: #selector(quit), keyEquivalent: "q").target = self
        item.menu = menu
        updateHotkey()
        providers.prepare()
        index.start()
        let arguments = CommandLine.arguments
#if ARGUS_BENCHMARK
        panel.onMetric = { [weak self] name, value in
            self?.benchmark?.record(name, value)
        }
        if let flag = arguments.firstIndex(of: "--benchmark"), arguments.indices.contains(flag + 1) {
            benchmark = AppPerformance(panel: panel, index: index, output: arguments[flag + 1])
            benchmark?.start()
            return
        }
#endif
        if let flag = arguments.firstIndex(of: "--query"), arguments.indices.contains(flag + 1) { panel.show(query: arguments[flag + 1]) }
        else if !arguments.contains("--background") { panel.show() }
    }

    private func updateHotkey() {
        item.button?.toolTip = "Argus · " + hotkey.description
        searchMenuItem.keyEquivalent = hotkey.activeChoice == nil ? "" : " "
        searchMenuItem.keyEquivalentModifierMask = hotkey.menuModifiers
    }

    @objc private func showSearch() { panel.show() }
    @objc private func toggleSearch() { panel.toggle() }
    @objc private func showPreferences() { panel.hide(); preferences.show() }
    @objc private func quit() { NSApp.terminate(nil) }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if panel?.isPresented != true { panel?.show() }
        return true
    }
    func applicationWillTerminate(_ notification: Notification) {
        panel.hide(immediately: true)
        providers.cancelSearch()
        index.stop()
    }
}

@main
enum ArgusApp {
    static func main() {
        if ScanWorker.runIfRequested() { return }
        let application = NSApplication.shared
        let delegate = AppDelegate()
        application.delegate = delegate
        withExtendedLifetime(delegate) { application.run() }
    }
}
