import AppKit
import PDFKit
import Darwin

@main
enum FileIndexTests {
    static func main() throws {
        if ScanWorker.runIfRequested() { return }
        if CommandLine.arguments.contains("--parent-exit-probe") {
            let child = Process(), input = Pipe(), output = Pipe()
            child.executableURL = Bundle.main.executableURL
            child.arguments = ["--scan-worker"]
            child.standardInput = input; child.standardOutput = output
            child.standardError = FileHandle.nullDevice
            try child.run()
            var request = ScanRequest(mode: .directory, path: FileManager.default.temporaryDirectory.path)
            request.delayForTesting = 10
            var data = try JSONEncoder().encode(request); data.append(10)
            try input.fileHandleForWriting.write(contentsOf: data)
            _ = try output.fileHandleForReading.read(upToCount: 512)
            print(child.processIdentifier)
            fflush(stdout)
            _exit(0)
        }
        let resolved = realpath(FileManager.default.temporaryDirectory.path, nil)!
        let temporary = URL(fileURLWithPath: String(cString: resolved))
        free(resolved)
        let fixture = temporary.appendingPathComponent("argus-index-test-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: fixture, withIntermediateDirectories: true)
        let root = fixture.resolvingSymlinksInPath()
        let home = root.appendingPathComponent("home"), apps = root.appendingPathComponent("Applications"), storage = root.appendingPathComponent("database")
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: apps, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let document = home.appendingPathComponent("项目汇报.txt")
        try "Safari 年度预算 uniqueannualbudget 你好世界".write(to: document, atomically: true, encoding: .utf8)
        let appNameCollision = home.appendingPathComponent("Safari.txt")
        try "ordinary app-name collision".write(to: appNameCollision, atomically: true, encoding: .utf8)
        let large = home.appendingPathComponent("大文档.txt")
        try String(repeating: "中", count: 11000).write(to: large, atomically: true, encoding: .utf8)
        let extracted = ContentExtractor.extract(large)
        precondition(extracted.contains("中") && extracted.utf8.count <= ContentExtractor.byteLimit && !extracted.contains("�"))
        let unicode = home.appendingPathComponent("utf16.txt")
        try "UTF16 你好🧭".write(to: unicode, atomically: true, encoding: .utf16)
        precondition(ContentExtractor.extract(unicode).contains("你好🧭"))
        let realApp = root.appendingPathComponent("Safari.app/Contents")
        try FileManager.default.createDirectory(at: realApp, withIntermediateDirectories: true)
        let plist: [String: Any] = ["CFBundleName": "Safari", "CFBundleIdentifier": "test.argus.safari", "CFBundlePackageType": "APPL"]
        try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0).write(to: realApp.appendingPathComponent("Info.plist"))
        let link = apps.appendingPathComponent("Safari.app")
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: realApp.deletingLastPathComponent())
        let flags = Process(); flags.executableURL = URL(fileURLWithPath: "/usr/bin/chflags"); flags.arguments = ["-h", "hidden", link.path]; try flags.run(); flags.waitUntilExit()
        let dependency = home.appendingPathComponent("node_modules/not-indexed.txt")
        try FileManager.default.createDirectory(at: dependency.deletingLastPathComponent(), withIntermediateDirectories: true)
        try "unwanteddependency".write(to: dependency, atomically: true, encoding: .utf8)
        let privateTrack = home.appendingPathComponent("Music/Music/private-library-track.txt")
        let ordinaryTrack = home.appendingPathComponent("Music/ordinary-track.txt")
        try FileManager.default.createDirectory(at: privateTrack.deletingLastPathComponent(), withIntermediateDirectories: true)
        try "managedmusiclibraryprivatecontent".write(to: privateTrack, atomically: true, encoding: .utf8)
        try "ordinarymusictrackcontent".write(to: ordinaryTrack, atomically: true, encoding: .utf8)

