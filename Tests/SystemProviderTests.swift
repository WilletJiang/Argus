import AppKit

@main
struct SystemProviderTests {
    static func main() {
        if ScanWorker.runIfRequested() { return }
        let providers = SystemProviders()

        func wait(_ condition: () -> Bool, timeout: TimeInterval = 4) {
            let deadline = Date(timeIntervalSinceNow: timeout)
            while !condition(), Date() < deadline {
                RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            precondition(condition(), "Asynchronous operation timed out")
        }

        func query(_ text: String, scope: SearchScope = .all) -> [SearchResult] {
            var batch: [SearchResult]?
            providers.search(text, scope: scope) {
                precondition(Thread.isMainThread, "Completion must run on the main thread")
                batch = $0
            }
            wait { batch != nil }
            if text.hasPrefix("define ") { wait { batch?.contains { $0.kind == .dictionary } == true } }
            return batch!
        }

        let suggestions = query("", scope: .actions)
        precondition(suggestions.count == 8 && suggestions.allSatisfy { $0.kind == .action })
        precondition(query("jietu", scope: .actions).contains { $0.title == "Screenshot & Recording" })
        precondition(query("JIE TU", scope: .actions).contains { $0.title == "Screenshot & Recording" })
        precondition(query("調高音量", scope: .actions).allSatisfy { $0.kind == .action })
        precondition(query("调高音量", scope: .actions).first?.target == "argus-action:volume-up")
        precondition(query("bluetooth", scope: .actions).contains { $0.title == "Bluetooth Settings" })
        precondition(query("system settings", scope: .files).isEmpty)
        precondition(query("system settings", scope: .applications).isEmpty)
        precondition(query("define notarealwordargus").contains {
            $0.kind == .dictionary && $0.target.hasPrefix("dict:") && !$0.snippet.isEmpty
        })

        let web = query("web C++ & 中 文")
        precondition(web.count == 3 && web.allSatisfy { $0.kind == .web })
        for result in web {
            precondition(result.target.contains("C%2B%2B") && result.target.contains("%26"))
            let components = URLComponents(string: result.target)!
            precondition(components.queryItems?.first?.value == "C++ & 中 文")
        }
        precondition(query("ddg offline").count == 1)
        precondition(query("google offline").first?.target.hasPrefix("https://www.google.com/") == true)
        precondition(query("bing offline").first?.target.hasPrefix("https://www.bing.com/") == true)
        precondition(query("https://example.com/docs").first?.target == "https://example.com/docs")
        precondition(query("regularquery").allSatisfy { $0.kind != .web })
        precondition(query("web offline", scope: .people).allSatisfy { $0.kind == .contact || $0.kind == .mail })

        var obsolete = false
        var latest = false
        providers.search("obsolete-mail-query", scope: .people) { _ in obsolete = true }
        providers.search("web latest", scope: .all) { _ in latest = true }
        wait { latest }
        precondition(!obsolete, "A superseded query must never publish")

        var cancelled = false
        providers.search("cancelled-mail-query", scope: .people) { _ in cancelled = true }
        providers.cancelSearch()
        RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.15))
        precondition(!cancelled, "An explicitly cancelled query must never publish")

        var mailStages = 0
        providers.search("argus-unlikely-mail-token", scope: .people) { results in
            precondition(Thread.isMainThread)
            precondition(results.allSatisfy { $0.kind == .contact || $0.kind == .mail })
            precondition(results.filter { $0.kind == .mail }.count <= 30)
            mailStages += 1
        }
        wait({ mailStages >= 2 }, timeout: 5)
        providers.cancelSearch()

        var actionCompleted = false
        let unsupported = SearchResult(id: "test", title: "Test", subtitle: "", kind: .action,
                                       target: "argus-action:unsupported", score: 0)
        ActionExecutor.execute(unsupported) { error in
            precondition(Thread.isMainThread && error != nil)
            actionCompleted = true
        }
        wait { actionCompleted }

        providers.prepare()
        RunLoop.current.run(until: Date(timeIntervalSinceNow: 1))
        let shortcuts = query("快捷指令", scope: .actions).filter { $0.kind == .shortcut }
        precondition(shortcuts.allSatisfy { UUID(uuidString: $0.target) != nil })
        precondition(!providers.status.isEmpty)
        print("System provider tests passed")
    }
}
