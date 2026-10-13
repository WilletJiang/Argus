import Foundation

@main
enum ResultRankingTests {
    static func main() {
        func hit(_ id: String, _ kind: SearchKind, _ score: Double, snippet: String = "") -> SearchResult {
            SearchResult(id: id, title: id, subtitle: id, kind: kind, target: "/argus-test/" + id, score: score, snippet: snippet)
        }
        let weak = hit("document", .file, 222)
        let metadata = hit("document", .content, 300, snippet: "Content match · System index")
        let merged = ResultRanking.merge(local: [weak], supplemental: [], content: [metadata])
        precondition(merged.count == 1 && merged[0].score == 300 && merged[0].kind == .content)

        let app = hit("Safari", .application, 1035)
        let body = hit("notes", .content, 230, snippet: "Safari settings")
        let fuzzy = hit("Samara", .file, 222)
        let exact = ResultRanking.merge(local: [app, body, fuzzy], supplemental: [], content: [])
        precondition(exact.map(\.id) == ["Safari", "notes"], "Exact apps must suppress fuzzy noise while retaining verified body hits")
        precondition(ResultRanking.merge(local: [fuzzy], supplemental: [], content: []).count == 1, "Fuzzy rescue must still work without a strong match")

        let filename = hit("notes", .file, 900)
        let detailed = ResultRanking.merge(local: [filename, body], supplemental: [], content: [metadata])
        precondition(detailed.first?.score == 900 && detailed.first?.snippet == body.snippet)

        let shown = [hit("Safari", .application, 850), hit("Safe", .file, 650)]
        let sameTier = ResultRanking.stabilized([hit("Safety", .file, 900), shown[0], shown[1]], after: shown)
        precondition(sameTier.map(\.id) == ["Safari", "Safety", "Safe"], "A late result within one tier must not displace the visible top hit")
        let clearlyBetter = ResultRanking.stabilized([hit("web", .web, 1200), shown[0], shown[1]], after: shown)
        precondition(clearlyBetter.map(\.id) == ["web", "Safari", "Safe"], "A late result a full tier higher must take the top")
        let reordered = ResultRanking.stabilized([shown[1], shown[0], hit("tail", .file, 100)], after: shown)
        precondition(reordered.map(\.id) == ["Safari", "Safe", "tail"], "Visible rows keep their order; weaker newcomers append")
        let dropped = ResultRanking.stabilized([shown[1]], after: shown)
        precondition(dropped.map(\.id) == ["Safe"], "Rows missing from the new batch disappear")
        precondition(ResultRanking.stabilized(shown, after: []).map(\.id) == shown.map(\.id))
        print("Result ranking tests passed: best-score deduplication, body evidence, fuzzy rescue, stable late batches")
    }
}
