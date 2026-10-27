import AppKit

/// The original contrast gradient, with a short transition when the chosen appearance changes.
final class GlassContentView: NSView {
    override var isFlipped: Bool { true }
    private var tint = Spring(1, precision: 1.0 / 255)
    private var dark: Bool?
    private var displayLink: CADisplayLink?

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateTint(immediately: false)
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        finishAppearanceTransition()
    }

    func finishAppearanceTransition() { updateTint(immediately: true) }

    private func updateTint(immediately: Bool) {
        let next = effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
        let snap = immediately || dark == nil || window?.isVisible != true || NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        guard next != dark || snap else { return }
        dark = next
        tint.retarget(next ? 0 : 1, response: 0.14, at: CACurrentMediaTime(), immediately: snap)
        needsDisplay = true
        if tint.isRunning, let window {
            if let displayLink { displayLink.isPaused = false }
            else {
                let link = window.displayLink(target: TintDisplayLinkTarget(self), selector: #selector(TintDisplayLinkTarget.tick(_:)))
                link.add(to: .main, forMode: .common)
                displayLink = link
            }
        } else { displayLink?.isPaused = true }
    }

    fileprivate func advanceTint(_ link: CADisplayLink) {
        guard window?.isVisible == true else { finishAppearanceTransition(); return }
        tint.advance(to: link.targetTimestamp)
        needsDisplay = true
        if !tint.isRunning { displayLink?.isPaused = true }
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let color = NSColor(white: CGFloat(min(max(tint.value, 0), 1)), alpha: 1)
        let gradient = NSGradient(colorsAndLocations: (color.withAlphaComponent(0.35), 0),
                                  (color.withAlphaComponent(0.42), 0.40),
                                  (color.withAlphaComponent(0.50), 1))!
        gradient.draw(from: NSPoint(x: bounds.midX, y: 0), to: NSPoint(x: bounds.midX, y: bounds.height),
                      options: [.drawsBeforeStartingLocation, .drawsAfterEndingLocation])
    }

    deinit { displayLink?.invalidate() }
}

private final class TintDisplayLinkTarget: NSObject {
    private weak var view: GlassContentView?
    init(_ view: GlassContentView) { self.view = view }
    @objc func tick(_ link: CADisplayLink) { view?.advanceTint(link) }
}
