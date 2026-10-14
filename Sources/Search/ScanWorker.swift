import AppKit
import CoreServices
import Darwin

struct ScanPolicy: Codable {
    var excludedPaths: [String] = []
    static let skippedNames: Set<String> = ["node_modules", "vendor", "__pycache__", ".git", ".build", "DerivedData", "Caches", "Cache", "tmp", "temp", "build", "dist", "site-packages", "venv", "Pods", "Carthage"]

    func excludes(_ path: String) -> Bool {
        if excludedPaths.contains(where: { path == $0 || path.hasPrefix($0 + "/") }) { return true }
        return path.split(separator: "/").contains { ScanPolicy.skippedNames.contains(String($0)) || $0.hasPrefix(".") }
    }
}

struct ScanRequest: Codable {
    enum Mode: String, Codable { case directory, item, content, dictionary }
    var mode: Mode
    var path: String
    var policy = ScanPolicy()
    var applicationsOnly = false
    var delayForTesting: Double = 0
}

struct ScanEntry: Codable {
    var path: String
    var name = ""
    var aliases = ""
    var kind: Int32 = 0
    var mtime: Double = 0
    var size: Int64 = 0
    var descend = false
    var regular = false
    var removed = false
    var text: String? = nil
}

private struct ScanResponse: Codable {
    var event: String
    var path: String? = nil
    var entry: ScanEntry? = nil
    var text: String? = nil
}

struct ScanResult {
    enum Outcome { case completed, unavailable, timedOut, cancelled }
    var outcome: Outcome
    var blockedPath: String? = nil
    var text: String? = nil
}

// Filesystem and native document/dictionary reads stay in this killable child process.
enum ScanWorker {
    static func runIfRequested() -> Bool {
        guard CommandLine.arguments.contains("--scan-worker") else { return false }
        let parent = getppid()
        guard parent > 1 else { return true }
        let monitor = DispatchSource.makeProcessSource(identifier: parent, eventMask: .exit, queue: .global(qos: .utility))
        monitor.setEventHandler { _exit(0) }
        monitor.resume()
        defer { monitor.cancel() }
        guard getppid() == parent else { return true }
        signal(SIGPIPE, SIG_IGN)
        var pending = Data(), buffer = [UInt8](repeating: 0, count: 65536)
        while true {
            let count = Darwin.read(STDIN_FILENO, &buffer, buffer.count)
            guard count > 0 else { break }
            pending.append(contentsOf: buffer.prefix(count))
            guard pending.count <= 524288 else { break }
            while let end = pending.firstIndex(of: 10) {
                let line = pending.prefix(upTo: end)
                pending.removeSubrange(...end)
                guard let request = try? JSONDecoder().decode(ScanRequest.self, from: line) else { return true }
                autoreleasepool { run(request) }
            }
        }
        return true
    }

    private static func emit(_ response: ScanResponse) {
        guard var data = try? JSONEncoder().encode(response) else { return }
        data.append(10)
        try? FileHandle.standardOutput.write(contentsOf: data)
    }

    private static func run(_ original: ScanRequest) {
        var request = original
        emit(ScanResponse(event: "progress", path: request.path))
        if request.delayForTesting > 0 { Thread.sleep(forTimeInterval: request.delayForTesting) }
        if request.mode == .dictionary {
            let length = (request.path as NSString).length
            let text = DCSCopyTextDefinition(nil, request.path as CFString, CFRange(location: 0, length: length))?.takeRetainedValue() as String?
            emit(ScanResponse(event: "done", text: text.map { String($0.prefix(8192)) }))
            return
        }
        // FSEvents may spell the same APFS path with /private; normalize inside the killable worker.
        let url = URL(fileURLWithPath: request.path)
        request.path = url.deletingLastPathComponent().resolvingSymlinksInPath().appendingPathComponent(url.lastPathComponent).path
        guard !request.policy.excludes(request.path) else { emit(ScanResponse(event: "done")); return }
        if request.mode == .directory {
            guard let directory = opendir(request.path) else {
                emit(ScanResponse(event: "error", path: request.path)); return
            }
            defer { closedir(directory) }
            while let raw = readdir(directory) {
                let name = withUnsafePointer(to: &raw.pointee.d_name) {
                    $0.withMemoryRebound(to: CChar.self, capacity: Int(MAXNAMLEN) + 1) { String(cString: $0) }
                }
                guard name != ".", name != ".." else { continue }
                let path = request.path + "/" + name
                guard !request.policy.excludes(path) else { continue }
                autoreleasepool { inspect(path, content: false, applicationsOnly: request.applicationsOnly) }
            }
        } else { inspect(request.path, content: request.mode == .content, applicationsOnly: request.applicationsOnly) }
        emit(ScanResponse(event: "done"))
    }

