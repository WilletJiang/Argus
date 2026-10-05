import Foundation

final class DictionaryProvider {
    enum Lookup { case completed(String?), cancelled }
    private struct Definition { let text: String?; let date: Date }
    private var cache: [String: Definition] = [:]
    private var order: [String] = []

    // Called on the system-provider queue; the supplied lookup owns cancellable process IO.
    func result(for query: String, lookup: (String) -> Lookup) -> SearchResult? {
        let prefixes = ["define ", "dict ", "词典 ", "释义 ", "定义 "]
        let prefix = prefixes.first { SearchText.normalized(query).hasPrefix($0) }
        let word = (prefix.map { String(query.dropFirst($0.count)) } ?? query).trimmingCharacters(in: .whitespaces)
        guard !word.isEmpty, word.count <= 64, word.split(whereSeparator: \.isWhitespace).count <= 4,
              word.unicodeScalars.contains(where: CharacterSet.letters.contains) else { return nil }
        let key = SearchText.normalized(word)
        let definition: String?
        if let cached = cache[key], Date().timeIntervalSince(cached.date) < 60 { definition = cached.text }
        else {
            switch lookup(word) {
            case .cancelled: return nil
            case .completed(let value): definition = value.map { compact($0) }
            }
            cache[key] = Definition(text: definition, date: Date())
            order.removeAll { $0 == key }
            order.append(key)
            if order.count > 128 { cache.removeValue(forKey: order.removeFirst()) }
        }
        guard definition != nil || prefix != nil else { return nil }
        let encoded = word.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? word
        return SearchResult(id: "dictionary:\(word)", title: "\(word) · Dictionary",
            subtitle: definition == nil ? "No local definition; open Dictionary to look it up" : "Local dictionary definition",
            kind: .dictionary, target: "dict://" + encoded, score: prefix == nil ? 520 : 1100,
            snippet: definition ?? "Choose and download local dictionaries in the Dictionary app settings.")
    }

    private func compact(_ text: String) -> String {
        String(text.split(whereSeparator: \.isWhitespace).joined(separator: " ").prefix(600))
    }
}
