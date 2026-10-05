import Foundation

enum ResultRanking {
    static func merge(local: [SearchResult], supplemental: [SearchResult], content: [SearchResult]) -> [SearchResult] {
        let strongMatch = local.contains { $0.score >= 750 }
        let candidates = local.filter { !strongMatch || $0.score >= 300 || $0.kind == .content }
        var unique: [String: SearchResult] = [:]
        for result in candidates + supplemental + content {
            guard var existing = unique[result.id] else { unique[result.id] = result; continue }
            let score = max(existing.score, result.score)
            // A verified body match is more useful than a filename-only duplicate.
            if result.kind == .content && existing.kind != .content { existing = result }
            existing.score = score
            unique[result.id] = existing
        }
        return unique.values.sorted {
            let left = $0.score + Preferences.shared.boost(for: $0.target)
            let right = $1.score + Preferences.shared.boost(for: $1.target)
            if left != right { return left > right }
            let titleOrder = $0.title.localizedStandardCompare($1.title)
            return titleOrder == .orderedSame ? $0.id < $1.id : titleOrder == .orderedAscending
        }
    }
}