    private static func inspect(_ path: String, content: Bool, applicationsOnly: Bool) {
        emit(ScanResponse(event: "progress", path: path))
        var attributes = stat()
        guard lstat(path, &attributes) == 0 else {
            if errno == ENOENT || errno == ENOTDIR { emit(ScanResponse(event: "entry", entry: ScanEntry(path: path, removed: true))) }
            return
        }
        let url = URL(fileURLWithPath: path)
        let type = attributes.st_mode & mode_t(S_IFMT)
        let app = url.pathExtension.lowercased() == "app"
        guard app || type != mode_t(S_IFLNK) else { return }
        let directory = type == mode_t(S_IFDIR)
        guard !applicationsOnly || app || directory else { return }
        let package = directory && ((try? url.resourceValues(forKeys: [.isPackageKey]))?.isPackage == true)
        var entry = ScanEntry(path: path, name: url.lastPathComponent, kind: app ? 2 : directory && !package ? 1 : 0,
                              mtime: Double(attributes.st_mtimespec.tv_sec) + Double(attributes.st_mtimespec.tv_nsec) / 1_000_000_000,
                              size: Int64(attributes.st_size), descend: directory && !package && !app, regular: type == mode_t(S_IFREG))
        if content {
            guard entry.regular, ContentExtractor.supports(url, size: Int(entry.size)) else { return }
            entry.text = ContentExtractor.extract(url)
            var after = stat()
            if lstat(path, &after) != 0 || after.st_size != attributes.st_size ||
               after.st_mtimespec.tv_sec != attributes.st_mtimespec.tv_sec ||
               after.st_mtimespec.tv_nsec != attributes.st_mtimespec.tv_nsec {
                entry.text = nil
            }
        } else if app {
            let names = applicationNames(url)
            entry.name = names.title
            entry.aliases = names.aliases
        } else { entry.aliases = SearchText.aliases(entry.name) + " " + SearchText.normalized(url.deletingPathExtension().lastPathComponent) }
        emit(ScanResponse(event: "entry", entry: entry))
    }

    private static func applicationNames(_ url: URL) -> (title: String, aliases: String) {
        let basename = url.deletingPathExtension().lastPathComponent
        guard let bundle = Bundle(url: url) else { return (basename, SearchText.normalized(basename)) }
        let keys = ["CFBundleDisplayName", "CFBundleName"]
        func names(_ dictionary: [String: Any]?) -> [String] { keys.compactMap { dictionary?[$0] as? String }.filter { !$0.isEmpty } }
        let current = names(bundle.localizedInfoDictionary) + names(bundle.infoDictionary)
        let preferred = Bundle.preferredLocalizations(from: bundle.localizations, forPreferences: Locale.preferredLanguages)
        var preferredNames: [String] = []
        var aliases = Set(current + [basename])
        let languages = ["en", "English", "zh_CN", "zh_TW", "zh_HK", "zh-Hans", "zh-Hant"]
        if let tableURL = bundle.url(forResource: "InfoPlist", withExtension: "loctable"),
           let data = try? Data(contentsOf: tableURL),
           let table = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] {
            for language in languages { aliases.formUnion(names(table[language] as? [String: Any])) }
            preferredNames = preferred.flatMap { names(table[$0] as? [String: Any]) }
        }
        for language in languages {
            if let file = bundle.url(forResource: "InfoPlist", withExtension: "strings", subdirectory: nil, localization: language),
               let data = try? Data(contentsOf: file),
               let dictionary = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] { aliases.formUnion(names(dictionary)) }
        }
        for language in preferred where preferredNames.isEmpty {
            if let file = bundle.url(forResource: "InfoPlist", withExtension: "strings", subdirectory: nil, localization: language),
               let data = try? Data(contentsOf: file),
               let dictionary = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] { preferredNames = names(dictionary) }
        }
        aliases.formUnion(preferredNames)
        return (preferredNames.first ?? current.first ?? basename, aliases.sorted().map { SearchText.normalized($0) + " " + SearchText.aliases($0) }.joined(separator: " "))
    }
}

// The IO queue consumes bounded batches synchronously, so pipe backpressure bounds memory.
final class ScanClient {
    private let queue = DispatchQueue(label: "argus.scan.io", qos: .utility)
    private let lock = NSLock()
    private let idleTimeout: Double
    private var generation = 0
    private var process: Process?
    private var input: FileHandle?
    private var output: FileHandle?
    private var lastActivity: Double = 0
    private var timedOut = false
    private var stopped = false

