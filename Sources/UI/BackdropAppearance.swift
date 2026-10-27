import AppKit
import ScreenCaptureKit
import OSLog

/// Samples the current desktop once while the search window is still transparent.
/// No persistent capture stream, application exclusion filter, or cross-Space cache is needed.
final class BackdropAppearance {
    typealias Capture = (CGRect, @escaping (CGImage?, Error?) -> Void) -> Void
    private static let log = Logger(subsystem: "dev.argus.search", category: "appearance")
    private weak var panel: NSPanel?
    private weak var shape: NSView?
    private let capture: Capture
    private let canCapture: () -> Bool
    private let timeout: TimeInterval
    private var generation = 0
    private var deadline: DispatchWorkItem?
    private var completion: (() -> Void)?
    private var observer: NSObjectProtocol?

    init(panel: NSPanel, shape: NSView, capture: Capture? = nil,
         canCapture: @escaping () -> Bool = CGPreflightScreenCaptureAccess, timeout: TimeInterval = 0.35) {
        self.panel = panel
        self.shape = shape
        self.capture = capture ?? { rect, completion in SCScreenshotManager.captureImage(in: rect, completionHandler: completion) }
        self.canCapture = canCapture
        self.timeout = timeout
        observer = NotificationCenter.default.addObserver(forName: AppearanceMode.didChange, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.panel?.appearance = nil
            self.finish(nil, generation: self.generation)
        }
    }

    func prepare(completion: @escaping () -> Void) {
        cancel()
        self.completion = completion
        let token = generation
        guard Preferences.shared.appearanceMode == .adaptive, canCapture(), let panel, let shape,
              let desktopTop = NSScreen.screens.first?.frame.maxY else {
            panel?.appearance = nil
            finish(nil, generation: token)
            return
        }
        let band = NSRect(x: 24, y: 12, width: shape.bounds.width - 48, height: SearchSurface.inputHeight - 24)
        let screenRect = panel.convertToScreen(shape.convert(band, to: nil))
        let rect = Self.captureRect(screenRect: screenRect, desktopTop: desktopTop)
        guard !rect.isEmpty else { finish(nil, generation: token); return }
        let deadline = DispatchWorkItem { [weak self] in
            guard let self, self.generation == token else { return }
            Self.log.error("Background snapshot timed out; showing search with the previous appearance")
            self.finish(nil, generation: token)
        }
        self.deadline = deadline
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout, execute: deadline)
        capture(rect) { [weak self] image, error in
            DispatchQueue.main.async {
                guard let self, self.generation == token else { return }
                if let error { Self.log.error("Background snapshot failed: \(error.localizedDescription, privacy: .public)") }
                self.finish(image, generation: token)
            }
        }
    }

    private func finish(_ image: CGImage?, generation token: Int) {
        guard generation == token, let completion else { return }
        self.completion = nil
        deadline?.cancel(); deadline = nil
        generation += 1
        if let image, let brightness = Self.brightness(of: image), Preferences.shared.appearanceMode == .adaptive {
            let next = brightness < 0.5
            panel?.appearance = NSAppearance(named: next ? .darkAqua : .aqua)
            Self.log.debug("Background brightness: \(brightness, privacy: .public); dark appearance: \(next, privacy: .public)")
        }
        completion()
    }

    func cancel() {
        generation += 1
        deadline?.cancel(); deadline = nil
        completion = nil
    }

    func invalidate() {
        cancel()
        panel?.appearance = nil
    }

    static func captureRect(screenRect: CGRect, desktopTop: CGFloat) -> CGRect {
        CGRect(x: screenRect.minX, y: desktopTop - screenRect.maxY, width: screenRect.width, height: screenRect.height)
    }

    static func brightness(of image: CGImage) -> Double? {
        let width = 48, height = 8
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let rendered = pixels.withUnsafeMutableBytes { bytes -> Bool in
            guard let context = CGContext(data: bytes.baseAddress, width: width, height: height,
                                          bitsPerComponent: 8, bytesPerRow: width * 4,
                                          space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            context.interpolationQuality = .low
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        guard rendered else { return nil }
        var values: [Double] = []
        values.reserveCapacity(width * height)
        for offset in stride(from: 0, to: pixels.count, by: 4) where pixels[offset + 3] > 240 {
            values.append((0.2126 * Double(pixels[offset]) + 0.7152 * Double(pixels[offset + 1]) + 0.0722 * Double(pixels[offset + 2])) / 255)
        }
        guard !values.isEmpty else { return nil }
        values.sort()
        return values[values.count / 2]
    }

    deinit {
        deadline?.cancel()
        if let observer { NotificationCenter.default.removeObserver(observer) }
    }
}
