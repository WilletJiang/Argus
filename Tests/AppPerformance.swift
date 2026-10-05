import AppKit
import Darwin

// Compiled only by scripts/build.sh --benchmark, never into the delivered app.
final class AppPerformance {
    private let panel: PanelController
    private let index: FileIndex
    private let output: URL
    private let queries = ["Safari", "Terminal", "Xcode", "Finder", "备忘录", "README", "swift", "Argus"]
    private var position = 0
    private var waiting = false
    private var collecting = false
    private var typing = false
    private var token = 0
    private var samples: [String: [Double]] = [:]
    private var failures: [String] = []
    private var memoryTimer: DispatchSourceTimer?
    private var peakCombinedFootprint: UInt64 = 0
    private let deadline = Date(timeIntervalSinceNow: 180)

    init(panel: PanelController, index: FileIndex, output: String) {
        self.panel = panel; self.index = index; self.output = URL(fileURLWithPath: output)
    }

    func start() {
        if memoryTimer == nil {
            let timer = DispatchSource.makeTimerSource(queue: .main)
            timer.schedule(deadline: .now(), repeating: 0.05)
            timer.setEventHandler { [weak self] in self?.sampleCombinedMemory() }
            timer.resume()
            memoryTimer = timer
        }
        guard index.status.hasPrefix("Index ready") || Date() > deadline else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.start() }
            return
        }
        if !index.status.hasPrefix("Index ready") { failures.append("Index did not finish within 180s: " + index.status) }
        next()
    }

    func record(_ name: String, _ value: Double) {
        guard waiting else { return }
        let key = typing ? "typing" + name.prefix(1).uppercased() + name.dropFirst() : name
        if collecting { samples[key, default: []].append(value) }
        if name == "firstResultsMs" {
            waiting = false
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { self.next() }
        }
    }

    private func next() {
        if position == queries.count * 7 {
            if typing { finish(); return }
            typing = true
            position = 0
        }
        if !typing { panel.hide() }
        token += 1
        let currentToken = token
        DispatchQueue.main.asyncAfter(deadline: .now() + (typing ? 0.15 : 0.25)) {
            self.waiting = true
            self.collecting = self.position >= self.queries.count
            let query = self.queries[self.position % self.queries.count]
            if self.typing { self.panel.setBenchmarkQuery(query) }
            else { self.panel.show(query: query) }
            self.position += 1
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                guard self.waiting, self.token == currentToken else { return }
                self.failures.append("No result for " + self.queries[(self.position - 1) % self.queries.count])
                self.waiting = false
                self.next()
            }
        }
    }

    private func memory(pid: pid_t = getpid()) -> [String: UInt64] {
        var usage = rusage_info_v4()
        let result = withUnsafeMutablePointer(to: &usage) { pointer in
            pointer.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(pid, RUSAGE_INFO_V4, $0) }
        }
        guard result == 0 else { return [:] }
        return ["physical_footprint_bytes": usage.ri_phys_footprint, "peak_physical_footprint_bytes": usage.ri_lifetime_max_phys_footprint, "resident_bytes": usage.ri_resident_size]
    }

    private func sampleCombinedMemory() {
        var children = [pid_t](repeating: 0, count: 64)
        let bytes = children.withUnsafeMutableBytes { proc_listchildpids(getpid(), $0.baseAddress, Int32($0.count)) }
        let count = max(0, Int(bytes) / MemoryLayout<pid_t>.stride)
        let total = children.prefix(count).reduce(memory()["physical_footprint_bytes", default: 0]) { $0 + memory(pid: $1)["physical_footprint_bytes", default: 0] }
        peakCombinedFootprint = max(peakCombinedFootprint, total)
    }

    private func finish() {
        let active = memory()
        panel.hide()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
            self.memoryTimer?.cancel()
            self.memoryTimer = nil
            var result: [String: Any] = ["measured_at": ISO8601DateFormatter().string(from: Date()), "queries": self.queries, "warmup_queries_per_mode": self.queries.count, "index_status": self.index.status, "indexed_rows": self.index.count, "active_memory": active, "hidden_memory": self.memory(), "failures": self.failures, "samples_ms": self.samples, "method": "Native AppKit window-ready and first nonempty result update callbacks, warm resident app. Each mode has 8 warmups and 48 measured queries: hide/reopen the panel after allowing 250ms for the hide fade, then query changes inside the visible panel. The typing mode sets the field and invokes the same search path as textDidChange; it excludes OS input event delivery. Includes result cells/icons/layout, excludes GPU presentation and animation completion. Separate C harness measures search computation."]
            result["app_version"] = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
            result["app_build"] = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion")
            result["sampled_peak_app_and_children_footprint_bytes"] = self.peakCombinedFootprint
            result["memory_sample_interval_ms"] = 50
            var distributions: [String: Any] = [:]
            for (name, values) in self.samples {
                let sorted = values.sorted()
                guard !sorted.isEmpty else { continue }
                distributions[name] = ["count": sorted.count, "p50_ms": sorted[(sorted.count - 1) / 2], "p95_ms": sorted[Int(ceil(Double(sorted.count) * 0.95)) - 1], "max_ms": sorted.last!]
            }
            result["distributions"] = distributions
            do {
                let data = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
                try data.write(to: self.output, options: .atomic)
            } catch { fputs("Benchmark output failed: \(error)\n", stderr) }
            NSApp.terminate(nil)
        }
    }
}
