import AppKit
import QuartzCore

/// Critically damped spring evaluated in closed form, so frame timing never changes the curve
/// and retargeting mid-flight carries the current velocity into the new motion.
struct Spring {
    private(set) var value: Double
    private(set) var target: Double
    private(set) var velocity = 0.0
    private(set) var isRunning = false
    private let precision: Double
    private var displacement = 0.0
    private var initialVelocity = 0.0
    private var omega = 0.0
    private var started = 0.0
    /// Frames are evaluated at their future display time, so later calls may carry an earlier clock.
    private var clock = -Double.infinity

    init(_ value: Double, precision: Double) {
        self.value = value
        target = value
        self.precision = precision
    }

    mutating func retarget(_ newTarget: Double, response: Double, at time: Double, immediately: Bool = false) {
        advance(to: time)
        let time = max(time, clock)
        if immediately || response <= 0 {
            value = newTarget; target = newTarget; velocity = 0; isRunning = false
            return
        }
        guard newTarget != target else { return }
        target = newTarget
        displacement = value - newTarget
        initialVelocity = velocity
        omega = 2 * .pi / response
        started = time
        isRunning = true
        settleIfNeeded()
    }

    mutating func advance(to time: Double) {
        clock = max(clock, time)
        guard isRunning else { return }
        let t = max(0, clock - started)
        let decay = exp(-omega * t)
        let b = initialVelocity + omega * displacement
        value = target + (displacement + b * t) * decay
        velocity = (initialVelocity - omega * b * t) * decay
        settleIfNeeded()
    }

    private mutating func settleIfNeeded() {
        guard abs(value - target) < precision && abs(velocity) < precision * omega else { return }
        value = target; velocity = 0; isRunning = false
    }
}

final class PanelMotion: NSObject {
    static let presenceResponse = (show: 0.24, hide: 0.16)
    static let heightResponse = 0.3
    static let hiddenScale = 0.97
    private let surface: SearchSurface
    private var presence = Spring(0, precision: 0.002)
    private var height = Spring(Double(SearchSurface.inputHeight), precision: 0.2)
    private var displayLink: CADisplayLink?
    private var scales = true
    var isPresented: Bool { presence.target == 1 }
    var heightTarget: CGFloat { CGFloat(height.target) }

    init(surface: SearchSurface) { self.surface = surface; super.init() }

    func present(_ presented: Bool, height target: CGFloat, immediately: Bool = false) {
        let window = surface.panel
        let opening = presented && !window.isVisible
        let presenceTarget: Double = presented ? 1 : 0
        if !opening && !immediately && presence.target == presenceTarget && (!presented || height.target == Double(target)) { return }
        let now = CACurrentMediaTime()
        let reduceMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        if opening || immediately { scales = !reduceMotion }
        let immediate = immediately || reduceMotion
        presence.retarget(presenceTarget, response: presented ? Self.presenceResponse.show : Self.presenceResponse.hide, at: now, immediately: immediate)
        if presented { height.retarget(Double(target), response: Self.heightResponse, at: now, immediately: immediate || opening) }
        // While the clock runs the next frame applies the new targets; applying here too would lay out twice per frame.
        if immediate || opening || !(presence.isRunning || height.isRunning) { apply() }
        if presence.isRunning || height.isRunning { startClock() } else { stopClock() }
    }

    fileprivate func tick(_ link: CADisplayLink) {
        // Evaluate at the time the frame reaches the display, not when the callback runs.
        let time = link.targetTimestamp
        presence.advance(to: time)
        height.advance(to: time)
        apply()
        if !presence.isRunning && !height.isRunning { stopClock() }
    }

    private func apply() {
        if !presence.isRunning && presence.target == 0 {
            if surface.panel.isVisible { surface.panel.orderOut(nil) }
            height.retarget(Double(SearchSurface.inputHeight), response: 0, at: 0, immediately: true)
            _ = surface.setHeight(SearchSurface.inputHeight)
            surface.setPresence(0, scale: 1)
            return
        }
        let progress = CGFloat(presence.value)
        let resized = surface.setHeight(CGFloat(height.value))
        let rescaled = surface.setPresence(progress, scale: scales ? CGFloat(Self.hiddenScale + (1 - Self.hiddenScale) * presence.value) : 1)
        if resized || rescaled { surface.panel.invalidateShadow() }
    }

    /// The link is created once and paused between animations; creating one costs far more than resuming it.
    private func startClock() {
        if let displayLink { displayLink.isPaused = false; return }
        let link = surface.panel.displayLink(target: DisplayLinkProxy(self), selector: #selector(DisplayLinkProxy.tick(_:)))
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    private func stopClock() { displayLink?.isPaused = true }
    deinit { displayLink?.invalidate() }
}

/// CADisplayLink retains its target; the proxy keeps that from retaining the motion controller.
private final class DisplayLinkProxy: NSObject {
    private weak var motion: PanelMotion?
    init(_ motion: PanelMotion) { self.motion = motion }
    @objc func tick(_ link: CADisplayLink) { motion?.tick(link) }
}
