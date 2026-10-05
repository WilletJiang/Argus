import AppKit

enum AppearanceMode: Int, CaseIterable {
    case system, light, dark

    var title: String {
        switch self {
        case .system: return "System"
        case .light: return "Light"
        case .dark: return "Dark"
        }
    }

    func apply() {
        switch self {
        case .system: NSApp.appearance = nil
        case .light: NSApp.appearance = NSAppearance(named: .aqua)
        case .dark: NSApp.appearance = NSAppearance(named: .darkAqua)
        }
    }
}

final class Preferences {
    static let shared = Preferences()
    private let defaults = UserDefaults.standard

    var contentIndexing: Bool {
        get { defaults.object(forKey: "contentIndexing") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "contentIndexing") }
    }
    var excludedPaths: [String] {
        get { defaults.stringArray(forKey: "excludedPaths") ?? [] }
        set { defaults.set(newValue, forKey: "excludedPaths") }
    }
    var hotkey: Int {
        get { defaults.object(forKey: "hotkey") as? Int ?? 0 }
        set { defaults.set(newValue, forKey: "hotkey") }
    }
    var appearanceMode: AppearanceMode {
        get { AppearanceMode(rawValue: defaults.integer(forKey: "appearanceMode")) ?? .system }
        set { defaults.set(newValue.rawValue, forKey: "appearanceMode") }
    }
    var recentTargets: [String] { defaults.stringArray(forKey: "recentTargets") ?? [] }

    func record(_ result: SearchResult) {
        guard [.application, .file, .folder].contains(result.kind) else { return }
        var targets = recentTargets.filter { $0 != result.target }
        targets.insert(result.target, at: 0)
        defaults.set(Array(targets.prefix(40)), forKey: "recentTargets")
    }

    func boost(for target: String) -> Double {
        guard let index = recentTargets.firstIndex(of: target) else { return 0 }
        return Double(max(0, 40 - index)) * 1.5
    }
}