    init(idleTimeout: Double = 4) { self.idleTimeout = idleTimeout }

    func perform(_ request: ScanRequest, onBatch: (([ScanEntry]) -> Void)? = nil, completion: @escaping (ScanResult) -> Void) {
        lock.lock(); let token = generation; lock.unlock()
        queue.async { [self] in completion(read(request, token: token, onBatch: onBatch)) }
    }

    func cancel() {
        interrupt(permanently: false)
    }

    func stop() {
        interrupt(permanently: true)
        close()
    }

    private func interrupt(permanently: Bool) {
        lock.lock(); generation += 1
        if permanently { stopped = true }
        if let process, process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
        lock.unlock()
    }

    func close() { queue.async { [self] in discard() } }

    private func discard() {
        lock.lock()
        if let process, process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
        process = nil
        lock.unlock()
        try? input?.close(); try? output?.close()
        input = nil; output = nil
    }

    private func read(_ request: ScanRequest, token: Int, onBatch: (([ScanEntry]) -> Void)?) -> ScanResult {
        lock.lock(); let cancelled = stopped || token != generation; lock.unlock()
        guard !cancelled else { return ScanResult(outcome: .cancelled) }
        if process == nil || process?.isRunning == false {
            discard()
            let child = Process(), stdin = Pipe(), stdout = Pipe()
            child.executableURL = Bundle.main.executableURL
            child.arguments = ["--scan-worker"]
            child.standardInput = stdin; child.standardOutput = stdout; child.standardError = FileHandle.nullDevice
            child.qualityOfService = .utility
            do { try child.run() } catch { return ScanResult(outcome: .unavailable) }
            input = stdin.fileHandleForWriting; output = stdout.fileHandleForReading
            _ = fcntl(stdin.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1)
            lock.lock(); process = child
            if stopped || token != generation { Darwin.kill(child.processIdentifier, SIGKILL) }
            lock.unlock()
        }
        guard let input, let output, var data = try? JSONEncoder().encode(request) else { return ScanResult(outcome: .unavailable) }
        lock.lock(); lastActivity = ProcessInfo.processInfo.systemUptime; timedOut = false; lock.unlock()
        let watchdog = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        watchdog.schedule(deadline: .now() + idleTimeout, repeating: min(idleTimeout, 0.25))
        watchdog.setEventHandler { [weak self] in
            guard let self else { return }
            self.lock.lock()
            if token == self.generation, ProcessInfo.processInfo.systemUptime - self.lastActivity >= self.idleTimeout,
               let child = self.process, child.isRunning { self.timedOut = true; Darwin.kill(child.processIdentifier, SIGKILL) }
            self.lock.unlock()
        }
        watchdog.resume()
        defer { watchdog.cancel() }
        data.append(10)
        do { try input.write(contentsOf: data) } catch { discard(); return ScanResult(outcome: .unavailable) }
        var pending = Data(), buffer = [UInt8](repeating: 0, count: 65536), batch: [ScanEntry] = []
        var path = request.path
        while true {
            let count = Darwin.read(output.fileDescriptor, &buffer, buffer.count)
            guard count > 0 else { break }
            lock.lock(); lastActivity = ProcessInfo.processInfo.systemUptime; lock.unlock()
            pending.append(contentsOf: buffer.prefix(count))
            guard pending.count <= 524288 else { break }
            while let end = pending.firstIndex(of: 10) {
                let line = pending.prefix(upTo: end)
                pending.removeSubrange(...end)
                guard let response = try? JSONDecoder().decode(ScanResponse.self, from: line) else { discard(); return ScanResult(outcome: .unavailable, blockedPath: path) }
                if response.event == "progress" { path = response.path ?? path }
                if let entry = response.entry {
                    batch.append(entry)
                    if batch.count == 64 { onBatch?(batch); batch.removeAll(keepingCapacity: true) }
                }
                if response.event == "done" || response.event == "error" {
                    if !batch.isEmpty { onBatch?(batch) }
                    return ScanResult(outcome: response.event == "done" ? .completed : .unavailable, blockedPath: response.path, text: response.text)
                }
            }
        }
        if !batch.isEmpty { onBatch?(batch) }
        lock.lock(); let wasCancelled = token != generation, wasTimedOut = timedOut; lock.unlock()
        discard()
        return ScanResult(outcome: wasCancelled ? .cancelled : wasTimedOut ? .timedOut : .unavailable, blockedPath: path)
    }

    deinit {
        if let process, process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
        try? input?.close(); try? output?.close()
    }
}
