import AppKit
import Darwin

/// Owns the serial reader and the lifetime of progressive query snapshots.
final class IndexSearch {
    private let readerQueue = DispatchQueue(label: "argus.index.reader", qos: .userInitiated)
    private let queryLock = NSLock()
    private var queryGeneration = 0
    private var stopped = false
    private let reader: OpaquePointer?
    private let home: URL

    init(database: String, home: URL) {
        reader = argus_index_open(database)
        self.home = home
    }

    func stop() {
        queryLock.lock(); queryGeneration += 1; stopped = true; queryLock.unlock()
        if let reader { argus_index_interrupt(reader) }
    }

    func prepare(after delay: Double = 0.25) {
        readerQueue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, let reader = self.reader else { return }
            self.queryLock.lock(); let active = !self.stopped; self.queryLock.unlock()
            guard active else { return }
            if argus_index_prepare_snapshot(reader) < 0 { self.prepare(after: 0.5) }
        }
    }

    func search(_ query: String, scope: SearchScope, exclusions: ScanPolicy, completion: @escaping ([SearchResult], Double) -> Void) {
        queryLock.lock(); queryGeneration += 1; let generation = queryGeneration; queryLock.unlock()
        if let reader { argus_index_interrupt(reader) }
        readerQueue.async { [weak self] in
            guard let self, let reader = self.reader else { DispatchQueue.main.async { completion([], 0) }; return }
            self.queryLock.lock(); let current = self.queryGeneration == generation; self.queryLock.unlock()
            guard current else { return }
            let start = ProcessInfo.processInfo.systemUptime
            let normalized = SearchText.normalized(query)
            let filter: Int32 = scope == .applications ? 2 : scope == .files ? 0 : -1
            if scope == .actions || scope == .people { DispatchQueue.main.async { completion([], 0) }; return }
            guard let search = argus_search_open(reader, normalized, filter, 48) else {
                DispatchQueue.main.async { if self.isCurrent(generation) { completion([], 0) } }
                return
            }
            defer { argus_search_close(search) }
            let metadataCount = argus_search_metadata(search)
            let metadata = self.results(from: search, count: metadataCount, normalized: normalized, exclusions: exclusions)
            let metadataTime = (ProcessInfo.processInfo.systemUptime - start) * 1000
            let finished = metadataCount < 0 || argus_search_finished(search) != 0
            if !metadata.isEmpty || finished {
                DispatchQueue.main.async {
                    guard self.isCurrent(generation) else { return }
                    completion(metadata, metadataTime)
                }
            }
            guard !finished, self.isCurrent(generation) else { return }
            let fullCount = argus_search_complete(search)
            let results = self.results(from: search, count: fullCount, normalized: normalized, exclusions: exclusions)
            let milliseconds = (ProcessInfo.processInfo.systemUptime - start) * 1000
            DispatchQueue.main.async {
                guard self.isCurrent(generation) else { return }
                completion(results, milliseconds)
            }
        }
    }

    private func isCurrent(_ generation: Int) -> Bool {
        queryLock.lock()
        defer { queryLock.unlock() }
        return queryGeneration == generation
    }

    private func results(from search: OpaquePointer, count: Int32, normalized: String, exclusions: ScanPolicy) -> [SearchResult] {
        var results: [SearchResult] = []
        results.reserveCapacity(max(0, Int(count)))
        for index in 0..<max(0, Int(count)) {
            var hit = ArgusResult()
            guard argus_search_read(search, Int32(index), &hit) != 0,
                  let pathBytes = hit.path, let nameBytes = hit.name, let snippetBytes = hit.snippet else { break }
            let path = String(cString: pathBytes)
            let name = String(cString: nameBytes)
            let snippet = String(cString: snippetBytes)
            guard !exclusions.excludes(path), access(path, F_OK) == 0 else { continue }
            let kind: SearchKind = hit.kind == 2 ? .application : hit.kind == 1 ? .folder : hit.content_match != 0 ? .content : .file
            let displayPath = path.hasPrefix(home.path + "/") ? "~" + path.dropFirst(home.path.count) : path
            var score = hit.score
            if kind == .application && SearchText.normalized(((path as NSString).lastPathComponent as NSString).deletingPathExtension) == normalized {
                score = max(score, 1000)
            }
            results.append(SearchResult(id: "file:" + path, title: name, subtitle: displayPath, kind: kind, target: path, score: score + (kind == .application ? 35 : 0), snippet: snippet))
        }
        return results
    }

    deinit { if let reader { argus_index_close(reader) } }
}
