import AppKit

final class ResultTable: NSTableView {
    var onHover: ((Int) -> Void)?
    /// Rows past this index are placeholders kept only so result changes never add or remove row views.
    var liveRows = 0 { didSet { cancelScroll(); updateHighlight(animated: false) } }

    let selection = CALayer()
    private var highlightY = Spring(0, precision: 0.1)
    private var scrollY = Spring(0, precision: 0.1)
    private var displayLink: CADisplayLink?
    private var changingSelection = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        selection.cornerCurve = .continuous
        selection.cornerRadius = ResultRow.cornerRadius
        selection.isHidden = true
        layer?.insertSublayer(selection, at: 0)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func selectRowIndexes(_ indexes: IndexSet, byExtendingSelection extend: Bool) {
        changingSelection = true
        super.selectRowIndexes(indexes, byExtendingSelection: extend)
        changingSelection = false
        updateHighlight(animated: true)
    }

    override func deselectAll(_ sender: Any?) {
        super.deselectAll(sender)
        updateHighlight(animated: false)
    }

    override func layout() {
        super.layout()
        if !changingSelection { updateHighlight(animated: false) }
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        renderHighlight()
    }

    private func updateHighlight(animated: Bool) {
        guard selectedRow >= 0, selectedRow < liveRows, selectedRow < numberOfRows else {
            selection.isHidden = true
            return
        }
        let target = rect(ofRow: selectedRow).minY + 2
        // Layout must not snap an in-flight highlight to its endpoint.
        if animated || target != CGFloat(highlightY.target) || selection.isHidden {
            highlightY.retarget(Double(target), response: 0.20, at: CACurrentMediaTime(),
                                immediately: !animated || selection.isHidden || reduceMotion)
        }
        selection.isHidden = false
        renderHighlight()
        startClockIfNeeded()
    }

    private var reduceMotion: Bool { NSWorkspace.shared.accessibilityDisplayShouldReduceMotion }

    private func renderHighlight() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        selection.frame = NSRect(x: ResultRow.horizontalInset, y: highlightY.value,
                                 width: max(0, bounds.width - ResultRow.horizontalInset * 2), height: rowHeight - 4)
        effectiveAppearance.performAsCurrentDrawingAppearance {
            selection.backgroundColor = NSColor.labelColor.withAlphaComponent(NSWorkspace.shared.accessibilityDisplayShouldIncreaseContrast ? 0.22 : 0.10).cgColor
        }
        CATransaction.commit()
    }

    override func scrollRowToVisible(_ row: Int) {
        guard row >= 0, row < liveRows, let clip = enclosingScrollView?.contentView else { return }
        let rect = rect(ofRow: row)
        let origin = scrollY.isRunning ? CGFloat(scrollY.target) : clip.bounds.minY
        let target = rect.minY < origin ? rect.minY : max(origin, rect.maxY - clip.bounds.height)
        scroll(to: target)
    }

    func scroll(by delta: CGFloat) {
        guard let clip = enclosingScrollView?.contentView else { return }
        scroll(to: (scrollY.isRunning ? CGFloat(scrollY.target) : clip.bounds.minY) + delta)
    }

    private func scroll(to target: CGFloat) {
        guard let clip = enclosingScrollView?.contentView else { return }
        if !scrollY.isRunning { scrollY.retarget(Double(clip.bounds.minY), response: 0, at: CACurrentMediaTime(), immediately: true) }
        let limit = max(0, CGFloat(liveRows) * (rowHeight + intercellSpacing.height) - clip.bounds.height)
        scrollY.retarget(Double(min(max(target, 0), limit)), response: 0.22, at: CACurrentMediaTime(), immediately: reduceMotion)
        if reduceMotion { applyScroll() }
        startClockIfNeeded()
    }

    func cancelScroll() {
        scrollY.retarget(Double(enclosingScrollView?.contentView.bounds.minY ?? 0), response: 0, at: CACurrentMediaTime(), immediately: true)
    }

    private func applyScroll() {
        guard let scroll = enclosingScrollView else { return }
        let clip = scroll.contentView
        let proposed = NSRect(origin: NSPoint(x: clip.bounds.minX, y: scrollY.value), size: clip.bounds.size)
        clip.scroll(to: clip.constrainBoundsRect(proposed).origin)
        scroll.reflectScrolledClipView(clip)
    }

    fileprivate func tick(_ link: CADisplayLink) {
        highlightY.advance(to: link.targetTimestamp)
        let scrolling = scrollY.isRunning
        scrollY.advance(to: link.targetTimestamp)
        if scrolling { applyScroll() }
        renderHighlight()
        if !highlightY.isRunning && !scrollY.isRunning { link.isPaused = true }
    }

    private func startClockIfNeeded() {
        guard highlightY.isRunning || scrollY.isRunning else { return }
        if let displayLink { displayLink.isPaused = false; return }
        let link = displayLink(target: ResultMotionTarget(self), selector: #selector(ResultMotionTarget.tick(_:)))
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { displayLink?.invalidate(); displayLink = nil; cancelScroll() }
    }

    deinit { displayLink?.invalidate() }

    override func accessibilityRows() -> [any NSAccessibilityRow]? { super.accessibilityRows()?.filter(isLive) }
    override func accessibilityVisibleRows() -> [any NSAccessibilityRow]? { super.accessibilityVisibleRows()?.filter(isLive) }
    override func accessibilityRowCount() -> Int { liveRows }
    private func isLive(_ element: any NSAccessibilityRow) -> Bool { (element as? NSView).map { row(for: $0) < liveRows } ?? true }
    private var hoverArea: NSTrackingArea?
    private var lastMouse = NSPoint(x: CGFloat.nan, y: CGFloat.nan)

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        guard hoverArea == nil else { return }
        let area = NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self)
        addTrackingArea(area)
        hoverArea = area
    }

    // Rows sliding under a stationary pointer must not steal the keyboard selection, so only real movement counts.
    override func mouseMoved(with event: NSEvent) {
        super.mouseMoved(with: event)
        let mouse = NSEvent.mouseLocation
        guard mouse != lastMouse else { return }
        lastMouse = mouse
        let row = row(at: convert(event.locationInWindow, from: nil))
        if row >= 0, row < liveRows { onHover?(row) }
    }
}


private final class ResultMotionTarget: NSObject {
    weak var table: ResultTable?
    init(_ table: ResultTable) { self.table = table }
    @objc func tick(_ link: CADisplayLink) { table?.tick(link) }
}

final class ResultScrollView: NSScrollView {
    override func scrollWheel(with event: NSEvent) {
        guard let table = documentView as? ResultTable else { super.scrollWheel(with: event); return }
        if event.hasPreciseScrollingDeltas || NSWorkspace.shared.accessibilityDisplayShouldReduceMotion {
            table.cancelScroll()
            super.scrollWheel(with: event)
        } else {
            table.scroll(by: -event.scrollingDeltaY * verticalLineScroll)
        }
    }
}
