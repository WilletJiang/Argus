import AppKit
import ServiceManagement

final class PreferencesController: NSObject {
    private var window: NSWindow?
    private let index: FileIndex
    private let providers: SystemProviders
    private let hotkey: HotKey
    private let statusLabel = NSTextField(wrappingLabelWithString: "")
    private let excludedLabel = NSTextField(wrappingLabelWithString: "")
    private let shortcut = NSPopUpButton()
    private let shortcutHint = NSTextField(wrappingLabelWithString: "")
    private let appearance = NSPopUpButton()
    private let backgroundAccess = NSButton()
    private let appearanceHint = NSTextField(wrappingLabelWithString: "")
    var onHotkeyChanged: (() -> Void)?

    init(index: FileIndex, providers: SystemProviders, hotkey: HotKey) {
        self.index = index; self.providers = providers; self.hotkey = hotkey
        super.init()
    }

    func show() {
        if window == nil { build() }
        refresh()
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }

    func updateStatus() { if window?.isVisible == true { refresh() } }

    private func build() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 620, height: 678), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Argus Settings"
        window.isReleasedWhenClosed = false
        window.center()
        self.window = window
        let view = FlippedView(frame: NSRect(x: 0, y: 0, width: 620, height: 678))
        window.contentView = view
        func label(_ text: String, _ y: CGFloat, size: CGFloat = 13, weight: NSFont.Weight = .regular) {
            let item = NSTextField(wrappingLabelWithString: text)
            item.font = .systemFont(ofSize: size, weight: weight)
            item.frame = NSRect(x: 28, y: y, width: 560, height: size > 20 ? 36 : 40)
            view.addSubview(item)
        }
        label("Argus", 20, size: 26, weight: .semibold)
        label("Find fast. Open effortlessly.", 59)
        label("Preferred shortcut", 111, weight: .medium)
        shortcut.addItems(withTitles: HotKey.titles)
        shortcut.selectItem(at: Preferences.shared.hotkey)
        shortcut.frame = NSRect(x: 300, y: 104, width: 130, height: 30)
        shortcut.target = self; shortcut.action = #selector(shortcutChanged)
        view.addSubview(shortcut)
        button("Keyboard Shortcuts", x: 443, y: 107, width: 150, action: #selector(openShortcuts), parent: view)
        shortcutHint.font = .systemFont(ofSize: 11.5)
        shortcutHint.textColor = .secondaryLabelColor
        shortcutHint.frame = NSRect(x: 28, y: 141, width: 560, height: 40)
        view.addSubview(shortcutHint)
        NotificationCenter.default.addObserver(self, selector: #selector(windowBecameKey), name: NSWindow.didBecomeKeyNotification, object: window)
        let content = NSButton(checkboxWithTitle: "Index file contents (on-device; cache limit: 128 MB)", target: self, action: #selector(contentChanged))
        content.state = Preferences.shared.contentIndexing ? .on : .off
        content.frame = NSRect(x: 28, y: 187, width: 550, height: 26)
        view.addSubview(content)
        let login = NSButton(checkboxWithTitle: "Launch at login", target: self, action: #selector(loginChanged))
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        login.frame = NSRect(x: 28, y: 224, width: 250, height: 26)
        view.addSubview(login)
        button("Rescan", x: 445, y: 222, width: 148, action: #selector(rescan), parent: view)
        label("Search locations", 270, weight: .medium)
        label("Your home folder and installed apps. Skips caches, hidden folders, and build output.", 299, size: 11.5)
        excludedLabel.font = .systemFont(ofSize: 11.5)
        excludedLabel.textColor = .secondaryLabelColor
        excludedLabel.frame = NSRect(x: 28, y: 326, width: 550, height: 38)
        view.addSubview(excludedLabel)
        button("Exclude Folder…", x: 28, y: 370, width: 130, action: #selector(addExclusion), parent: view)
        button("Clear Exclusions", x: 166, y: 370, width: 156, action: #selector(clearExclusions), parent: view)
        button("Enable Contacts", x: 28, y: 423, width: 130, action: #selector(contacts), parent: view)
        button("Disk Access", x: 166, y: 423, width: 156, action: #selector(diskAccess), parent: view)
        button("Open Index Folder", x: 330, y: 423, width: 148, action: #selector(openStorage), parent: view)
        statusLabel.font = .systemFont(ofSize: 11.5)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.frame = NSRect(x: 28, y: 479, width: 550, height: 70)
        view.addSubview(statusLabel)
        label("Appearance", 570, weight: .medium)
        appearance.addItems(withTitles: AppearanceMode.allCases.map(\.title))
        appearance.frame = NSRect(x: 300, y: 563, width: 150, height: 30)
        appearance.target = self
        appearance.action = #selector(appearanceChanged)
        view.addSubview(appearance)
        backgroundAccess.title = "Allow Background"
        backgroundAccess.bezelStyle = .rounded
        backgroundAccess.frame = NSRect(x: 460, y: 563, width: 133, height: 30)
        backgroundAccess.target = self
        backgroundAccess.action = #selector(allowBackground)
        view.addSubview(backgroundAccess)
        appearanceHint.font = .systemFont(ofSize: 11.5)
        appearanceHint.textColor = .secondaryLabelColor
        appearanceHint.frame = NSRect(x: 28, y: 603, width: 560, height: 52)
        view.addSubview(appearanceHint)
    }

    private func button(_ title: String, x: CGFloat, y: CGFloat, width: CGFloat, action: Selector, parent: NSView) {
        let button = NSButton(title: title, target: self, action: action)
        button.bezelStyle = .rounded
        button.frame = NSRect(x: x, y: y, width: width, height: 30)
        parent.addSubview(button)
    }

    private func refresh() {
        appearance.selectItem(at: Preferences.shared.appearanceMode.rawValue)
        let adaptive = Preferences.shared.appearanceMode == .adaptive
        let allowed = CGPreflightScreenCaptureAccess()
        backgroundAccess.isHidden = !adaptive || allowed
        appearanceHint.stringValue = !adaptive ? "Use a fixed appearance for the search panel." : allowed ?
            "Matches the background each time search opens. Brightness is processed on this Mac; no images are saved." :
            "Allow screen recording access to match the background behind the search bar. Until allowed, appearance follows the system. No images are saved."
        shortcut.selectItem(at: Preferences.shared.hotkey)
        shortcutHint.stringValue = hotkey.notice
        excludedLabel.stringValue = Preferences.shared.excludedPaths.isEmpty ? "No custom excluded folders." : Preferences.shared.excludedPaths.joined(separator: "\n")
        statusLabel.stringValue = "\(index.status) · \(index.count.formatted()) items\n\(providers.status)\nShortcut: \(hotkey.description) · Index stays on this Mac. Queries and file contents are never uploaded."
    }

    @objc private func appearanceChanged() {
        guard let mode = AppearanceMode(rawValue: appearance.indexOfSelectedItem) else { return }
        Preferences.shared.appearanceMode = mode
        mode.apply()
        refresh()
    }

    @objc private func allowBackground() {
        if !CGRequestScreenCaptureAccess() {
            NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!)
        }
        refresh()
    }

    @objc private func shortcutChanged() {
        let choice = shortcut.indexOfSelectedItem
        let success = hotkey.register(choice: choice)
        if success { Preferences.shared.hotkey = choice }
        onHotkeyChanged?()
        refresh()
        if !success { statusLabel.stringValue = "Shortcut registration failed. Try another combination." }
    }
    @objc private func windowBecameKey() {
        hotkey.register(choice: Preferences.shared.hotkey)
        onHotkeyChanged?()
        refresh()
    }
    @objc private func contentChanged(_ sender: NSButton) {
        Preferences.shared.contentIndexing = sender.state == .on
        index.contentPreferenceChanged()
        refresh()
    }
    @objc private func loginChanged(_ sender: NSButton) {
        do {
            if sender.state == .on { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
        } catch { sender.state = SMAppService.mainApp.status == .enabled ? .on : .off; statusLabel.stringValue = "Could not change launch-at-login setting: \(error.localizedDescription)" }
    }
    @objc private func rescan() { index.rebuild(); statusLabel.stringValue = "Background scan scheduled. Search remains available." }
    @objc private func contacts() { providers.requestContactsAccess { [weak self] message in self?.statusLabel.stringValue = message } }
    @objc private func diskAccess() { NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")!) }
    @objc private func openShortcuts() { NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.keyboard?Shortcuts")!) }
    @objc private func openStorage() { NSWorkspace.shared.open(FileIndex.storageURL) }
    @objc private func addExclusion() {
        let picker = NSOpenPanel()
        picker.canChooseFiles = false; picker.canChooseDirectories = true; picker.allowsMultipleSelection = true
        picker.prompt = "Exclude"
        guard let window else { return }
        picker.beginSheetModal(for: window) { [weak self] response in
            guard response == .OK else { return }
            Preferences.shared.excludedPaths = Array(Set(Preferences.shared.excludedPaths + picker.urls.map(\.path))).sorted()
            self?.index.exclusionsChanged()
            self?.refresh()
        }
    }
    @objc private func clearExclusions() { Preferences.shared.excludedPaths = []; index.rebuild(); refresh() }
    deinit { NotificationCenter.default.removeObserver(self) }
}
