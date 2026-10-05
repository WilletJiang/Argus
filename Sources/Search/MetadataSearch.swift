import Foundation
import CoreServices

final class MetadataSearch {
    private final class Session {
        let query: MDQuery
        let version: Int
        let completion: ([SearchResult]) -> Void
        weak var owner: MetadataSearch?
        var timeout: DispatchWorkItem?

        init(query: MDQuery, version: Int, completion: @escaping ([SearchResult]) -> Void, owner: MetadataSearch) {
            self.query = query
            self.version = version
            self.completion = completion
            self.owner = owner
        }

        func stop() {
            timeout?.cancel()
            timeout = nil
            CFNotificationCenterRemoveObserver(CFNotificationCenterGetLocalCenter(),
                Unmanaged.passUnretained(self).toOpaque(), nil, nil)
            MDQueryStop(query)
        }
    }

    private let queue = DispatchQueue(label: "Argus.metadata-search", qos: .userInitiated)
    private let lock = NSLock()
    private var generation = 0
    private var active: Session?
    private let home = FileManager.default.homeDirectoryForCurrentUser.standardizedFileURL

    deinit {
        let session = active
        queue.async { session?.stop() }
    }

    func search(_ query: String, completion: @escaping ([SearchResult]) -> Void) {
        let version = advanceGeneration()
        queue.async { [weak self] in
            guard let self, self.isCurrent(version) else { return }
            self.active?.stop()
            self.active = nil
            guard let expression = Self.expression(for: query),
                  let query = MDQueryCreate(nil, expression as CFString, nil, nil) else {
                self.deliver([], version: version, completion: completion)
                return
            }
            MDQuerySetMaxCount(query, 64)
            MDQuerySetSearchScope(query, [self.home.path] as CFArray, 0)
            MDQuerySetDispatchQueue(query, self.queue)
            let session = Session(query: query, version: version, completion: completion, owner: self)
            self.active = session
            CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(),
                Unmanaged.passUnretained(session).toOpaque(), { _, observer, _, _, _ in
                    guard let observer else { return }
                    let session = Unmanaged<Session>.fromOpaque(observer).takeUnretainedValue()
                    session.owner?.finish(session)
                }, kMDQueryDidFinishNotification, Unmanaged.passUnretained(query).toOpaque(), .deliverImmediately)
            let timeout = DispatchWorkItem { [weak self, weak session] in
                guard let session else { return }
                self?.finish(session)
            }
            session.timeout = timeout
            self.queue.asyncAfter(deadline: .now() + 3, execute: timeout)
            if !MDQueryExecute(query, 0) { self.finish(session) }
        }
    }

    func cancel() {
        _ = advanceGeneration()
        queue.async { [weak self] in
            self?.active?.stop()
            self?.active = nil
        }
    }

    // Metadata uses glob strings. Escape user text before surrounding each token with contains wildcards.
    static func expression(for query: String) -> String? {
        let normalized = SearchText.normalized(query).trimmingCharacters(in: .whitespacesAndNewlines)
        let tokens = normalized.split(whereSeparator: \.isWhitespace)
        guard !tokens.isEmpty, tokens.count <= 64, normalized.utf8.count <= 2048,
              !normalized.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) && !CharacterSet.whitespacesAndNewlines.contains($0) }) else { return nil }
        let comparisons = tokens.map { token in
            let escaped = token.reduce(into: "") { result, character in
                if ["\\", "\"", "'", "*", "?"].contains(character) { result.append("\\") }
                result.append(character)
            }
            return "kMDItemTextContent == \"*\(escaped)*\"cd"
        }
        return comparisons.joined(separator: " && ") +
            " && kMDItemContentTypeTree != \"public.email-message\"" +
            " && kMDItemContentTypeTree != \"com.apple.application-bundle\"" +
            " && kMDItemContentTypeTree != \"public.folder\""
    }

    private func finish(_ session: Session) {
        guard active === session else { return }
        session.stop()
        active = nil
        guard isCurrent(session.version) else { return }
        MDQueryDisableUpdates(session.query)
        defer { MDQueryEnableUpdates(session.query) }
        var results: [SearchResult] = []
        for index in 0..<min(64, MDQueryGetResultCount(session.query)) {
            guard let pointer = MDQueryGetResultAtIndex(session.query, index) else { continue }
            let item = Unmanaged<MDItem>.fromOpaque(pointer).takeUnretainedValue()
            guard let path = MDItemCopyAttribute(item, kMDItemPath) as? String,
                  path.hasPrefix(home.path + "/"), !path.lowercased().contains(".app/") else { continue }
            let url = URL(fileURLWithPath: path)
            results.append(SearchResult(id: "file:" + path, title: url.lastPathComponent, subtitle: path,
                kind: .content, target: path, score: 300, snippet: "Content match · System index"))
        }
        deliver(results, version: session.version, completion: session.completion)
    }

    private func advanceGeneration() -> Int {
        lock.lock()
        defer { lock.unlock() }
        generation += 1
        return generation
    }

    private func isCurrent(_ version: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return generation == version
    }

    private func deliver(_ results: [SearchResult], version: Int, completion: @escaping ([SearchResult]) -> Void) {
        DispatchQueue.main.async { [weak self] in
            guard self?.isCurrent(version) == true else { return }
            completion(results)
        }
    }
}
