import AppKit
import CoreServices
import Darwin

final class FileIndex {
    static var storageURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Argus", isDirectory: true)
    }
    private let writerQueue = DispatchQueue(label: "argus.index.writer", qos: .utility)
    private var writer: OpaquePointer?
    private let searcher: IndexSearch
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
    private var contentCursor: Int64 = 0
    private var snapshotQueued = false
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
        searcher = IndexSearch(database: path, home: self.home)
    }

    func start() {
        writerQueue.async { [weak self] in
            guard let self, let writer = self.writer else { self?.report("Could not open index. Check disk permissions.", 0); return }
            self.watcher = FileWatcher(paths: [self.home.path] + self.applicationRoots.map(\.path), queue: self.writerQueue) { [weak self] path, flags in self?.changed(path, flags: flags) }
            self.report("Updating search index…", Int(argus_index_count(writer)))
            if argus_index_upgrade(writer) == 0 {
                self.report("Using previous index: " + String(cString: argus_index_error(writer)), Int(argus_index_count(writer)))
            }
            self.contentBytes = argus_index_content_bytes(writer)
            self.scanAll()
        }
    }

#if ARGUS_BENCHMARK
    func prepareBenchmarkIndex() {
        writerQueue.async {
            guard let writer = self.writer else { return }
            guard argus_index_upgrade(writer) != 0 else {
                self.report("Benchmark migration failed", 0); return
            }
            _ = argus_index_prepare_snapshot(writer)
            self.report("Index ready · frozen benchmark corpus", Int(argus_index_count(writer)))
        }
    }
#endif

    func rebuild() {
        writerQueue.async { [weak self] in self?.scanAll() }
    }

    func stop() {
        scanClient.stop()
        searcher.stop()
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
            self.report("Updating search index…", Int(argus_index_count(writer)))
            if argus_index_upgrade(writer) == 0 {
                self.report("Using previous index: " + String(cString: argus_index_error(writer)), Int(argus_index_count(writer)))
            }
            self.contentBytes = argus_index_content_bytes(writer)
            self.scanAll()
        }
    }

    func search(_ query: String, scope: SearchScope, completion: @escaping ([SearchResult], Double) -> Void) {
        searcher.search(query, scope: scope, exclusions: policy, completion: completion)
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
        contentCursor = 0; snapshotQueued = false
        check(argus_index_clear_scan_jobs(writer))
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
        guard let writer, !scanPolicy.excludes(job.path) else { return }
        snapshotQueued = false
        check(argus_index_queue_scan(writer, job.path, job.mode == .directory ? 0 : 1, job.applicationsOnly ? 1 : 0))
    }

    private func path(from item: inout ArgusScanItem) -> String {
        withUnsafePointer(to: &item.path) { pointer in
            pointer.withMemoryRebound(to: CChar.self, capacity: 4096) { String(cString: $0) }
        }
    }

    private func nextJob() {
        guard !requestInFlight, let writer else { return }
        if failure != nil { finishScan(); return }
        var candidate: Job?
        while candidate == nil {
            var item = ArgusScanItem()
            let scan = argus_index_next_scan(writer, &item)
            if scan < 0 { check(0); finishScan(); return }
            let next: Job
            if scan > 0 {
                next = Job(mode: item.mode == 0 ? .directory : .item,
                           path: path(from: &item), applicationsOnly: item.applications_only != 0)
            } else {
                if !snapshotQueued { snapshotQueued = true; searcher.prepare() }
                guard Preferences.shared.contentIndexing,
                      contentBytes + Int64(ContentExtractor.byteLimit) <= contentLimit else { break }
                let content = argus_index_next_content(writer, contentCursor, &item)
                if content < 0 { check(0); finishScan(); return }
                guard content > 0 else { break }
                contentCursor = item.id
                let target = path(from: &item)
                guard ContentExtractor.supports(URL(fileURLWithPath: target), size: Int(item.size)) else { continue }
                next = Job(mode: .content, path: target)
            }
            if !scanPolicy.excludes(next.path) { candidate = next }
        }
        guard let job = candidate else { finishScan(); return }
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
                if Preferences.shared.contentIndexing {
                    if let text = entry.text,
                       argus_index_needs_update(writer, entry.path, entry.mtime, entry.size) == 0 {
                        check(argus_index_set_content(writer, entry.path, text))
                    } else { enqueue(Job(mode: .item, path: entry.path)) }
                }
                continue
            }
            if entry.descend { enqueue(Job(mode: .directory, path: entry.path, applicationsOnly: job.applicationsOnly)) }
            if job.applicationsOnly && entry.kind != 2 { continue }
            let eligible = !job.applicationsOnly && entry.regular &&
                ContentExtractor.supports(URL(fileURLWithPath: entry.path), size: Int(entry.size))
            let contentNeeded = eligible && Preferences.shared.contentIndexing &&
                argus_index_content_needed(writer, entry.path, entry.mtime) != 0
            // App aliases follow the preferred language even when bundle mtime is unchanged.
            if entry.kind == 2 || contentNeeded || argus_index_needs_update(writer, entry.path, entry.mtime, entry.size) != 0 {
                check(argus_index_upsert_scanned(writer, entry.path, entry.name, entry.aliases,
                                                entry.kind, entry.mtime, entry.size, eligible ? 1 : 0))
            }
            if contentNeeded { contentCursor = 0 }
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
        check(argus_index_clear_scan_jobs(writer))
        contentCursor = 0
        deferredPaths.removeAll(); retries.removeAll()
        scanClient.close()
        if fullScan, failure == nil { check(argus_index_prune_missing(writer)) }
        fullScan = false
        scanning = false
        let message = failure.map { "Index update failed: " + $0 } ?? (rejectedDirectories > 0 ? "Index ready · Some locations are currently unreadable" : "Index ready")
        report(message, Int(argus_index_count(writer)))
        if failure == nil { searcher.prepare() }
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
            contentCursor = 0; snapshotQueued = false
        }
        let dropped = flags & FSEventStreamEventFlags(kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped) != 0
        enqueue(Job(mode: dropped && url.pathExtension.lowercased() != "app" ? .directory : .item, path: url.path))
        nextJob()
    }

    deinit {
        watcher = nil
        scanClient.cancel()
        scanClient.close()
        if let writer { argus_index_close(writer) }
    }
}
