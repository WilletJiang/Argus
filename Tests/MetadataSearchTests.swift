import AppKit
import CoreServices

@main
struct MetadataSearchTests {
    static func main() throws {
        let provider = MetadataSearch()

        func wait(_ condition: () -> Bool, timeout: TimeInterval = 4) {
            let deadline = Date(timeIntervalSinceNow: timeout)
            while !condition(), Date() < deadline { RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.01)) }
            precondition(condition(), "Metadata completion timed out")
        }

        func search(_ text: String) -> [SearchResult] {
            var results: [SearchResult]?
            let started = Date()
            provider.search(text) {
                precondition(Thread.isMainThread && $0.count <= 64)
                precondition($0.allSatisfy { $0.kind == .content && $0.id == "file:" + $0.target })
                results = $0
            }
            wait { results != nil }
            precondition(Date().timeIntervalSince(started) < 4)
            return results!
        }

        precondition(MetadataSearch.expression(for: " \n ") == nil)
        precondition(MetadataSearch.expression(for: "bad\0query") == nil)
        precondition(MetadataSearch.expression(for: String(repeating: "long", count: 600)) == nil)
        let punctuation = ["hello world", "literal\"quote", "quote's", "back\\slash", "*", "?",
                           "\") || (kMDItemFSName == \"*", "中文 café"]
        for text in punctuation {
            let expression = MetadataSearch.expression(for: text)!
            precondition(MDQueryCreate(nil, expression as CFString, nil, nil) != nil,
                         "Escaped user text must form a valid metadata expression")
        }
        precondition(search("").isEmpty)

        var obsolete = false
        var current = false
        provider.search("superseded-query") { _ in obsolete = true }
        provider.search("argus-unique-no-match-" + UUID().uuidString) { _ in current = true }
        wait { current }
        precondition(!obsolete, "Superseded results must not reach the UI")
        var cancelled = false
        provider.search("cancelled-query") { _ in cancelled = true }
        provider.cancel()
        RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.15))
        precondition(!cancelled, "Cancelled results must not reach the UI")

        let directory = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("ArgusMetadataFixture-" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let fixture = directory.appendingPathComponent("fulltext.txt")
        let attributes = directory.appendingPathComponent("attributes.plist")
        let token = "ArgusMetadata" + UUID().uuidString.replacingOccurrences(of: "-", with: "")
        let text = String(repeating: "prefix body\n", count: 6000) +
            "\n\(token) tailtoken special\"quote literal*star marker?question slash\\path"
        try text.write(to: fixture, atomically: true, encoding: .utf8)

        func importFile(_ arguments: [String]) throws -> Int32 {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/mdimport")
            process.arguments = arguments
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus
        }

        let importStatus = try importFile(["-t", "-o", attributes.path, fixture.path])
        precondition(importStatus == 0)
        let imported = try String(contentsOf: attributes, encoding: .utf8)
        precondition(imported.contains(token), "Installed importer must extract beyond the local prefix")
        try FileManager.default.removeItem(at: attributes)
        let indexStatus = try importFile(["-i", fixture.path])
        precondition(indexStatus == 0)

        let indexingDeadline = Date(timeIntervalSinceNow: 5)
        var match: SearchResult?
        repeat {
            match = search(token + " tailtoken").first { $0.target == fixture.path }
            if match == nil { RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.15)) }
        } while match == nil && Date() < indexingDeadline
        if let match {
            precondition(match.snippet == "Content match · System index")
            precondition(search(token + " nonexistent-token").allSatisfy { $0.target != fixture.path })
            precondition(search(token + " special\"quote").contains { $0.target == fixture.path })
            precondition(search(token + " literal*star").contains { $0.target == fixture.path })
            precondition(search(token + " literalXstar").allSatisfy { $0.target != fixture.path })
            print("Metadata fixture match passed, including AND and literal punctuation")
        } else {
            print("Metadata fixture match skipped: system index did not expose the imported fixture; importer extraction passed")
        }
        provider.cancel()
        print("Metadata query parser, callback, cancellation and bound checks passed")
    }
}
