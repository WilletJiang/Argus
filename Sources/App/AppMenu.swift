import AppKit

enum AppMenu {
    static func install(target: NSObject, search: Selector, settings: Selector, quit: Selector) -> NSMenuItem {
        let bar = NSMenu()
        let application = NSMenu(title: "Argus")
        let toggle = application.addItem(withTitle: "Show / Hide Search", action: search, keyEquivalent: "")
        toggle.target = target
        let preferences = application.addItem(withTitle: "Settings…", action: settings, keyEquivalent: ",")
        preferences.target = target
        application.addItem(.separator())
        let exit = application.addItem(withTitle: "Quit Argus", action: quit, keyEquivalent: "q")
        exit.target = target
        let applicationItem = bar.addItem(withTitle: "Argus", action: nil, keyEquivalent: "")
        applicationItem.submenu = application

        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        let redo = edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "z")
        redo.keyEquivalentModifierMask = [.command, .shift]
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        let editItem = bar.addItem(withTitle: "Edit", action: nil, keyEquivalent: "")
        editItem.submenu = edit
        NSApp.mainMenu = bar
        return toggle
    }
}
