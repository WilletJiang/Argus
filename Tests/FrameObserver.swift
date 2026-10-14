import AppKit
import CoreMedia
import CoreVideo
import ScreenCaptureKit

// Observe only the benchmark PID's window. Pixels are inspected in memory and never saved.
final class FrameObserver: NSObject, SCStreamOutput {
    let file: FileHandle
    init(path: String) throws {
        FileManager.default.createFile(atPath: path, contents: nil)
        file = try FileHandle(forWritingTo: URL(fileURLWithPath: path))
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let info = attachments.first,
              let timestamp = info[.displayTime] as? NSNumber,
              let pixels = CMSampleBufferGetImageBuffer(sample) else { return }
        CVPixelBufferLockBaseAddress(pixels, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixels, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pixels) else { return }
        let width = CVPixelBufferGetWidth(pixels), height = CVPixelBufferGetHeight(pixels)
        let rowStride = CVPixelBufferGetBytesPerRow(pixels)
        guard width >= 610, height >= 60 else { return }
        let bytes = base.assumingMemoryBound(to: UInt8.self)
        // Fingerprint only the result area; query field changes cannot satisfy this observation.
        var hash: UInt64 = 1469598103934665603
        for y in stride(from: 85, to: min(height, 135), by: 2) {
            for x in stride(from: 18, to: 600, by: 2) {
                let offset = y * rowStride + x * 4
                for channel in 0..<3 { hash = (hash ^ UInt64(bytes[offset + channel])) &* 1099511628211 }
            }
        }
        let value: [String: Any] = ["display_mach": timestamp.uint64Value, "result_region_hash": hash]
        if var data = try? JSONSerialization.data(withJSONObject: value) {
            data.append(10); try? file.write(contentsOf: data)
        }
    }
}

@main
enum ObserveFrames {
    @MainActor static func main() async throws {
        _ = NSApplication.shared
        let args = CommandLine.arguments
        guard args.count == 3, let pid = Int32(args[1]), CGPreflightScreenCaptureAccess() else {
            fputs("Usage: frame-observer PID OUTPUT; existing screen capture permission required\n", stderr); exit(1)
        }
        var window: SCWindow?
        for _ in 0..<100 {
            let content = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: false)
            window = content.windows.first { $0.owningApplication?.processID == pid && $0.frame.width >= 610 && $0.frame.width <= 630 }
            if window != nil { break }
            try await Task.sleep(for: .milliseconds(100))
        }
        guard let window else { fputs("No benchmark window\n", stderr); exit(1) }
        let filter = SCContentFilter(desktopIndependentWindow: window)
        let config = SCStreamConfiguration()
        config.width = 620; config.height = Int(window.frame.height)
        config.pixelFormat = kCVPixelFormatType_32BGRA
        config.minimumFrameInterval = CMTime(value: 1, timescale: 120)
        config.queueDepth = 3; config.showsCursor = false; config.ignoreShadowsSingleWindow = true
        config.colorSpaceName = CGColorSpace.sRGB
        let observer = try FrameObserver(path: args[2])
        let stream = SCStream(filter: filter, configuration: config, delegate: nil)
        try stream.addStreamOutput(observer, type: .screen, sampleHandlerQueue: DispatchQueue(label: "argus.frame.observer"))
        try await stream.startCapture()
        try Data().write(to: URL(fileURLWithPath: args[2] + ".ready"))
        while kill(pid, 0) == 0 { try await Task.sleep(for: .milliseconds(200)) }
        // The window disappearing on app exit can stop the stream first.
        try? await stream.stopCapture()
        try observer.file.close()
    }
}
