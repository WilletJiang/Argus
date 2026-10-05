import AppKit
import QuartzCore

struct FadeTrack {
    private(set) var value: CGFloat = 0
    private(set) var target: CGFloat = 0
    private var origin: CGFloat = 0
    private var started = 0.0
    private var duration = 0.0
    private(set) var isRunning = false

    mutating func retarget(_ target: CGFloat, at time: Double, immediately: Bool) {
        advance(to: time)
        guard self.target != target || immediately else { return }
        self.target = target
        origin = value
        started = time
        duration = 0.18 * Double(abs(target - value))
        isRunning = !immediately && duration > 0.001
        if !isRunning { value = target }
    }

    mutating func advance(to time: Double) {
        guard isRunning else { return }
        let progress = min(max((time - started) / duration, 0), 1)
        let eased = progress * progress * (3 - 2 * progress)
        value = origin + (target - origin) * CGFloat(eased)
        if progress == 1 { value = target; isRunning = false }
    }
}

final class PanelMotion: NSObject {
    private let surface: SearchSurface
    private var windowFade = FadeTrack()
    private var resultsFade = FadeTrack()
    private var displayLink: CADisplayLink?
    var isPresented: Bool { windowFade.target == 1 }

    init(surface: SearchSurface) { self.surface = surface; super.init() }

    func present(header: Bool, results: Bool, immediately: Bool = false) {
        let now = CACurrentMediaTime()
        let immediate = immediately || NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        let window = surface.panel
        let opening = header && !window.isVisible
        windowFade.retarget(header ? 1 : 0, at: now, immediately: immediate)
        if header {
            resultsFade.retarget(results ? 1 : 0, at: now, immediately: immediate || opening)
            if opening {
                surface.setExpansion(resultsFade.value)
                window.alphaValue = windowFade.value
            }
        }
        apply(at: now)
        if windowFade.isRunning || resultsFade.isRunning {
            if displayLink == nil {
                let link = window.displayLink(target: self, selector: #selector(tick(_:)))
                link.add(to: .main, forMode: .common)
                displayLink = link
            }
        } else { stopClock() }
    }

    @objc private func tick(_ link: CADisplayLink) {
        let now = CACurrentMediaTime()
        windowFade.advance(to: now)
        resultsFade.advance(to: now)
        apply(at: now)
        if !windowFade.isRunning && !resultsFade.isRunning { stopClock() }
    }

    private func apply(at time: Double) {
        surface.panel.alphaValue = windowFade.value
        surface.setExpansion(resultsFade.value)
        if !windowFade.isRunning && windowFade.target == 0 {
            surface.panel.orderOut(nil)
            resultsFade.retarget(0, at: time, immediately: true)
            surface.setExpansion(0)
        }
    }

    private func stopClock() { displayLink?.invalidate(); displayLink = nil }
    deinit { stopClock() }
}
