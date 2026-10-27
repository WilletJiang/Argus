import AppKit

final class SearchField: NSSearchField {
    weak var caretAlignmentView: NSView?
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        cell = SearchFieldCell(textCell: "")
        isEditable = true
        isSelectable = true
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func becomeFirstResponder() -> Bool {
        let result = super.becomeFirstResponder()
        if let editor = currentEditor() { editor.selectedRange = NSRange(location: (editor.string as NSString).length, length: 0) }
        return result
    }
}

private final class SearchFieldCell: NSSearchFieldCell {
    // Keep AppKit's editing, IME and selection; only the caret is drawn separately.
    private let editor: SearchFieldEditor = {
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        let container = NSTextContainer(containerSize: NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude))
        storage.addLayoutManager(layout)
        layout.addTextContainer(container)
        let editor = SearchFieldEditor(frame: .zero, textContainer: container)
        editor.isFieldEditor = true
        editor.drawsBackground = false
        return editor
    }()

    override func fieldEditor(for controlView: NSView) -> NSTextView? {
        editor.searchField = controlView as? SearchField
        return editor
    }
}

final class SearchFieldEditor: NSTextView {
    weak var searchField: SearchField?
    let caret = SearchCaretView()
    private var blinkTimer: Timer?

    override init(frame frameRect: NSRect, textContainer container: NSTextContainer?) {
        super.init(frame: frameRect, textContainer: container)
        insertionPointColor = .clear
        let notifications = NotificationCenter.default
        notifications.addObserver(self, selector: #selector(resetCaret), name: NSTextView.didChangeSelectionNotification, object: self)
        notifications.addObserver(self, selector: #selector(windowFocusChanged), name: NSWindow.didBecomeKeyNotification, object: nil)
        notifications.addObserver(self, selector: #selector(windowFocusChanged), name: NSWindow.didResignKeyNotification, object: nil)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    // AppKit otherwise draws a second, one-pixel caret with its own blink cycle.
    override var shouldDrawInsertionPoint: Bool { false }
    override func drawInsertionPoint(in rect: NSRect, color: NSColor, turnedOn flag: Bool) {}

    override func becomeFirstResponder() -> Bool {
        let accepted = super.becomeFirstResponder()
        if accepted { DispatchQueue.main.async { [weak self] in self?.resetCaret() } }
        return accepted
    }

    override func resignFirstResponder() -> Bool {
        let accepted = super.resignFirstResponder()
        if accepted { stopBlinking() }
        return accepted
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { stopBlinking(); caret.removeFromSuperview() }
    }

    override func didChangeText() {
        super.didChangeText()
        resetCaret()
    }

    override func layout() {
        super.layout()
        if canShowCaret { positionCaret() }
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        caret.needsDisplay = true
    }

    private var canShowCaret: Bool {
        window?.isVisible == true && window?.isKeyWindow == true && window?.firstResponder === self && selectedRange.length == 0
    }

    @objc private func windowFocusChanged(_ notification: Notification) {
        if notification.object as? NSWindow === window { resetCaret() }
    }

    @objc func resetCaret() {
        let wasEditing = blinkTimer != nil
        stopBlinking()
        guard canShowCaret else { return }
        positionCaret(animated: wasEditing)
        caret.isHidden = false
        let timer = Timer(timeInterval: 0.55, repeats: true) { [weak self] _ in
            guard let self else { return }
            guard self.canShowCaret else { self.stopBlinking(); return }
            self.caret.isHidden.toggle()
        }
        RunLoop.main.add(timer, forMode: .common)
        blinkTimer = timer
    }

    private func stopBlinking() {
        blinkTimer?.invalidate()
        blinkTimer = nil
        caret.isHidden = true
    }

    private func positionCaret(animated: Bool = false) {
        guard let field = searchField, let host = field.superview, let window else { return }
        if caret.superview !== host { caret.removeFromSuperview(); host.addSubview(caret) }
        let screenRect = firstRect(forCharacterRange: NSRange(location: selectedRange.location, length: 0), actualRange: nil)
        let insertion = host.convert(window.convertFromScreen(screenRect), from: nil)
        let centerY: CGFloat
        if let alignment = field.caretAlignmentView {
            centerY = host.convert(alignment.bounds, from: alignment).midY
        } else { centerY = host.convert(field.bounds, from: field).midY }
        let height = ((font?.capHeight ?? 22) + 4).rounded()
        let scale = window.backingScaleFactor
        let x = (insertion.minX * scale).rounded() / scale - 0.5
        let stem = NSRect(x: x, y: centerY - height / 2, width: SearchCaretView.stemWidth, height: height)
        let frame = stem.insetBy(dx: -SearchCaretView.padding, dy: -SearchCaretView.padding)
        guard frame != caret.frame else { return }
        let previous = caret.layer?.presentation()?.position ?? caret.layer?.position
        let pointerMove = NSApp.currentEvent.map { [.leftMouseDown, .leftMouseDragged, .leftMouseUp].contains($0.type) } ?? false
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0
            caret.frame = frame
        }
        guard animated, !hasMarkedText(), !pointerMove, !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion,
              let layer = caret.layer, let previous, abs(previous.x - layer.position.x) > 0.5 else {
            caret.layer?.removeAnimation(forKey: "caretMovement")
            return
        }
        // Retarget from the visible position so rapid typing never jumps back to an old endpoint.
        let movement = CABasicAnimation(keyPath: "position.x")
        movement.fromValue = previous.x
        movement.toValue = layer.position.x
        movement.duration = 0.09
        movement.timingFunction = CAMediaTimingFunction(name: .easeOut)
        layer.add(movement, forKey: "caretMovement")
    }

    deinit {
        blinkTimer?.invalidate()
        caret.removeFromSuperview()
        NotificationCenter.default.removeObserver(self)
    }
}

/// A separate view makes blink-off erase the entire caret without touching the text backing store.
final class SearchCaretView: NSView {
    static let stemWidth: CGFloat = 2
    static let padding: CGFloat = 1
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        let dark = effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
        let stem = bounds.insetBy(dx: Self.padding, dy: Self.padding)
        (dark ? NSColor.white : NSColor.black).setFill()
        NSBezierPath(roundedRect: stem, xRadius: 1, yRadius: 1).fill()
    }
}
