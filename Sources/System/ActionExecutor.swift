import AppKit

final class ActionExecutor {
    static func execute(_ result: SearchResult, completion: @escaping (String?) -> Void) {
        if result.kind == .shortcut {
            run("/usr/bin/shortcuts", arguments: ["run", result.target], completion: completion)
            return
        }
        if result.target.hasPrefix("argus-action:") {
            executeSystemAction(String(result.target.dropFirst("argus-action:".count)), completion: completion)
            return
        }
        DispatchQueue.main.async {
            let url = result.target.hasPrefix("/") ? URL(fileURLWithPath: result.target) : URL(string: result.target)
            guard let url else { completion("Could not open this result: invalid address."); return }
            if NSWorkspace.shared.open(url) { completion(nil) }
            else { completion("Could not open this result. Check that the file exists and the required app is installed.") }
        }
    }

    static func reveal(_ result: SearchResult) {
        DispatchQueue.main.async {
            guard result.target.hasPrefix("/") else { return }
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: result.target)])
        }
    }

    private static func executeSystemAction(_ action: String, completion: @escaping (String?) -> Void) {
        switch action {
        case "appearance":
            run("/usr/bin/osascript", arguments: ["-e", "tell application \"System Events\" to tell appearance preferences to set dark mode to not dark mode"], completion: completion)
        case "volume-up":
            run("/usr/bin/osascript", arguments: ["-e", "set newVolume to (output volume of (get volume settings)) + 10", "-e", "if newVolume > 100 then set newVolume to 100", "-e", "set volume output volume newVolume"], completion: completion)
        case "volume-down":
            run("/usr/bin/osascript", arguments: ["-e", "set newVolume to (output volume of (get volume settings)) - 10", "-e", "if newVolume < 0 then set newVolume to 0", "-e", "set volume output volume newVolume"], completion: completion)
        case "mute":
            run("/usr/bin/osascript", arguments: ["-e", "set volume output muted (not (output muted of (get volume settings)))"], completion: completion)
        case "display-sleep":
            run("/usr/bin/pmset", arguments: ["displaysleepnow"], completion: completion)
        case "sleep":
            run("/usr/bin/pmset", arguments: ["sleepnow"], completion: completion)
        default:
            DispatchQueue.main.async { completion("This system action is not supported.") }
        }
    }

    private static func run(_ executable: String, arguments: [String], completion: @escaping (String?) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let process = Process()
            let errors = Pipe()
            process.executableURL = URL(fileURLWithPath: executable)
            process.arguments = arguments
            process.standardOutput = FileHandle.nullDevice
            process.standardError = errors
            do {
                try process.run()
                var data = Data()
                while let chunk = try errors.fileHandleForReading.read(upToCount: 65536), !chunk.isEmpty {
                    data.append(chunk.prefix(max(0, 1500 - data.count)))
                }
                process.waitUntilExit()
                let message: String?
                if process.terminationStatus == 0 { message = nil }
                else {
                    let detail = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
                    message = detail.isEmpty ? "Action failed (error code \(process.terminationStatus))." : detail
                }
                DispatchQueue.main.async { completion(message) }
            } catch {
                DispatchQueue.main.async { completion("Action failed: \(error.localizedDescription)") }
            }
        }
    }
}
