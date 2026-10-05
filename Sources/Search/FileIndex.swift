import AppKit
import CoreServices
import Darwin

final class FileIndex {
    static var storageURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Argus", isDirectory: true)
    }
    private let writerQueue = DispatchQueue(label: "argus.index.writer", qos: .utility)
    private let readerQueue = DispatchQueue(label: "argus.index.reader", qos: .userInitiated)
    private let queryLock = NSLock()
    private var queryGeneration = 0
    private var writer: OpaquePointer?
    private var reader: OpaquePointer?
    private var watcher: FileWatcher?
    private var scanning = false
    private var fullScan = false
    private var rescanRequested = false
    private var requestInFlight = false
    private let scanClient = ScanClient()
    private struct Job {
        var mode: ScanRequest.Mode
        var path: String
        var applicationsOnly = false
    }
    private var jobs: [Job] = []
    private var contentJobs: [Job] = []
    private var jobOffset = 0
    private var contentOffset = 0
    private var queuedPaths: Set<String> = []
    private var deferredPaths: Set<String> = []
    private var retries: [String: Int] = [:]
    private var retryCount = 0
    private var lastReport: Double = 0
    private var failure: String?
    private var rejectedDirectories = 0
    private var contentBytes: Int64 = 0
    private let contentLimit: Int64 = 128 * 1024 * 1024
    private let home: URL
    private let homePaths: Set<String>
    private let storage: URL
    private let applicationRoots: [URL]
    var onStatus: ((String, Int) -> Void)?
    private(set) var status = "Preparing index…"
    private(set) var count = 0

    init(home: URL = FileManager.default.homeDirectoryForCurrentUser, storage: URL = FileIndex.storageURL, applications: [URL]? = nil) {
        self.home = home.resolvingSymlinksInPath()
        self.homePaths = Set([home.path, self.home.path])
        self.storage = storage.resolvingSymlinksInPath()
        self.applicationRoots = (applications ?? [URL(fileURLWithPath: "/Applications"), URL(fileURLWithPath: "/System/Applications"), URL(fileURLWithPath: "/System/Library/CoreServices/Applications"), URL(fileURLWithPath: "/System/Library/CoreServices/Finder.app"), home.appendingPathComponent("Applications")]).map { $0.resolvingSymlinksInPath() }
        try? FileManager.default.createDirectory(at: storage, withIntermediateDirectories: true)
        try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: storage.path)
        let path = storage.appendingPathComponent("index.sqlite").path
        writer = argus_index_open(path)
        reader = argus_index_open(path)
    }

    func start() {
        writerQueue.async { [weak self] in
            guard let self, let writer = self.writer else { self?.report("Could not open index. Check disk permissions.", 0); return }
            self.watcher = FileWatcher(paths: [self.home.path] + self.applicationRoots.map(\.path), queue: self.writerQueue) { [weak self] path, flags in self?.changed(path, flags: flags) }
            self.contentBytes = argus_index_content_bytes(writer)
            self.scanAll()
        }
    }

    func rebuild() {
        writerQueue.async { [weak self] in self?.scanAll() }
    }

    func stop() {
        scanClient.stop()
        queryLock.lock(); queryGeneration += 1; queryLock.unlock()
        if let reader { argus_index_interrupt(reader) }
        writerQueue.async { [weak self] in self?.watcher = nil }
    }

    func contentPreferenceChanged() {
        writerQueue.async { [weak self] in
            guard let self, let writer = self.writer else { return }
            if Preferences.shared.contentIndexing { self.scanAll() }
            else { argus_index_clear_content(writer); self.contentBytes = 0; self.report("Content indexing is off", Int(argus_index_count(writer))) }
        }
    }

    func exclusionsChanged() {
        writerQueue.async { [weak self] in
            guard let self, let writer = self.writer else { return }
            for path in Preferences.shared.excludedPaths { argus_index_remove_tree(writer, path) }
            self.contentBytes = argus_index_content_bytes(writer)
            self.scanAll()
        }
    }

    func search(_ query: String, scope: SearchScope, completion: @escaping ([SearchResult], Double) -> Void) {
        queryLock.lock(); queryGeneration += 1; let generation = queryGeneration; queryLock.unlock()
        if let reader { argus_index_interrupt(reader) }
        readerQueue.async { [weak self] in
            guard let self, let reader = self.reader else { DispatchQueue.main.async { completion([], 0) }; return }
            self.queryLock.lock(); let current = self.queryGeneration == generation; self.queryLock.unlock()
            guard current else { return }
            let start = ProcessInfo.processInfo.systemUptime
            let normalized = SearchText.normalized(query)
            let exclusions = self.policy
            let filter: Int32 = scope == .applications ? 2 : scope == .files ? 0 : -1
            if scope == .actions || scope == .people { DispatchQueue.main.async { completion([], 0) }; return }
            let hits = UnsafeMutablePointer<ArgusHit>.allocate(capacity: 48)
            defer { hits.deallocate() }
            let metadataCount = argus_index_search_metadata(reader, normalized, filter, 48, hits)
            let metadata = self.results(from: hits, count: metadataCount, normalized: normalized, exclusions: exclusions)
            let metadataTime = (ProcessInfo.processInfo.systemUptime - start) * 1000
            let finished = metadataCount < 0 || (metadataCount == 48 && hits[47].score > 300)
            if !metadata.isEmpty || finished {
                DispatchQueue.main.async {
                    guard self.isCurrent(generation) else { return }
                    completion(metadata, metadataTime)
                }
            }
            guard !finished, self.isCurrent(generation) else { return }
            let fullCount = argus_index_search(reader, normalized, filter, 48, hits)
            let results = self.results(from: hits, count: fullCount, normalized: normalized, exclusions: exclusions)
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

    private func results(from hits: UnsafeMutablePointer<ArgusHit>, count: Int32, normalized: String, exclusions: ScanPolicy) -> [SearchResult] {
        var results: [SearchResult] = []
        for index in 0..<max(0, Int(count)) {
            var hit = hits[index]
            let path = withUnsafePointer(to: &hit.path) { $0.withMemoryRebound(to: CChar.self, capacity: 4096) { String(cString: $0) } }
            let name = withUnsafePointer(to: &hit.name) { $0.withMemoryRebound(to: CChar.self, capacity: 512) { String(cString: $0) } }
            let snippet = withUnsafePointer(to: &hit.snippet) { $0.withMemoryRebound(to: CChar.self, capacity: 512) { String(cString: $0) } }
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

    private func report(_ text: String, _ total: Int) {
        DispatchQueue.main.async { [weak self] in self?.status = text; self?.count = total; self?.onStatus?(text, total) }
    }

    private var policy: ScanPolicy {
        let privatePaths = homePaths.flatMap { [$0 + "/Library", $0 + "/go/pkg", $0 + "/Music/Music"] }
        return ScanPolicy(excludedPaths: privatePaths + [storage.path] + Preferences.shared.excludedPaths)
    }

    private var scanPolicy: ScanPolicy {
        var result = policy
        result.excludedPaths += deferredPaths
        return result
    }

    func excluded(_ url: URL) -> Bool { policy.excludes(url.path) }

    private func scanAll() {
        guard let writer else { return }
        if scanning { rescanRequested = true; return }
        scanning = true
        fullScan = true
        rejectedDirectories = 0
        failure = nil
        deferredPaths.removeAll()
        retries.removeAll()
        retryCount = 0
        check(argus_index_prune_excluded(writer, { path, context in
            guard let path, let context else { return 0 }
            let index = Unmanaged<FileIndex>.fromOpaque(context).takeUnretainedValue()
            return index.excluded(URL(fileURLWithPath: String(cString: path))) ? 1 : 0
        }, Unmanaged.passUnretained(self).toOpaque()))
        report("Indexing apps and personal files…", Int(argus_index_count(writer)))
        for root in applicationRoots {
            enqueue(Job(mode: root.pathExtension.lowercased() == "app" ? .item : .directory, path: root.path, applicationsOnly: true))
        }
        enqueue(Job(mode: .directory, path: home.path))
        nextJob()
    }

    private func enqueue(_ job: Job) {
        guard !scanPolicy.excludes(job.path) else { return }
        let key = job.mode.rawValue + ":" + job.path
        guard queuedPaths.insert(key).inserted else { return }
        if job.mode == .content { contentJobs.append(job) }
        else { jobs.append(job) }
    }

    private func nextJob() {
        guard !requestInFlight, let writer else { return }
        if failure != nil { finishScan(); return }
        var candidate: Job?
        while candidate == nil {
            let next: Job
            if jobOffset < jobs.count { next = jobs[jobOffset]; jobOffset += 1 }
            else if Preferences.shared.contentIndexing, contentOffset < contentJobs.count, contentBytes + Int64(ContentExtractor.byteLimit) <= contentLimit {
                next = contentJobs[contentOffset]; contentOffset += 1
            } else { break }
            queuedPaths.remove(next.mode.rawValue + ":" + next.path)
            if !scanPolicy.excludes(next.path) { candidate = next }
        }
        guard let job = candidate else { finishScan(); return }
        if jobOffset >= 256 { jobs.removeFirst(jobOffset); jobOffset = 0 }
        if contentOffset >= 256 { contentJobs.removeFirst(contentOffset); contentOffset = 0 }
        requestInFlight = true
        var request = ScanRequest(mode: job.mode, path: job.path)
        request.policy = scanPolicy
        request.applicationsOnly = job.applicationsOnly
        if job.mode == .content, ProcessInfo.processInfo.systemUptime - lastReport > 0.2 {
            lastReport = ProcessInfo.processInfo.systemUptime
            report("Filenames ready. Indexing contents…", Int(argus_index_count(writer)))
        }
        scanClient.perform(request, onBatch: { [weak self] entries in
            guard let self else { return }
            self.writerQueue.sync { self.apply(entries, job: job) }
        }) { [weak self] result in
            self?.writerQueue.async { [weak self] in
                guard let self else { return }
                self.requestInFlight = false
                if result.outcome == .timedOut || result.outcome == .unavailable {
                    self.rejectedDirectories += 1
                    let blocked = result.blockedPath ?? job.path
                    self.deferredPaths.insert(blocked)
                    // A stuck child attribute read must not hide its ordinary siblings.
                    if job.mode == .directory, blocked != job.path, self.retryCount < 32, self.retries[job.path, default: 0] < 4 {
                        self.retryCount += 1
                        self.retries[job.path, default: 0] += 1
                        self.enqueue(job)
                    }
                }
                self.nextJob()
            }
        }
    }

    private func apply(_ entries: [ScanEntry], job: Job) {
        guard let writer, failure == nil, check(argus_index_begin(writer)) else { return }
        for entry in entries where !scanPolicy.excludes(entry.path) {
            if entry.removed { check(argus_index_remove_tree(writer, entry.path)); continue }
            if job.mode == .content {
                if Preferences.shared.contentIndexing, let text = entry.text {
                    check(argus_index_set_content(writer, entry.path, text))
                }
                continue
            }
            if entry.descend { enqueue(Job(mode: .directory, path: entry.path, applicationsOnly: job.applicationsOnly)) }
            if job.applicationsOnly && entry.kind != 2 { continue }
            // App aliases can change with the preferred language even when bundle mtime is unchanged.
            if entry.kind == 2 || argus_index_needs_update(writer, entry.path, entry.mtime, entry.size) != 0 {
                check(argus_index_upsert(writer, entry.path, entry.name, entry.aliases, entry.kind, entry.mtime, entry.size))
            }
            if !job.applicationsOnly, entry.regular, Preferences.shared.contentIndexing,
               ContentExtractor.supports(URL(fileURLWithPath: entry.path), size: Int(entry.size)),
               argus_index_content_needed(writer, entry.path, entry.mtime) != 0 {
                enqueue(Job(mode: .content, path: entry.path))
            }
        }
        if failure == nil { check(argus_index_commit(writer)) }
        else { argus_index_rollback(writer) }
        contentBytes = argus_index_content_bytes(writer)
        if job.mode != .content, ProcessInfo.processInfo.systemUptime - lastReport > 0.2 {
            lastReport = ProcessInfo.processInfo.systemUptime
            report("Indexing filenames…", Int(argus_index_count(writer)))
        }
    }

    private func finishScan() {
        guard let writer else { return }
        jobs.removeAll(); contentJobs.removeAll()
        jobOffset = 0; contentOffset = 0; queuedPaths.removeAll()
        deferredPaths.removeAll(); retries.removeAll()
        scanClient.close()
        if fullScan, failure == nil { check(argus_index_prune_missing(writer)) }
        fullScan = false
        scanning = false
        let message = failure.map { "Index update failed: " + $0 } ?? (rejectedDirectories > 0 ? "Index ready · Some locations are currently unreadable" : "Index ready")
        report(message, Int(argus_index_count(writer)))
        if rescanRequested { rescanRequested = false; scanAll() }
    }

    @discardableResult private func check(_ result: Int32) -> Bool {
        guard result == 0, let writer else { return true }
        failure = String(cString: argus_index_error(writer))
        return false
    }

    private func changed(_ path: String, flags: FSEventStreamEventFlags) {
        var url = URL(fileURLWithPath: path)
        if let index = url.pathComponents.firstIndex(where: { $0.lowercased().hasSuffix(".app") }) {
            url = URL(fileURLWithPath: NSString.path(withComponents: Array(url.pathComponents.prefix(index + 1))))
        }
        guard !excluded(url), writer != nil else { return }
        if !scanning {
            scanning = true
            rejectedDirectories = 0
            failure = nil
            retries.removeAll(); retryCount = 0
        }
        let dropped = flags & FSEventStreamEventFlags(kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped) != 0
        enqueue(Job(mode: dropped && url.pathExtension.lowercased() != "app" ? .directory : .item, path: url.path))
        nextJob()
    }

    deinit {
        watcher = nil
        scanClient.cancel()
        scanClient.close()
        if let reader { argus_index_close(reader) }
        if let writer { argus_index_close(writer) }
    }
}
