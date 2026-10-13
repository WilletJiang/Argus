import Foundation

enum ResultRanking {
    /// A late result must beat an already visible one by a full match tier to move above it.
    static let displacementMargin = 150.0

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
        let boosts = Preferences.shared.boosts()
        return unique.values
            .map { ($0, $0.score + boosts[$0.target, default: 0]) }
            .sorted {
                if $0.1 != $1.1 { return $0.1 > $1.1 }
                let titleOrder = $0.0.title.localizedStandardCompare($1.0.title)
                return titleOrder == .orderedSame ? $0.0.id < $1.0.id : titleOrder == .orderedAscending
            }
            .map(\.0)
    }

    /// Keeps rows that are already on screen in place while later batches of the same query arrive.
    /// Newcomers enter in rank order and only move above a visible row they clearly outrank.
    static func stabilized(_ incoming: [SearchResult], after displayed: [SearchResult], margin: Double = displacementMargin) -> [SearchResult] {
        guard !displayed.isEmpty, !incoming.isEmpty else { return incoming }
        let boosts = Preferences.shared.boosts()
        var pending: [String: SearchResult] = [:]
        pending.reserveCapacity(incoming.count)
        for result in incoming where pending[result.id] == nil { pending[result.id] = result }
        var ordered: [(result: SearchResult, score: Double)] = []
        ordered.reserveCapacity(incoming.count)
        for old in displayed {
            if let result = pending.removeValue(forKey: old.id) { ordered.append((result, result.score + boosts[result.target, default: 0])) }
        }
        for result in incoming where pending.removeValue(forKey: result.id) != nil {
            let score = result.score + boosts[result.target, default: 0]
            let index = ordered.firstIndex { score > $0.score + margin } ?? ordered.count
            ordered.insert((result, score), at: index)
        }
        return ordered.map(\.result)
    }
}
