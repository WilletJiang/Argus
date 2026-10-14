import Darwin
import Foundation

// Sample off the main thread so a long directory batch cannot hide the allocation peak.
final class BenchmarkMemory {
    private let queue = DispatchQueue(label: "argus.benchmark.memory")
    private var timer: DispatchSourceTimer?
    private var peak: UInt64 = 0

    static func read(_ pid: pid_t = getpid()) -> [String: UInt64] {
        var usage = rusage_info_v4()
        let result = withUnsafeMutablePointer(to: &usage) { pointer in
            pointer.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(pid, RUSAGE_INFO_V4, $0) }
        }
        guard result == 0 else { return [:] }
        return ["physical_footprint_bytes": usage.ri_phys_footprint,
                "peak_physical_footprint_bytes": usage.ri_lifetime_max_phys_footprint,
                "resident_bytes": usage.ri_resident_size]
    }

    func start() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            var children = [pid_t](repeating: 0, count: 64)
            let count = children.withUnsafeMutableBytes { proc_listchildpids(getpid(), $0.baseAddress, Int32($0.count)) }
            let total = children.prefix(max(0, min(children.count, Int(count)))).reduce(Self.read()["physical_footprint_bytes", default: 0]) {
                $0 + Self.read($1)["physical_footprint_bytes", default: 0]
            }
            self.peak = max(self.peak, total)
        }
        timer.resume(); self.timer = timer
    }

    func finish() -> UInt64 {
        timer?.cancel(); timer = nil
        return queue.sync { peak }
    }
}
