import AppKit

enum SearchKind: String, Codable {
    case application, file, folder, content, action, shortcut, contact, mail, web, dictionary

    var label: String {
        switch self {
        case .application: return "App"
        case .file: return "File"
        case .folder: return "Folder"
        case .content: return "File contents"
        case .action: return "System action"
        case .shortcut: return "Shortcut"
        case .contact: return "Contact"
        case .mail: return "Mail"
        case .web: return "Web search"
        case .dictionary: return "Dictionary"
        }
    }

    var symbol: String {
        switch self {
        case .application: return "app.fill"
        case .file, .content: return "doc.text"
        case .folder: return "folder.fill"
        case .action: return "gearshape"
        case .shortcut: return "bolt.fill"
        case .contact: return "person.crop.circle"
        case .mail: return "envelope"
        case .web: return "globe"
        case .dictionary: return "character.book.closed"
        }
    }
}

struct SearchResult: Identifiable {
    var id: String
    var title: String
    var subtitle: String
    var kind: SearchKind
    var target: String
    var score: Double
    var snippet: String = ""
    var symbol: String? = nil
}

enum SearchScope: Int, CaseIterable {
    case all, applications, files, actions, people

    var title: String {
        switch self {
        case .all: return "All"
        case .applications: return "Apps"
        case .files: return "Files"
        case .actions: return "Actions"
        case .people: return "Contacts & Mail"
        }
    }

    func includes(_ kind: SearchKind) -> Bool {
        switch self {
        case .all: return true
        case .applications: return kind == .application
        case .files: return [.file, .folder, .content].contains(kind)
        case .actions: return [.action, .shortcut].contains(kind)
        case .people: return [.contact, .mail].contains(kind)
        }
    }
}

enum SearchText {
    static func normalized(_ value: String) -> String {
        value.folding(options: [.caseInsensitive, .diacriticInsensitive, .widthInsensitive], locale: Locale(identifier: "en_US_POSIX")).lowercased()
    }

    static func aliases(_ value: String) -> String {
        guard value.unicodeScalars.contains(where: { $0.value >= 0x3400 && $0.value <= 0x9fff }) else { return "" }
        let latin = value.applyingTransform(.toLatin, reverse: false) ?? value
        let words = normalized(latin).split(whereSeparator: { !$0.isLetter && !$0.isNumber })
        return words.joined(separator: " ") + " " + words.joined() + " " + String(words.compactMap(\.first))
    }

    static func score(_ query: String, in value: String) -> Double {
        let q = normalized(query), text = normalized(value)
        guard !q.isEmpty else { return 0 }
        if text == q { return 1000 }
        if text.hasPrefix(q) { return 850 }
        if text.contains(q) { return 650 }
        let tokens = q.split(whereSeparator: \.isWhitespace)
        if tokens.allSatisfy({ text.contains($0) }) { return 550 }
        if aliases(value).contains(q) { return 500 }
        return 0
    }
}