        func wait(_ condition: () -> Bool, timeout: Double = 8, file: StaticString = #file, line: UInt = #line) {
            let deadline = Date(timeIntervalSinceNow: timeout)
            while !condition(), Date() < deadline { RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.01)) }
            precondition(condition(), "Index operation timed out", file: file, line: line)
        }
        let recoveryClient = ScanClient(idleTimeout: 0.4)
        var delayed = ScanRequest(mode: .directory, path: home.path)
        delayed.delayForTesting = 0.8
        var timeoutResult: ScanResult?
        recoveryClient.perform(delayed) { result in DispatchQueue.main.async { timeoutResult = result } }
        wait { timeoutResult != nil }
        precondition(timeoutResult?.outcome == .timedOut && timeoutResult?.blockedPath == home.path)
        var recovered: ScanResult?, recoveredEntries: [ScanEntry] = []
        recoveryClient.perform(ScanRequest(mode: .directory, path: ordinaryTrack.deletingLastPathComponent().path), onBatch: { entries in
            DispatchQueue.main.async { recoveredEntries += entries }
        }) { result in DispatchQueue.main.async { recovered = result } }
        wait { recovered != nil }
        precondition(recovered?.outcome == .completed && recoveredEntries.contains { $0.path == ordinaryTrack.path })
        delayed.delayForTesting = 1
        var cancelled: ScanResult?
        recoveryClient.perform(delayed) { result in DispatchQueue.main.async { cancelled = result } }
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.1) { recoveryClient.cancel() }
        wait { cancelled != nil }
        precondition(cancelled?.outcome == .cancelled)
        recoveryClient.close()
        let probe = Process(), probeOutput = Pipe()
        probe.executableURL = Bundle.main.executableURL
        probe.arguments = ["--parent-exit-probe"]
        probe.standardOutput = probeOutput
        try probe.run()
        let identifierText = String(data: probeOutput.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8)!
        probe.waitUntilExit()
        let orphan = pid_t(identifierText.trimmingCharacters(in: .whitespacesAndNewlines))!
        defer { if kill(orphan, 0) == 0 { kill(orphan, SIGKILL) } }
        wait({ kill(orphan, 0) != 0 }, timeout: 3)
        recoveryClient.stop()
        var stoppedResult: ScanResult?
        recoveryClient.perform(ScanRequest(mode: .directory, path: home.path)) { result in DispatchQueue.main.async { stoppedResult = result } }
        wait { stoppedResult != nil }
        precondition(stoppedResult?.outcome == .cancelled, "A stopped client must never launch a new worker")

        let localizedTable: [String: Any] = ["en": ["CFBundleDisplayName": "Safari"], "zh_CN": ["CFBundleDisplayName": "浏览器验证"]]
        let resources = realApp.appendingPathComponent("Resources")
        try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: true)
        try PropertyListSerialization.data(fromPropertyList: localizedTable, format: .binary, options: 0).write(to: resources.appendingPathComponent("InfoPlist.loctable"))
        var ready = false
        let index = FileIndex(home: home, storage: storage, applications: [apps])
        index.onStatus = { text, _ in if text.hasPrefix("Index ready") { ready = true }; precondition(!text.contains("failed"), text) }
        index.start()
        wait { ready }
        func query(_ text: String, scope: SearchScope = .all) -> [SearchResult] {
            var results: [SearchResult]?
            index.search(text, scope: scope) { values, _ in precondition(Thread.isMainThread); results = values }
            wait { results != nil }
            return results!
        }
        let applicationResults = query("Safari", scope: .applications)
        precondition(applicationResults.first?.target == link.path, "Hidden system application links must remain searchable")
        precondition(query("Safari").first?.target == link.path, "An exact English app basename must outrank a same-name file when the app display name is localized")
        precondition(query("浏览器验证", scope: .applications).contains { $0.target == link.path }, "Actual localized application names must match")
        precondition(query("llqyz", scope: .applications).contains { $0.target == link.path }, "Localized application pinyin initials must match")
        precondition(query("xmhb").contains { $0.target == document.path }, "Chinese initials must match")
        precondition(query("xiangmu").contains { $0.target == document.path }, "Pinyin must match")
        precondition(query("uniqueannualbudget").contains { $0.target == document.path && $0.kind == .content })
        precondition(query("unwanteddependency").isEmpty)
        precondition(query("private-library-track").allSatisfy { $0.target != privateTrack.path }, "Managed Music library filenames must be excluded")
        precondition(query("managedmusiclibraryprivatecontent").isEmpty, "Managed Music library content must be excluded")
        precondition(query("ordinary-track").contains { $0.target == ordinaryTrack.path }, "Ordinary music files must remain searchable")
        precondition(query("ordinarymusictrackcontent").contains { $0.target == ordinaryTrack.path && $0.kind == .content })
        precondition(query("Safari", scope: .files).contains { $0.target == appNameCollision.path })
        precondition(query("Safari", scope: .files).allSatisfy { $0.kind != .application })
        var batches: [[SearchResult]] = []
        index.search("Safari", scope: .all) { values, _ in batches.append(values) }
        wait { batches.count == 2 }
        precondition(batches[0].first?.target == link.path && !batches[0].contains { $0.target == document.path })
        precondition(batches[1].contains { $0.target == document.path && $0.kind == .content }, "Body matches must supplement the first metadata batch")
        var staleCallbacks = 0
        var latest: [SearchResult]?
        index.search("Safari", scope: .all) { _, _ in staleCallbacks += 1 }
        index.search("uniqueannualbudget", scope: .all) { values, _ in latest = values }
        wait { latest != nil }
        RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.05))
        precondition(staleCallbacks == 0 && latest?.contains { $0.target == document.path && $0.kind == .content } == true, "An older progressive search must never publish over a newer query")
        let created = home.appendingPathComponent("新增验证.txt")
        try "eventstreamtest".write(to: created, atomically: true, encoding: .utf8)
        wait({ query("新增验证").contains { $0.target == created.path } }, timeout: 8)
        let moved = home.appendingPathComponent("重命名验证.txt")
        try FileManager.default.moveItem(at: created, to: moved)
        wait({ query("重命名验证").contains { $0.target == moved.path } && query("新增验证").isEmpty }, timeout: 8)
        try FileManager.default.removeItem(at: moved)
        wait { query("重命名验证").isEmpty }
        let emoji = "中👨‍👩‍👧‍👦a"
        precondition((emoji as NSString).length == 13)
        index.stop()
        print("File index tests passed: worker timeout/recovery/cancellation/parent-exit/shutdown, progressive results/stale-query suppression, localized hidden app, pinyin, content, UTF-8/UTF-16, managed Music exclusion, filters, live create/rename/delete")
    }
}
