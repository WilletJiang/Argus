import AppKit
import Darwin
import QuartzCore

// Compiled only by scripts/build.sh --benchmark.
final class AppPerformance {
    private let panel: PanelController
    private let index: FileIndex
    private let output: URL
    private let queries = ["Safari", "Terminal", "Finder", "README", "swift", "Argus", "项目", "中文"]
    private let memory = BenchmarkMemory()
    private let started = ProcessInfo.processInfo.systemUptime
    private var position = 0
    private var sequence = 0
    private var waiting = false
    private var collecting = false
    private var typing = false
    private var samples: [String: [Double]] = [:]
    private var trials: [[String: Any]] = []
    private var failures: [String] = []
    private var readyAt = 0.0
    private var eventStarted: UInt64 = 0
    private var frameDeadline = 0.0

    init(panel: PanelController, index: FileIndex, output: String) {
        self.panel = panel; self.index = index; self.output = URL(fileURLWithPath: output)
    }

    func start() {
        memory.start()
        awaitIndex()
    }

    private func awaitIndex() {
        guard index.status.hasPrefix("Index ready") else {
            if ProcessInfo.processInfo.systemUptime - started > 900 {
                failures.append("Index did not finish: " + index.status); finish(); return
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { self.awaitIndex() }
            return
        }
        readyAt = ProcessInfo.processInfo.systemUptime
        if CommandLine.arguments.contains("--benchmark-index-only") { finish(); return }
        panel.show(query: "")
        frameDeadline = ProcessInfo.processInfo.systemUptime + 30
        awaitObserver()
    }

    private func awaitObserver() {
        if CommandLine.arguments.contains("--benchmark-frames"),
           !FileManager.default.fileExists(atPath: output.path + ".frames.ready") {
            if ProcessInfo.processInfo.systemUptime > frameDeadline {
                failures.append("Frame observer did not become ready"); finish(); return
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { self.awaitObserver() }
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.next() }
    }

    func record(_ name: String, _ value: Double) {
        guard waiting else { return }
        let key = (typing ? "typing" : "reopen") + name.prefix(1).uppercased() + name.dropFirst()
        if collecting { samples[key, default: []].append(value) }
        if name == "firstResultsMs" {
            panel.panel.contentView?.displayIfNeeded()
            CATransaction.flush()
            trials.append(["sequence": sequence, "query": queries[(position - 1) % queries.count],
                           "mode": typing ? "typing" : "reopen", "measured": collecting,
                           "event_start_mach": eventStarted, "result_commit_mach": mach_absolute_time()])
            waiting = false
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { self.next() }
        }
    }

    private func next() {
        if position == queries.count * 7 {
            if typing { finish(); return }
            typing = true; position = 0
        }
        if !typing { panel.hide() }
        sequence += 1
        let current = sequence
        DispatchQueue.main.asyncAfter(deadline: .now() + (typing ? 0.15 : 0.25)) {
            self.waiting = true
            self.collecting = self.position >= self.queries.count
            let query = self.queries[self.position % self.queries.count]
            self.position += 1
            self.eventStarted = mach_absolute_time()
            if !self.typing { self.panel.show(query: "") }
            self.panel.field.selectText(nil)
            guard let editor = self.panel.field.currentEditor() as? NSTextView else {
                self.failures.append("No native field editor"); self.waiting = false; self.next(); return
            }
            // Use the real NSTextInputClient path and let AppKit emit textDidChange.
            // This works identically for ASCII and Chinese regardless of the active input method.
            editor.insertText(query, replacementRange: editor.selectedRange())
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                guard self.waiting, self.sequence == current else { return }
                self.failures.append("No result for " + query + "; field=" + self.panel.field.stringValue)
                self.waiting = false; self.next()
            }
        }
    }

    private func finish() {
        let active = BenchmarkMemory.read()
        panel.hide()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
            var timebase = mach_timebase_info_data_t()
            mach_timebase_info(&timebase)
            var result: [String: Any] = [
                "measured_at": ISO8601DateFormatter().string(from: Date()), "queries": self.queries,
                "warmup_queries_per_mode": self.queries.count, "index_status": self.index.status,
                "indexed_rows": self.index.count, "index_ready_seconds": self.readyAt - self.started,
                "active_memory": active, "hidden_memory": BenchmarkMemory.read(),
                "sampled_peak_app_and_children_footprint_bytes": self.memory.finish(),
                "memory_sample_interval_ms": 10, "failures": self.failures, "samples_ms": self.samples,
                "trials": self.trials, "mach_timebase_numer": timebase.numer, "mach_timebase_denom": timebase.denom,
                "method": "NSTextInputClient.insertText through the native field editor and AppKit textDidChange; includes editing, search, cells/icons/layout. External ScreenCaptureKit observer measures the first changed result-region window frame after the result view commit. Excludes physical keyboard/OS event delivery and monitor photon latency. Capture observer runs outside the app/worker process tree. Hidden/active memory is proc_pid_rusage physical footprint and RSS; parent lifetime peak is exact, combined app+worker peak is sampled every 10ms. Index timing starts after FileIndex initialization."
            ]
            var distributions: [String: Any] = [:]
            for (name, values) in self.samples {
                let sorted = values.sorted()
                guard !sorted.isEmpty else { continue }
                distributions[name] = ["count": sorted.count, "p50_ms": sorted[(sorted.count - 1) / 2],
                    "p95_ms": sorted[Int(ceil(Double(sorted.count) * 0.95)) - 1], "max_ms": sorted.last!]
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
